/* Integration-style unit test for the dvs_media IPC server.
 * Drives the socket protocol end-to-end: requestId echo, async prepare,
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
  static char buf[262144]; /* must hold the biggest query_state envelope */
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

static struct json_object* request_on(int fd, const char* type, const char* msg_id,
                                      const char* extra_json) {
  char line[2048];
  if (extra_json)
    snprintf(line, sizeof(line), "{\"type\":\"%s\",\"requestId\":\"%s\",%s", type,
             msg_id, extra_json);
  else
    snprintf(line, sizeof(line), "{\"type\":\"%s\",\"requestId\":\"%s\"}", type,
             msg_id);
  /* note: extra_json must include its own trailing '}' */
  (void)write(fd, line, strlen(line));
  (void)write(fd, "\n", 1);
  return read_line(fd, 3000);
}

static struct json_object* request(const char* type, const char* msg_id,
                                   const char* extra_json) {
  return request_on(conn_fd, type, msg_id, extra_json);
}

static struct json_object* request_payload(const char* type, const char* msg_id,
                                           const char* payload_json) {
  char line[2048];
  snprintf(line, sizeof(line),
           "{\"type\":\"%s\",\"requestId\":\"%s\",\"payload\":%s}", type, msg_id,
           payload_json);
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

static void drain_pending(int fd) {
  for (;;) {
    struct json_object* o = read_line(fd, 200);
    if (!o) break;
    json_object_put(o);
  }
}

static void* server_thread(void* arg) {
  (void)arg;
  dvs_ipc_server_run(SOCK);
  return NULL;
}

/* lane_stats hook that returns a large array so the query_state envelope
 * exceeds the historical fixed 8192-byte buffer (P15 regression guard). */
static struct json_object* big_lane_stats(void) {
  json_object* arr = json_object_new_array();
  for (int i = 0; i < 300; i++) {
    json_object* lane = json_object_new_object();
    char sig[32];
    snprintf(sig, sizeof(sig), "sig-lane-%03d-long-identifier", i);
    json_object_object_add(lane, "sig", json_object_new_string(sig));
    json_object_object_add(lane, "state", json_object_new_string("ready"));
    json_object_object_add(lane, "packetLoss", json_object_new_int(i % 7));
    json_object_object_add(lane, "latencyUs", json_object_new_int(1000 + i));
    json_object_object_add(lane, "decoder", json_object_new_string("mpp-h264"));
    json_object_object_add(lane, "renderX", json_object_new_int(i * 16));
    json_object_object_add(lane, "renderY", json_object_new_int(i * 9));
    json_object_object_add(lane, "renderW", json_object_new_int(640));
    json_object_object_add(lane, "renderH", json_object_new_int(360));
    json_object_array_add(arr, lane);
  }
  return arr;
}

/* Prepare gate mirroring the engine bridge (engine/ipc_engine_bridge.c
 * hook_prepare) which cannot link into this no-MTL test target: pure
 * operation frames and streams/layout shapes pass; the platform
 * windows/geometry shape is refused so a pre-translation agent can never
 * stage nothing and fake success on commit. */
static int test_prepare_gate(struct json_object* req, char* err, size_t errlen) {
  struct json_object* t = NULL;
  if (req && (json_object_object_get_ex(req, "streams", &t) ||
               json_object_object_get_ex(req, "layout", &t)))
    return 0;
  if (req && json_object_object_get_ex(req, "windows", &t) && t) {
    snprintf(err, errlen,
             "unsupported layout shape: agent must translate windows to streams");
    return -1;
  }
  return 0;
}

int main(void) {
  unlink(SOCK);
  dvs_ipc_set_prepare_delay_ms(100);
  /* Query + prepare-gate hooks: commit stays NULL so the server keeps its
   * pure state-machine mode for the flow tests below. */
  {
    dvs_ipc_engine_hooks_t hooks;
    memset(&hooks, 0, sizeof(hooks));
    hooks.prepare = test_prepare_gate;
    hooks.lane_stats = big_lane_stats;
    dvs_ipc_install_hooks(&hooks);
  }

  pthread_t tid;
  pthread_create(&tid, NULL, server_thread, NULL);
  usleep(100 * 1000); /* let the server bind */

  conn_fd = connect_sock();
  CHECK(conn_fd >= 0, "client connected");

  /* 1. requestId echo */
  struct json_object* r = request("query_state", "m-1", NULL);
  CHECK(r && jstr(r, "requestId") && strcmp(jstr(r, "requestId"), "m-1") == 0,
        "response echoes requestId");
  json_object_put(r);

  /* 1b. duplicate requestId replays the cached response */
  r = request("query_state", "m-1dup", NULL);
  CHECK(r && jint(r, "status") == 200, "first query ok");
  json_object_put(r);
  r = request("query_state", "m-1dup", NULL);
  CHECK(r && jint(r, "status") == 200 && jstr(r, "requestId") &&
        strcmp(jstr(r, "requestId"), "m-1dup") == 0,
        "duplicate requestId replayed");
  json_object_put(r);

  /* 1c. second connection is served while the first stays open */
  {
    int fd2 = connect_sock();
    CHECK(fd2 >= 0, "second client connected");
    struct json_object* r2 = request_on(fd2, "query_state", "m-2conn", NULL);
    CHECK(r2 && jint(r2, "status") == 200, "second connection served concurrently");
    if (r2) json_object_put(r2);
    close(fd2);
  }

  /* 2. hello handshake reports the bundle contract versions */
  r = request("hello", "m-hello", NULL);
  {
    struct json_object* data = NULL;
    if (r) json_object_object_get_ex(r, "data", &data);
    CHECK(r && jint(r, "status") == 200, "hello ok");
    CHECK(data && jint(data, "ipcVersion") == 1, "hello reports ipc version 1");
    CHECK(data && jstr(data, "bundleVersion") &&
          strcmp(jstr(data, "bundleVersion"), "1") == 0,
          "hello reports bundle version 1");
  }
  json_object_put(r);

  /* 3. prepare is async: accepted then event */
  r = request("prepare", "m-2", "\"operationId\":\"op1\",\"revision\":1}");
  CHECK(r && jint(r, "status") == 202, "prepare returns 202 accepted");
  json_object_put(r);

  /* 3b. idempotent replay does not spawn a second worker: one event only */
  r = request("prepare", "m-2", "\"operationId\":\"op1\",\"revision\":1}");
  CHECK(r && jint(r, "status") == 202, "prepare replay returns 202");
  json_object_put(r);

  struct json_object* ev = read_line(conn_fd, 3000);
  const char* ev_type = ev ? jstr(ev, "type") : NULL;
  CHECK(ev && ev_type && strcmp(ev_type, "media.prepare.result") == 0,
        "prepare result event pushed");
  {
    struct json_object* evd = NULL;
    struct json_object* evlanes = NULL;
    if (ev) json_object_object_get_ex(ev, "data", &evd);
    if (evd) json_object_object_get_ex(evd, "lanes", &evlanes);
    CHECK(evlanes && json_object_is_type(evlanes, json_type_array),
          "prepare result carries channel summary");
    CHECK(ev && jstr(ev, "requestId") && jstr(ev, "requestId")[0],
          "prepare result carries requestId");
  }
  json_object_put(ev);
  ev = read_line(conn_fd, 400);
  CHECK(ev == NULL, "no duplicate prepare event");
  if (ev) json_object_put(ev);

  /* 4. commit */
  r = request("commit", "m-3", "\"operationId\":\"op1\",\"revision\":1}");
  CHECK(r && jint(r, "status") == 200, "commit ok");
  json_object_put(r);

  ev = read_line(conn_fd, 2000);
  CHECK(ev && jstr(ev, "type") && strcmp(jstr(ev, "type"), "media.layout_applied") == 0,
        "layout applied event broadcast");
  CHECK(ev && jstr(ev, "requestId") && strcmp(jstr(ev, "requestId"), "m-3") == 0,
        "layout applied echoes the commit requestId");
  json_object_put(ev);
  ev = read_line(conn_fd, 2000);
  CHECK(ev && jstr(ev, "type") && strcmp(jstr(ev, "type"), "media.state_changed") == 0,
        "state changed event broadcast");
  json_object_put(ev);

  /* 4b. payload-nested prepare/commit works */
  r = request_payload("prepare", "m-p1", "{\"operationId\":\"opPay\",\"revision\":2}");
  CHECK(r && jint(r, "status") == 202, "payload prepare accepted");
  json_object_put(r);
  ev = read_line(conn_fd, 3000);
  CHECK(ev && jstr(ev, "type") && strcmp(jstr(ev, "type"), "media.prepare.result") == 0,
        "payload prepare event pushed");
  json_object_put(ev);
  r = request_payload("commit", "m-p2", "{\"operationId\":\"opPay\",\"revision\":2}");
  CHECK(r && jint(r, "status") == 200, "payload commit ok");
  json_object_put(r);
  ev = read_line(conn_fd, 2000);
  CHECK(ev && jstr(ev, "type") && strcmp(jstr(ev, "type"), "media.layout_applied") == 0,
        "payload commit broadcasts");
  json_object_put(ev);
  ev = read_line(conn_fd, 2000);
  if (ev) json_object_put(ev); /* state_changed */

  /* 4c. abort matrix: idle refused, happy path clears */
  r = request("prepare", "m-a1", "\"operationId\":\"opA\",\"revision\":3}");
  CHECK(r && jint(r, "status") == 202, "abort setup prepare accepted");
  json_object_put(r);
  ev = read_line(conn_fd, 3000);
  if (ev) json_object_put(ev);
  r = request("abort", "m-a2", "\"operationId\":\"opWrong\"}");
  CHECK(r && jint(r, "status") == 400, "abort mismatch rejected");
  json_object_put(r);
  r = request("abort", "m-a3", "\"operationId\":\"opA\"}");
  CHECK(r && jint(r, "status") == 200, "abort prepared op ok");
  json_object_put(r);
  drain_pending(conn_fd);
  r = request("abort", "m-a4", NULL);
  CHECK(r && jint(r, "status") == 409, "abort on idle rejected");
  json_object_put(r);

  /* 5. stale revision rejected */
  r = request("prepare", "m-4", "\"operationId\":\"op2\",\"revision\":0}");
  CHECK(r && jstr(r, "code") && strcmp(jstr(r, "code"), "STALE_REVISION") == 0,
        "stale revision rejected");
  json_object_put(r);

  /* 6. commit without prepare rejected */
  r = request("commit", "m-5", "\"operationId\":\"op9\",\"revision\":5}");
  CHECK(r && jstr(r, "code") && strcmp(jstr(r, "code"), "INVALID_STATE") == 0,
        "commit without prepare rejected");
  json_object_put(r);

  /* 7. clear idempotent + unknown type */
  r = request("clear", "m-6", NULL);
  CHECK(r && jint(r, "status") == 200, "clear ok");
  json_object_put(r);
  drain_pending(conn_fd);
  r = request("bogus", "m-7", NULL);
  CHECK(r && jint(r, "status") == 400, "unknown type rejected");
  json_object_put(r);

  /* 8. query_state revision persisted */
  r = request("query_state", "m-8", NULL);
  struct json_object* data = NULL;
  if (r) json_object_object_get_ex(r, "data", &data);
  CHECK(data && jint(data, "revision") == 2, "revision persisted after commit");
  json_object_put(r);

  /* 9. platform windows shape: the bridge no longer silently passes the
   * windows shape through — since the agent translates to streams
   * (DVS_WINDOWS_TRANSLATION_GAPS_DESIGN.md), a windows-shape prepare means
   * a pre-translation agent and is refused; no layout is staged, so a later
   * commit keeps the running picture (no clear). */
  r = request_payload("prepare", "m-w1",
                      "{\"operationId\":\"opWin\",\"revision\":3,"
                      "\"applyAtNs\":0,"
                      "\"windows\":[{\"id\":\"w1\",\"sig\":\"s1\",\"x\":0,\"y\":0,"
                      "\"w\":1920,\"h\":1080,\"z\":1,\"muted\":false}],"
                      "\"geometry\":{\"x\":0,\"y\":0,\"w\":1920,\"h\":1080}}");
  CHECK(r && jint(r, "status") != 202, "windows shape prepare refused");
  json_object_put(r);
  ev = read_line(conn_fd, 500);
  CHECK(ev == NULL, "no prepare.result event for refused windows shape");
  if (ev) json_object_put(ev);
  r = request_payload("commit", "m-w2", "{\"operationId\":\"opWin\",\"revision\":3}");
  CHECK(r && jint(r, "status") == 409 && jstr(r, "code") &&
            strcmp(jstr(r, "code"), "INVALID_STATE") == 0,
        "commit without staged layout refused, picture kept");
  json_object_put(r);
  ev = read_line(conn_fd, 500);
  CHECK(ev == NULL, "refused commit broadcasts nothing");
  if (ev) json_object_put(ev);

  /* 10. oversized query_state: 300 lanes must not truncate the envelope */
  r = request("query_state", "m-big", NULL);
  {
    struct json_object* lanes = NULL;
    if (r) json_object_object_get_ex(r, "data", &data);
    if (data) json_object_object_get_ex(data, "lanes", &lanes);
    CHECK(r && jint(r, "status") == 200, "big query_state ok");
    CHECK(lanes && json_object_is_type(lanes, json_type_array) &&
              json_object_array_length(lanes) == 300,
          "big query_state lanes intact (no truncation)");
  }
  json_object_put(r);

  close(conn_fd);
  /* server runs until killed; test process exits regardless */
  printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
  return failures ? 1 : 0;
}
