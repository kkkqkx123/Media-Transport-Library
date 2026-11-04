#include "ipc_server.h"
#include "state_machine.h"
#include "dvs_node_bundle.h"
#include "engine/capability.h"

#include <errno.h>
#include <json-c/json.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define DVS_IPC_MAX_CLIENTS 8
#define DVS_IPC_BUF 8192

/* ------------------------------------------------------------------ */
/* per-connection state                                                */
/* ------------------------------------------------------------------ */

typedef struct {
  int fd;
  int active;
  pthread_mutex_t wmu; /* guards writes from reader + worker threads */
} dvs_client_t;

static dvs_client_t g_clients[DVS_IPC_MAX_CLIENTS];
static pthread_mutex_t g_clients_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_wmu_global = PTHREAD_MUTEX_INITIALIZER; /* sequence ids */
static long g_seq = 0;
static int g_prepare_delay_ms = 50;

static dvs_capability g_cap;

/* Engine hooks (installed by engine/ipc_engine_bridge.c in the real binary;
 * unset in the no-MTL unit test, leaving the server in pure state-machine
 * mode). */
static dvs_ipc_engine_hooks_t g_hooks;

void dvs_ipc_set_prepare_delay_ms(int ms) { g_prepare_delay_ms = ms; }

void dvs_ipc_install_hooks(const dvs_ipc_engine_hooks_t* h) {
    if (h) g_hooks = *h;
}

static int write_line_locked(dvs_client_t* c, const char* line) {
  pthread_mutex_lock(&c->wmu);
  ssize_t n = write(c->fd, line, strlen(line));
  ssize_t r = (n > 0) ? write(c->fd, "\n", 1) : -1;
  pthread_mutex_unlock(&c->wmu);
  return (n > 0 && r > 0) ? 0 : -1;
}

static int send_envelope(dvs_client_t* c, const char* request_id, int status,
                         const char* code, const char* msg, const char* data_json) {
  char buf[1024];
  snprintf(buf, sizeof(buf),
           "{\"ok\":%s,\"status\":%d,\"code\":\"%s\",\"msg\":\"%s\",\"data\":%s,\"requestId\":\"%s\"}",
           (status < 400) ? "true" : "false", status, code, msg,
           data_json ? data_json : "{}", request_id ? request_id : "ipc");
  return write_line_locked(c, buf);
}

static void broadcast_event(const char* type, const char* data_json) {
  char buf[1024];
  snprintf(buf, sizeof(buf), "{\"type\":\"%s\",\"data\":%s}", type,
           data_json ? data_json : "{}");
  pthread_mutex_lock(&g_clients_mu);
  for (int i = 0; i < DVS_IPC_MAX_CLIENTS; i++) {
    if (g_clients[i].active)
      write_line_locked(&g_clients[i], buf);
  }
  pthread_mutex_unlock(&g_clients_mu);
}

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static const char* str_field(struct json_object* o, const char* key) {
  struct json_object* v = NULL;
  if (o && json_object_object_get_ex(o, key, &v) && v)
    return json_object_get_string(v);
  return NULL;
}

static int int_field(struct json_object* o, const char* key, int def) {
  struct json_object* v = NULL;
  if (o && json_object_object_get_ex(o, key, &v) && v)
    return json_object_get_int(v);
  return def;
}

/* ------------------------------------------------------------------ */
/* async prepare worker                                                */
/* ------------------------------------------------------------------ */

typedef struct {
  dvs_client_t* client;
  char request_id[64];
  char opid[64];
  int revision;
} prepare_job_t;

static void* prepare_worker(void* arg) {
  prepare_job_t* job = arg;

  /* Simulated preparation work (decode-first-frame wait, plane allocation).
   * Real engine work lands here when engine/session_mgr is wired up. */
  if (g_prepare_delay_ms > 0) usleep((useconds_t)g_prepare_delay_ms * 1000);

  int rc = dvs_state_finish_prepare(job->opid);
  if (rc != 0) dvs_state_fail_prepare(job->opid);
  char ev[600];
  snprintf(ev, sizeof(ev),
           "{\"type\":\"media.prepare.result\",\"requestId\":\"%s\","
           "\"data\":{\"operationId\":\"%s\",\"revision\":%d,\"status\":\"%s\"}}",
           job->request_id, job->opid, job->revision,
           (rc == 0) ? "ready" : "blocked");
  write_line_locked(job->client, ev);

  free(job);
  return NULL;
}

static void spawn_prepare(dvs_client_t* c, const char* request_id,
                          const char* opid, int revision) {
  prepare_job_t* job = calloc(1, sizeof(*job));
  if (!job) {
    dvs_state_fail_prepare(opid);
    send_envelope(c, request_id, 500, "INTERNAL", "out of memory", NULL);
    return;
  }
  job->client = c;
  snprintf(job->request_id, sizeof(job->request_id), "%s", request_id);
  snprintf(job->opid, sizeof(job->opid), "%s", opid);
  job->revision = revision;

  pthread_t tid;
  if (pthread_create(&tid, NULL, prepare_worker, job) != 0) {
    free(job);
    dvs_state_fail_prepare(opid);
    send_envelope(c, request_id, 500, "INTERNAL", "cannot start prepare worker", NULL);
    return;
  }
  pthread_detach(tid);
}

/* ------------------------------------------------------------------ */
/* request handling                                                    */
/* ------------------------------------------------------------------ */

static int handle_line(dvs_client_t* c, char* line) {
  struct json_object* req = json_tokener_parse(line);
  if (!req) {
    send_envelope(c, NULL, 400, "INVALID_ARGUMENT", "malformed json", NULL);
    return 0;
  }
  const char* type = str_field(req, "type");
  const char* msg_id = str_field(req, "messageId");

  char rid[64];
  if (msg_id && msg_id[0]) {
    snprintf(rid, sizeof(rid), "%s", msg_id);
  } else {
    pthread_mutex_lock(&g_wmu_global);
    long s = ++g_seq;
    pthread_mutex_unlock(&g_wmu_global);
    snprintf(rid, sizeof(rid), "req-%ld", s);
  }

  char err[128] = {0};
  char data[512];

  if (!type || !type[0]) {
    send_envelope(c, rid, 400, "INVALID_ARGUMENT", "missing type", NULL);
    goto done;
  }

  if (strcmp(type, "hello") == 0) {
    /* bundle handshake: report protocol + bundle versions with a state
     * snapshot so the agent can confirm it talks to a co-released engine */
    dvs_state_t snap;
    dvs_state_snapshot(&snap);
    const char* name = "idle";
    switch (snap.op_state) {
      case DVS_OP_PREPARING: name = "preparing"; break;
      case DVS_OP_PREPARED: name = "prepared"; break;
      default: break;
    }
    snprintf(data, sizeof(data),
             "{\"ipcVersion\":%d,\"bundleVersion\":\"%s\",\"state\":\"%s\",\"revision\":%d}",
             DVS_MEDIA_IPC_VERSION, DVS_NODE_BUNDLE_VERSION, name, snap.revision);
    send_envelope(c, rid, 200, "OK", "hello", data);
  } else if (strcmp(type, "configure") == 0) {
    /* single entry for desired state (streams/layout JSON); stash as pending */
    if (g_hooks.prepare) g_hooks.prepare(req);
    send_envelope(c, rid, 200, "OK", "configured", NULL);
  } else if (strcmp(type, "prepare") == 0) {
    const char* opid = str_field(req, "operationId");
    int revision = int_field(req, "revision", -1);
    if (!opid || !opid[0]) {
      send_envelope(c, rid, 400, "INVALID_ARGUMENT", "missing operationId", NULL);
      goto done;
    }
    if (revision < 0) {
      dvs_state_t snap;
      dvs_state_snapshot(&snap);
      revision = snap.revision + 1;
    }
    int rc = dvs_state_begin_prepare(opid, revision, err, sizeof(err));
    if (rc != 0) {
      int status = (rc == DVS_ERR_STALE_REVISION) ? 409 : 400;
      const char* code =
          (rc == DVS_ERR_STALE_REVISION) ? "STALE_REVISION" : "INVALID_ARGUMENT";
      send_envelope(c, rid, status, code, err, NULL);
      goto done;
    }
    spawn_prepare(c, rid, opid, revision);
    send_envelope(c, rid, 202, "ACCEPTED", "preparing", NULL);
  } else if (strcmp(type, "commit") == 0) {
    const char* opid = str_field(req, "operationId");
    int revision = int_field(req, "revision", -1);
    if (!opid || !opid[0]) {
      send_envelope(c, rid, 400, "INVALID_ARGUMENT", "missing operationId", NULL);
      goto done;
    }
    if (revision < 0) {
      dvs_state_t snap;
      dvs_state_snapshot(&snap);
      revision = (snap.op_state == DVS_OP_PREPARED) ? snap.prepared_revision
                                                    : snap.revision + 1;
    }
    int applied = 0;
    int rc = dvs_state_commit(opid, revision, &applied, err, sizeof(err));
    if (rc != 0) {
      int status = 409;
      const char* code = (rc == DVS_ERR_STALE_REVISION) ? "STALE_REVISION"
                         : (rc == DVS_ERR_INVALID_STATE) ? "INVALID_STATE"
                                                         : "INVALID_ARGUMENT";
      send_envelope(c, rid, status, code, err, NULL);
      goto done;
    }
    snprintf(data, sizeof(data), "{\"revision\":%d}", applied);
    /* apply the pending layout to the real engine, if hooks installed */
    if (g_hooks.commit) {
      if (g_hooks.commit() != 0) {
        send_envelope(c, rid, 500, "INTERNAL", "layout apply failed", NULL);
        goto done;
      }
    }
    send_envelope(c, rid, 200, "OK", "committed", data);
    char ev[256];
    snprintf(ev, sizeof(ev),
             "{\"operationId\":\"%s\",\"revision\":%d,\"status\":\"applied\"}", opid,
             applied);
    broadcast_event("media.layout_applied", ev);
  } else if (strcmp(type, "abort") == 0) {
    const char* opid = str_field(req, "operationId");
    dvs_state_t snap;
    dvs_state_snapshot(&snap);
    if (snap.op_state == DVS_OP_IDLE && (!opid || !opid[0] || !snap.operation_id[0])) {
      send_envelope(c, rid, 409, "INVALID_STATE", "nothing to abort", NULL);
      goto done;
    }
    dvs_state_abort(opid);
    if (g_hooks.abort) g_hooks.abort();
    send_envelope(c, rid, 200, "OK", "aborted", NULL);
  } else if (strcmp(type, "clear") == 0) {
    if (g_hooks.clear) g_hooks.clear();
    dvs_state_clear();
    send_envelope(c, rid, 200, "OK", "cleared", NULL);
  } else if (strcmp(type, "query_state") == 0) {
    dvs_state_t snap;
    dvs_state_snapshot(&snap);
    const char* name = "idle";
    switch (snap.op_state) {
      case DVS_OP_PREPARING: name = "preparing"; break;
      case DVS_OP_PREPARED: name = "prepared"; break;
      default: break;
    }
    int rx_sessions = 0, decoding = 0, layers = 0;
    if (g_hooks.counts) g_hooks.counts(&rx_sessions, &decoding, &layers);
    char ptp_state[32];
    int ptp_off = 0;
    snprintf(ptp_state, sizeof(ptp_state), "%s", g_cap.ptp_state);
    ptp_off = g_cap.ptp_offset_us;
    if (g_hooks.ptp_state) g_hooks.ptp_state(ptp_state, sizeof(ptp_state), &ptp_off);
    /* Build the data object so the per-lane array can be embedded. */
    json_object* data = json_object_new_object();
    json_object_object_add(data, "state", json_object_new_string(name));
    json_object_object_add(data, "revision", json_object_new_int(snap.revision));
    json_object_object_add(data, "operationId", json_object_new_string(snap.operation_id));
    json_object_object_add(data, "opState", json_object_new_int((int)snap.op_state));
    json_object_object_add(data, "rxSessions", json_object_new_int(rx_sessions));
    json_object_object_add(data, "decoding", json_object_new_int(decoding));
    json_object_object_add(data, "layers", json_object_new_int(layers));
    json_object* ptp = json_object_new_object();
    json_object_object_add(ptp, "state", json_object_new_string(ptp_state));
    json_object_object_add(ptp, "offsetUs", json_object_new_int(ptp_off));
    json_object_object_add(data, "ptp", ptp);
    json_object* lanes = (g_hooks.lane_stats) ? g_hooks.lane_stats() : NULL;
    if (!lanes) lanes = json_object_new_array();
    json_object_object_add(data, "lanes", lanes);
    const char* data_s = json_object_to_json_string(data);
    send_envelope(c, rid, 200, "OK", "", data_s);
    json_object_put(data);
  } else if (strcmp(type, "event") == 0) {
    send_envelope(c, rid, 200, "OK", "event", NULL);
  } else {
    send_envelope(c, rid, 400, "INVALID_ARGUMENT", "unknown type", NULL);
  }

done:
  json_object_put(req);
  return 0;
}

/* ------------------------------------------------------------------ */
/* server loop                                                         */
/* ------------------------------------------------------------------ */

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
  if (listen(listen_fd, DVS_IPC_MAX_CLIENTS) < 0) {
    perror("listen");
    close(listen_fd);
    return -1;
  }

  dvs_state_init();
  dvs_capability_probe(&g_cap);
  memset(g_clients, 0, sizeof(g_clients));

  for (;;) {
    int client = accept(listen_fd, NULL, NULL);
    if (client < 0) {
      if (errno == EINTR) continue;
      perror("accept");
      continue;
    }

    dvs_client_t* c = NULL;
    pthread_mutex_lock(&g_clients_mu);
    for (int i = 0; i < DVS_IPC_MAX_CLIENTS; i++) {
      if (!g_clients[i].active) {
        c = &g_clients[i];
        break;
      }
    }
    if (c) {
      c->fd = client;
      c->active = 1;
      pthread_mutex_init(&c->wmu, NULL);
    }
    pthread_mutex_unlock(&g_clients_mu);

    if (!c) {
      /* too many clients: reply inline without registering the slot */
      const char* busy =
          "{\"ok\":false,\"status\":503,\"code\":\"BUSY\",\"msg\":\"too many "
          "clients\",\"data\":{},\"requestId\":\"ipc\"}\n";
      (void)!write(client, busy, strlen(busy));
      close(client);
      continue;
    }

    /* long-lived connection: read lines until EOF */
    char buf[DVS_IPC_BUF];
    size_t used = 0;
    for (;;) {
      struct pollfd pfd = {.fd = client, .events = POLLIN};
      int pr = poll(&pfd, 1, -1);
      if (pr < 0) {
        if (errno == EINTR) continue;
        break;
      }
      if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) break;
      if (!(pfd.revents & POLLIN)) break;

      ssize_t n = read(client, buf + used, sizeof(buf) - 1 - used);
      if (n <= 0) break;
      used += (size_t)n;
      buf[used] = '\0';

      char* start = buf;
      char* nl;
      while ((nl = strchr(start, '\n')) != NULL) {
        *nl = '\0';
        if (start[0] != '\0') handle_line(c, start);
        start = nl + 1;
      }
      /* keep the trailing partial line */
      used -= (size_t)(start - buf);
      memmove(buf, start, used);
      if (used == sizeof(buf) - 1) {
        /* oversized garbage line: drop it */
        used = 0;
      }
    }

    pthread_mutex_lock(&g_clients_mu);
    c->active = 0;
    pthread_mutex_unlock(&g_clients_mu);
    pthread_mutex_destroy(&c->wmu);
    close(client);
  }

  close(listen_fd);
  unlink(sock_path);
  return 0;
}
