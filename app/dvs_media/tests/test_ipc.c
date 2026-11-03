/* Integration-style unit test for the dvs_media IPC server.
 * Drives the socket protocol end-to-end: messageId echo, async prepare,
 * event push, revision validation. */
#include "../ipc/ipc_server.h"
#include "../ipc/state_machine.h"

#include <json-c/json.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;
static const char* SOCK = "/tmp/dvs_test_ipc.sock";

#define CHECK(cond, name)                                                     \
  do {                                                                        \
    if (cond) {                                                               \
      printf("PASS: %s\n", name);                                             \
    } else {                                                                  \
      printf("FAIL: %s\n", name);                                             \
      failures++;                                                             \
    }                                                                         \
  } while (0)

static int conn_fd = -1;

static int connect_sock(void) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, SOCK, sizeof(addr.sun_path) - 1);
  if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

/* reads one JSON line with timeout; returns parsed object or NULL */
static struct json_object* read_line(int fd, int timeout_ms) {
  static char buf[8192];
  static size_t used = 0;
  for (;;) {
    char* nl = memchr(buf, '\n', used);
    if (nl) {
      *nl = '\0';
      struct json_object* o = json_tokener_parse(buf);
      size_t consumed = (size_t)(nl - buf) + 1;
      memmove(buf, nl + 1, used - consumed);
      used -= consumed;
      return o;
    }
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    int pr = poll(&pfd, 1, timeout_ms);
    if (pr <= 0) return NULL;
    ssize_t n = read(fd, buf + used, sizeof(buf) - 1 - used);
    if (n <= 0) return NULL;
    used += (size_t)n;
  }
}

static struct json_object* request(const char* type, const char* msg_id,
                                   const char* extra_json) {
  char line[2048];
  if (extra_json)
    snprintf(line, sizeof(line), "{\"type\":\"%s\",\"messageId\":\"%s\",%s", type,
             msg_id, extra_json);
  else
    snprintf(line, sizeof(line), "{\"type\":\"%s\",\"messageId\":\"%s\"}", type,
             msg_id);
  /* note: extra_json must include its own trailing '}' */
  (void)write(conn_fd, line, strlen(line));
  (void)write(conn_fd, "\n", 1);
  return read_line(conn_fd, 3000);
}

static const char* jstr(struct json_object* o, const char* k) {
  struct json_object* v = NULL;
  if (o && json_object_object_get_ex(o, k, &v) && v)
    return json_object_get_string(v);
  return NULL;
}

static int jint(struct json_object* o, const char* k) {
  struct json_object* v = NULL;
  if (o && json_object_object_get_ex(o, k, &v) && v)
    return json_object_get_int(v);
  return -1;
}

static void* server_thread(void* arg) {
  (void)arg;
  dvs_ipc_server_run(SOCK);
  return NULL;
}

int main(void) {
  unlink(SOCK);
  dvs_ipc_set_prepare_delay_ms(100);

  pthread_t tid;
  pthread_create(&tid, NULL, server_thread, NULL);
  usleep(100 * 1000); /* let the server bind */

  conn_fd = connect_sock();
  CHECK(conn_fd >= 0, "client connected");

  /* 1. messageId echo */
  struct json_object* r = request("query_state", "m-1", NULL);
  CHECK(r && jstr(r, "requestId") && strcmp(jstr(r, "requestId"), "m-1") == 0,
        "response echoes messageId");
  json_object_put(r);

  /* 2. prepare is async: accepted then event */
  r = request("prepare", "m-2", "\"operationId\":\"op1\",\"revision\":1}");
  CHECK(r && jint(r, "status") == 202, "prepare returns 202 accepted");
  json_object_put(r);

  struct json_object* ev = read_line(conn_fd, 3000);
  const char* ev_type = ev ? jstr(ev, "type") : NULL;
  CHECK(ev && ev_type && strcmp(ev_type, "media.prepare.result") == 0,
        "prepare result event pushed");
  json_object_put(ev);
  ev = read_line(conn_fd, 500);
  if (ev) json_object_put(ev); /* consume duplicate event line if any */

  /* 3. commit */
  r = request("commit", "m-3", "\"operationId\":\"op1\",\"revision\":1}");
  CHECK(r && jint(r, "status") == 200, "commit ok");
  json_object_put(r);

  ev = read_line(conn_fd, 2000);
  CHECK(ev && jstr(ev, "type") && strcmp(jstr(ev, "type"), "media.layout_applied") == 0,
        "layout applied event broadcast");
  json_object_put(ev);

  /* 4. stale revision rejected */
  r = request("prepare", "m-4", "\"operationId\":\"op2\",\"revision\":0}");
  CHECK(r && jstr(r, "code") && strcmp(jstr(r, "code"), "STALE_REVISION") == 0,
        "stale revision rejected");
  json_object_put(r);

  /* 5. commit without prepare rejected */
  r = request("commit", "m-5", "\"operationId\":\"op9\",\"revision\":5}");
  CHECK(r && jstr(r, "code") && strcmp(jstr(r, "code"), "INVALID_STATE") == 0,
        "commit without prepare rejected");
  json_object_put(r);

  /* 6. clear idempotent + unknown type */
  r = request("clear", "m-6", NULL);
  CHECK(r && jint(r, "status") == 200, "clear ok");
  json_object_put(r);
  r = request("bogus", "m-7", NULL);
  CHECK(r && jint(r, "status") == 400, "unknown type rejected");
  json_object_put(r);

  /* 7. query_state revision persisted */
  r = request("query_state", "m-8", NULL);
  struct json_object* data = NULL;
  if (r) json_object_object_get_ex(r, "data", &data);
  CHECK(data && jint(data, "revision") == 1, "revision persisted after commit");
  json_object_put(r);

  close(conn_fd);
  /* server runs until killed; test process exits regardless */
  printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
  return failures ? 1 : 0;
}
