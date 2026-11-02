#include "ipc_server.h"
#include "state_machine.h"

#include <errno.h>
#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

static void send_response(int fd, const char* json_str) {
  char buf[4096];
  snprintf(buf, sizeof(buf), "%s\n", json_str);
  (void)write(fd, buf, strlen(buf));
}

static const char* type_of(struct json_object* o) {
  struct json_object* t = NULL;
  if (json_object_object_get_ex(o, "type", &t)) {
    return json_object_get_string(t);
  }
  return "";
}

int dvs_ipc_server_run(const char* sock_path) {
  int listen_fd;
  struct sockaddr_un addr;

  unlink(sock_path);
  listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    perror("socket");
    return -1;
  }
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
  if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    perror("bind");
    close(listen_fd);
    return -1;
  }
  if (listen(listen_fd, 8) < 0) {
    perror("listen");
    close(listen_fd);
    return -1;
  }

  dvs_state_init();

  for (;;) {
    int client = accept(listen_fd, NULL, NULL);
    if (client < 0) {
      if (errno == EINTR) continue;
      perror("accept");
      continue;
    }
    char buf[8192];
    ssize_t n = read(client, buf, sizeof(buf) - 1);
    if (n <= 0) {
      close(client);
      continue;
    }
    buf[n] = '\0';

    /* Handle possibly multiple lines; process first line only. */
    char* newline = strchr(buf, '\n');
    if (newline) *newline = '\0';

    struct json_object* req = json_tokener_parse(buf);
    const char* t = req ? type_of(req) : "";

    char resp[1024];
    if (strcmp(t, "configure") == 0) {
      dvs_state_set_name("idle");
      snprintf(resp, sizeof(resp),
               "{\"ok\":true,\"status\":200,\"code\":\"OK\",\"msg\":\"configured\",\"data\":{},\"requestId\":\"ipc\"}");
    } else if (strcmp(t, "prepare") == 0) {
      dvs_state_set_name("prepared");
      snprintf(resp, sizeof(resp),
               "{\"ok\":true,\"status\":200,\"code\":\"OK\",\"msg\":\"prepared\",\"data\":{},\"requestId\":\"ipc\"}");
    } else if (strcmp(t, "commit") == 0) {
      dvs_state_set_revision(dvs_state_get_revision() + 1);
      dvs_state_set_name("committed");
      snprintf(resp, sizeof(resp),
               "{\"ok\":true,\"status\":200,\"code\":\"OK\",\"msg\":\"committed\",\"data\":{\"revision\":%d},\"requestId\":\"ipc\"}",
               dvs_state_get_revision());
    } else if (strcmp(t, "abort") == 0) {
      dvs_state_set_name("idle");
      snprintf(resp, sizeof(resp),
               "{\"ok\":true,\"status\":200,\"code\":\"OK\",\"msg\":\"aborted\",\"data\":{},\"requestId\":\"ipc\"}");
    } else if (strcmp(t, "clear") == 0) {
      dvs_state_set_name("idle");
      snprintf(resp, sizeof(resp),
               "{\"ok\":true,\"status\":200,\"code\":\"OK\",\"msg\":\"cleared\",\"data\":{},\"requestId\":\"ipc\"}");
    } else if (strcmp(t, "set_sources") == 0) {
      snprintf(resp, sizeof(resp),
               "{\"ok\":true,\"status\":200,\"code\":\"OK\",\"msg\":\"sources set\",\"data\":{},\"requestId\":\"ipc\"}");
    } else if (strcmp(t, "query_state") == 0) {
      snprintf(resp, sizeof(resp),
               "{\"ok\":true,\"status\":200,\"code\":\"OK\",\"msg\":\"\",\"data\":{\"state\":\"%s\",\"revision\":%d,\"rxSessions\":0,\"decoding\":0,\"layers\":0,\"ptp\":{\"state\":\"unknown\",\"offsetUs\":0}},\"requestId\":\"ipc\"}",
               dvs_state_get_name(), dvs_state_get_revision());
    } else if (strcmp(t, "event") == 0) {
      snprintf(resp, sizeof(resp),
               "{\"ok\":true,\"status\":200,\"code\":\"OK\",\"msg\":\"event\",\"data\":{},\"requestId\":\"ipc\"}");
    } else {
      snprintf(resp, sizeof(resp),
               "{\"ok\":false,\"status\":400,\"code\":\"INVALID_ARGUMENT\",\"msg\":\"unknown type\",\"data\":{},\"requestId\":\"ipc\"}");
    }

    if (req) json_object_put(req);
    send_response(client, resp);
    close(client);
  }

  close(listen_fd);
  unlink(sock_path);
  return 0;
}
