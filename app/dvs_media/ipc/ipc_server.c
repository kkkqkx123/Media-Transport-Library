#include "ipc_server.h"
#include "state_machine.h"
#include "dvs_node_bundle.h"
#include "engine/capability.h"

#include <errno.h>
#include <json-c/json.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#define DVS_IPC_MAX_CLIENTS 8
#define DVS_IPC_BUF 8192
#define DVS_IPC_IDEM 32

/* ------------------------------------------------------------------ */
/* per-connection state                                                */
/* ------------------------------------------------------------------ */

typedef struct {
  char id[64];
  char* resp;
} dvs_idem_entry_t;

typedef struct {
  int fd;
  int active;
  unsigned long gen;
  pthread_mutex_t wmu; /* guards writes from reader + worker threads */
  dvs_idem_entry_t idem[DVS_IPC_IDEM];
  int idem_head;
  int idem_count;
} dvs_client_t;

static dvs_client_t g_clients[DVS_IPC_MAX_CLIENTS];
static pthread_mutex_t g_clients_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_seq_mu = PTHREAD_MUTEX_INITIALIZER;
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

/* Build the response envelope dynamically (json-c handles escaping and
 * there is no fixed-size truncation, P15). Returns a malloc'd line without
 * the trailing newline; NULL on OOM. */
static char* build_envelope(const char* request_id, int status, const char* code,
                            const char* msg, const char* data_json) {
  json_object* o = json_object_new_object();
  if (!o) return NULL;
  json_object_object_add(o, "ok", json_object_new_boolean(status < 400));
  json_object_object_add(o, "status", json_object_new_int(status));
  json_object_object_add(o, "code", json_object_new_string(code ? code : "INTERNAL"));
  json_object_object_add(o, "msg", json_object_new_string(msg ? msg : ""));
  json_object* data = json_tokener_parse(data_json ? data_json : "{}");
  if (!data) data = json_object_new_object();
  json_object_object_add(o, "data", data);
  json_object_object_add(
      o, "requestId", json_object_new_string(request_id ? request_id : "ipc"));
  const char* s = json_object_to_json_string(o);
  char* out = s ? strdup(s) : NULL;
  json_object_put(o);
  return out;
}

static int send_envelope(dvs_client_t* c, const char* request_id, int status,
                         const char* code, const char* msg, const char* data_json) {
  char* line = build_envelope(request_id, status, code, msg, data_json);
  if (!line) return -1;
  int rc = write_line_locked(c, line);
  free(line);
  return rc;
}

/* Per-connection messageId replay cache. Only the owning reader thread
 * touches it, so no extra lock is needed. */
static const char* idem_lookup(dvs_client_t* c, const char* rid) {
  for (int i = 0; i < c->idem_count; i++) {
    int idx = (c->idem_head + DVS_IPC_IDEM - 1 - i + DVS_IPC_IDEM) % DVS_IPC_IDEM;
    if (c->idem[idx].resp && strcmp(c->idem[idx].id, rid) == 0) return c->idem[idx].resp;
  }
  return NULL;
}

static void idem_store(dvs_client_t* c, const char* rid, const char* resp_line) {
  int idx;
  if (c->idem_count < DVS_IPC_IDEM) {
    idx = (c->idem_head + c->idem_count) % DVS_IPC_IDEM;
    c->idem_count++;
  } else {
    idx = c->idem_head;
    free(c->idem[idx].resp);
    c->idem[idx].resp = NULL;
    c->idem_head = (c->idem_head + 1) % DVS_IPC_IDEM;
  }
  snprintf(c->idem[idx].id, sizeof(c->idem[idx].id), "%s", rid);
  c->idem[idx].resp = strdup(resp_line);
}

static void idem_clear(dvs_client_t* c) {
  for (int i = 0; i < DVS_IPC_IDEM; i++) {
    free(c->idem[i].resp);
    c->idem[i].resp = NULL;
    c->idem[i].id[0] = '\0';
  }
  c->idem_head = 0;
  c->idem_count = 0;
}

static int respond_cached(dvs_client_t* c, const char* rid, int cacheable,
                          int status, const char* code, const char* msg,
                          const char* data_json) {
  char* line = build_envelope(rid, status, code, msg, data_json);
  if (!line) return -1;
  if (cacheable && rid && rid[0]) idem_store(c, rid, line);
  int rc = write_line_locked(c, line);
  free(line);
  return rc;
}

static void broadcast_event(const char* type, const char* data_json) {
  char buf[8192];
  snprintf(buf, sizeof(buf), "{\"type\":\"%s\",\"data\":%s}", type,
           data_json ? data_json : "{}");
  /* Snapshot the active clients so a slow reader cannot hold the global
   * table lock across blocking writes; each write still takes its own
   * per-connection lock. A client that disconnects mid-broadcast simply
   * gets a failed write that is ignored. */
  dvs_client_t* targets[DVS_IPC_MAX_CLIENTS];
  int n = 0;
  pthread_mutex_lock(&g_clients_mu);
  for (int i = 0; i < DVS_IPC_MAX_CLIENTS; i++) {
    if (g_clients[i].active)
      targets[n++] = &g_clients[i];
  }
  pthread_mutex_unlock(&g_clients_mu);
  for (int i = 0; i < n; i++)
    write_line_locked(targets[i], buf);
}

static void broadcast_state_changed(void) {
  dvs_state_t snap;
  dvs_state_snapshot(&snap);
  const char* name = "idle";
  switch (snap.op_state) {
    case DVS_OP_PREPARING: name = "preparing"; break;
    case DVS_OP_PREPARED: name = "prepared"; break;
    default: break;
  }
  char ev[256];
  snprintf(ev, sizeof(ev),
           "{\"operationId\":\"%s\",\"revision\":%d,\"state\":\"%s\",\"opState\":%d}",
           snap.operation_id, snap.revision, name, (int)snap.op_state);
  broadcast_event("media.state_changed", ev);
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

/* Business body: agent nests fields under payload, older callers use the top
 * level. type/messageId stay top-level; everything else reads from the body. */
static struct json_object* body_of(struct json_object* req) {
  struct json_object* p = NULL;
  if (req && json_object_object_get_ex(req, "payload", &p) && p &&
      json_object_is_type(p, json_type_object))
    return p;
  return req;
}

/* ------------------------------------------------------------------ */
/* async prepare worker                                                */
/* ------------------------------------------------------------------ */

typedef struct {
  int slot;
  unsigned long gen;
  int fd;
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

  pthread_mutex_lock(&g_clients_mu);
  int ok = (job->slot >= 0 && job->slot < DVS_IPC_MAX_CLIENTS &&
            g_clients[job->slot].active && g_clients[job->slot].gen == job->gen &&
            g_clients[job->slot].fd == job->fd);
  dvs_client_t* c = ok ? &g_clients[job->slot] : NULL;
  if (c) pthread_mutex_lock(&c->wmu);
  pthread_mutex_unlock(&g_clients_mu);
  if (c) {
    ssize_t n = write(c->fd, ev, strlen(ev));
    ssize_t r = (n > 0) ? write(c->fd, "\n", 1) : -1;
    (void)r;
    pthread_mutex_unlock(&c->wmu);
  }

  free(job);
  return NULL;
}

static void spawn_prepare(dvs_client_t* c, int slot, unsigned long gen, int fd,
                          const char* request_id, const char* opid, int revision) {
  prepare_job_t* job = calloc(1, sizeof(*job));
  if (!job) {
    dvs_state_fail_prepare(opid);
    send_envelope(c, request_id, 500, "INTERNAL", "out of memory", NULL);
    return;
  }
  job->slot = slot;
  job->gen = gen;
  job->fd = fd;
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
  int cacheable = (msg_id && msg_id[0]) ? 1 : 0;
  if (cacheable) {
    snprintf(rid, sizeof(rid), "%s", msg_id);
    const char* hit = idem_lookup(c, rid);
    if (hit) {
      write_line_locked(c, hit);
      json_object_put(req);
      return 0;
    }
  } else {
    pthread_mutex_lock(&g_seq_mu);
    long s = ++g_seq;
    pthread_mutex_unlock(&g_seq_mu);
    snprintf(rid, sizeof(rid), "req-%ld", s);
  }

  struct json_object* body = body_of(req);
  char err[128] = {0};
  char data[512];

  if (!type || !type[0]) {
    respond_cached(c, rid, cacheable, 400, "INVALID_ARGUMENT", "missing type", NULL);
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
    respond_cached(c, rid, cacheable, 200, "OK", "hello", data);
  } else if (strcmp(type, "configure") == 0) {
    if (g_hooks.prepare) {
      if (g_hooks.prepare(body, err, sizeof(err)) != 0) {
        respond_cached(c, rid, cacheable, 400, "INVALID_ARGUMENT",
                       err[0] ? err : "bad configure", NULL);
        goto done;
      }
    }
    respond_cached(c, rid, cacheable, 200, "OK", "configured", NULL);
  } else if (strcmp(type, "set_sources") == 0) {
    if (g_hooks.prepare) {
      if (g_hooks.prepare(body, err, sizeof(err)) != 0) {
        respond_cached(c, rid, cacheable, 400, "INVALID_ARGUMENT",
                       err[0] ? err : "bad sources", NULL);
        goto done;
      }
    }
    respond_cached(c, rid, cacheable, 200, "OK", "sources set", NULL);
  } else if (strcmp(type, "prepare") == 0) {
    const char* opid = str_field(body, "operationId");
    int revision = int_field(body, "revision", -1);
    if (!opid || !opid[0]) {
      respond_cached(c, rid, cacheable, 400, "INVALID_ARGUMENT",
                     "missing operationId", NULL);
      goto done;
    }
    if (revision < 0) {
      dvs_state_t snap;
      dvs_state_snapshot(&snap);
      revision = snap.revision + 1;
    }
    /* idempotent replay must not re-validate nor re-spawn the worker */
    {
      dvs_state_t snap;
      dvs_state_snapshot(&snap);
      if (snap.op_state != DVS_OP_IDLE && strcmp(snap.operation_id, opid) == 0 &&
          revision == snap.prepared_revision) {
        respond_cached(c, rid, cacheable, 202, "ACCEPTED", "preparing", NULL);
        goto done;
      }
    }
    if (g_hooks.prepare) {
      if (g_hooks.prepare(body, err, sizeof(err)) != 0) {
        respond_cached(c, rid, cacheable, 400, "INVALID_ARGUMENT",
                       err[0] ? err : "bad prepare", NULL);
        goto done;
      }
    }
    int rc = dvs_state_begin_prepare(opid, revision, err, sizeof(err));
    if (rc != 0) {
      int status = (rc == DVS_ERR_STALE_REVISION) ? 409 : 400;
      const char* code =
          (rc == DVS_ERR_STALE_REVISION) ? "STALE_REVISION" : "INVALID_ARGUMENT";
      if (rc == DVS_ERR_INVALID_STATE) {
        status = 409;
        code = "INVALID_STATE";
      }
      respond_cached(c, rid, cacheable, status, code, err, NULL);
      goto done;
    }
    int slot = (int)(c - g_clients);
    spawn_prepare(c, slot, c->gen, c->fd, rid, opid, revision);
    respond_cached(c, rid, cacheable, 202, "ACCEPTED", "preparing", NULL);
  } else if (strcmp(type, "commit") == 0) {
    const char* opid = str_field(body, "operationId");
    int revision = int_field(body, "revision", -1);
    if (!opid || !opid[0]) {
      respond_cached(c, rid, cacheable, 400, "INVALID_ARGUMENT",
                     "missing operationId", NULL);
      goto done;
    }
    if (revision < 0) {
      dvs_state_t snap;
      dvs_state_snapshot(&snap);
      revision = (snap.op_state == DVS_OP_PREPARED) ? snap.prepared_revision
                                                    : snap.revision + 1;
    }
    int applied = 0;
    /* Replay of an already-committed operation answers without touching
     * the engine again; the first commit already applied the layout. */
    {
      dvs_state_t snap;
      dvs_state_snapshot(&snap);
      if (snap.op_state == DVS_OP_IDLE && strcmp(snap.operation_id, opid) == 0 &&
          revision == snap.revision) {
        snprintf(data, sizeof(data), "{\"revision\":%d}", snap.revision);
        respond_cached(c, rid, cacheable, 200, "OK", "committed", data);
        goto done;
      }
    }
    /* Apply the pending layout before advancing the state machine so an
     * engine failure leaves the operation prepared (retry/abort) instead
     * of a falsely committed revision. The executor commit is idempotent. */
    if (g_hooks.commit) {
      if (g_hooks.commit() != 0) {
        respond_cached(c, rid, cacheable, 500, "INTERNAL", "layout apply failed",
                       NULL);
        goto done;
      }
    }
    int rc = dvs_state_commit(opid, revision, &applied, err, sizeof(err));
    if (rc != 0) {
      int status = 409;
      const char* code = (rc == DVS_ERR_STALE_REVISION) ? "STALE_REVISION"
                         : (rc == DVS_ERR_INVALID_STATE) ? "INVALID_STATE"
                                                         : "INVALID_ARGUMENT";
      if (rc == DVS_ERR_INVALID_ARG) status = 400;
      respond_cached(c, rid, cacheable, status, code, err, NULL);
      goto done;
    }
    snprintf(data, sizeof(data), "{\"revision\":%d}", applied);
    respond_cached(c, rid, cacheable, 200, "OK", "committed", data);
    char ev[256];
    snprintf(ev, sizeof(ev),
             "{\"operationId\":\"%s\",\"revision\":%d,\"status\":\"applied\"}", opid,
             applied);
    broadcast_event("media.layout_applied", ev);
    broadcast_state_changed();
  } else if (strcmp(type, "abort") == 0) {
    const char* opid = str_field(body, "operationId");
    int rc = dvs_state_abort(opid);
    if (rc != 0) {
      if (rc == DVS_ERR_INVALID_ARG) {
        respond_cached(c, rid, cacheable, 400, "INVALID_ARGUMENT",
                       "operation mismatch", NULL);
      } else {
        respond_cached(c, rid, cacheable, 409, "INVALID_STATE",
                       "nothing to abort", NULL);
      }
      goto done;
    }
    if (g_hooks.abort) g_hooks.abort();
    respond_cached(c, rid, cacheable, 200, "OK", "aborted", NULL);
    broadcast_state_changed();
  } else if (strcmp(type, "clear") == 0) {
    if (g_hooks.clear) g_hooks.clear();
    dvs_state_clear();
    respond_cached(c, rid, cacheable, 200, "OK", "cleared", NULL);
    broadcast_state_changed();
  } else if (strcmp(type, "tx_start") == 0) {
    if (!g_hooks.tx_start) {
      respond_cached(c, rid, cacheable, 501, "NOT_IMPLEMENTED",
                     "tx backend not attached", NULL);
      goto done;
    }
    if (g_hooks.tx_start(body, err, sizeof(err)) != 0) {
      respond_cached(c, rid, cacheable, 400, "INVALID_ARGUMENT",
                     err[0] ? err : "bad tx params", NULL);
      goto done;
    }
    /* Report the resulting session state so the caller can tell running
     * apart from configured (backend pending). */
    if (g_hooks.tx_status) {
      json_object* tx = g_hooks.tx_status();
      const char* tx_s = json_object_to_json_string(tx);
      respond_cached(c, rid, cacheable, 200, "OK", "tx started", tx_s ? tx_s : "{}");
      if (tx) json_object_put(tx);
    } else {
      respond_cached(c, rid, cacheable, 200, "OK", "tx started", NULL);
    }
  } else if (strcmp(type, "tx_stop") == 0) {
    if (g_hooks.tx_stop) g_hooks.tx_stop();
    respond_cached(c, rid, cacheable, 200, "OK", "tx stopped", NULL);
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
    json_object* datao = json_object_new_object();
    json_object_object_add(datao, "state", json_object_new_string(name));
    json_object_object_add(datao, "revision", json_object_new_int(snap.revision));
    json_object_object_add(datao, "operationId", json_object_new_string(snap.operation_id));
    json_object_object_add(datao, "opState", json_object_new_int((int)snap.op_state));
    json_object_object_add(datao, "rxSessions", json_object_new_int(rx_sessions));
    json_object_object_add(datao, "decoding", json_object_new_int(decoding));
    json_object_object_add(datao, "layers", json_object_new_int(layers));
    json_object* ptp = json_object_new_object();
    json_object_object_add(ptp, "state", json_object_new_string(ptp_state));
    json_object_object_add(ptp, "offsetUs", json_object_new_int(ptp_off));
    json_object_object_add(datao, "ptp", ptp);
    json_object* lanes = (g_hooks.lane_stats) ? g_hooks.lane_stats() : NULL;
    if (!lanes) lanes = json_object_new_array();
    json_object_object_add(datao, "lanes", lanes);
    /* TX session snapshot. NULL hook (unit test) means idle. */
    json_object* tx = (g_hooks.tx_status) ? g_hooks.tx_status() : NULL;
    if (!tx) {
      tx = json_object_new_object();
      json_object_object_add(tx, "state", json_object_new_string("idle"));
    }
    json_object_object_add(datao, "tx", tx);
    const char* data_s = json_object_to_json_string(datao);
    respond_cached(c, rid, cacheable, 200, "OK", "", data_s);
    json_object_put(datao);
  } else if (strcmp(type, "capability") == 0) {
    /* CAP-1: refresh live PTP and MTL version before serializing. The hooks
     * are NULL in the no-MTL unit test, which then keeps probed values. */
    int refreshed = 0;
    if (g_hooks.ptp_state) {
      char ps[32];
      int off = 0;
      snprintf(ps, sizeof(ps), "%s", g_cap.ptp_state);
      g_hooks.ptp_state(ps, sizeof(ps), &off);
      snprintf(g_cap.ptp_state, sizeof(g_cap.ptp_state), "%s", ps);
      g_cap.ptp_offset_us = off;
      refreshed = 1;
    }
    if (g_hooks.mtl_version) {
      const char* v = g_hooks.mtl_version();
      if (v && v[0]) {
        snprintf(g_cap.mtl_version, sizeof(g_cap.mtl_version), "%s", v);
        refreshed = 1;
      }
    }
    char* cs = dvs_capability_to_json(&g_cap);
    (void)refreshed;
    respond_cached(c, rid, cacheable, 200, "OK", "", cs ? cs : "{}");
    free(cs);
  } else if (strcmp(type, "event") == 0) {
    respond_cached(c, rid, cacheable, 200, "OK", "event", NULL);
  } else {
    respond_cached(c, rid, cacheable, 400, "INVALID_ARGUMENT", "unknown type", NULL);
  }

done:
  json_object_put(req);
  return 0;
}

/* ------------------------------------------------------------------ */
/* server loop                                                         */
/* ------------------------------------------------------------------ */

static void* client_thread(void* arg) {
  int idx = (int)(intptr_t)arg;
  dvs_client_t* c = &g_clients[idx];
  unsigned long gen;
  int fd;
  pthread_mutex_lock(&g_clients_mu);
  gen = c->gen;
  fd = c->fd;
  pthread_mutex_unlock(&g_clients_mu);

  char buf[DVS_IPC_BUF];
  size_t used = 0;
  for (;;) {
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    int pr = poll(&pfd, 1, -1);
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) break;
    if (!(pfd.revents & POLLIN)) break;

    ssize_t n = read(fd, buf + used, sizeof(buf) - 1 - used);
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
  if (g_clients[idx].gen == gen && g_clients[idx].fd == fd) {
    g_clients[idx].active = 0;
    close(fd);
  } else {
    close(fd);
  }
  pthread_mutex_unlock(&g_clients_mu);
  return NULL;
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
  if (listen(listen_fd, DVS_IPC_MAX_CLIENTS) < 0) {
    perror("listen");
    close(listen_fd);
    return -1;
  }
  /* Owner-only IPC socket: only the node user may drive media verbs. */
  chmod(sock_path, S_IRWXU);

  dvs_state_init();
  dvs_capability_probe(&g_cap);
  memset(g_clients, 0, sizeof(g_clients));
  for (int i = 0; i < DVS_IPC_MAX_CLIENTS; i++) {
    pthread_mutex_init(&g_clients[i].wmu, NULL);
    g_clients[i].fd = -1;
  }

  for (;;) {
    int client = accept(listen_fd, NULL, NULL);
    if (client < 0) {
      if (errno == EINTR) continue;
      perror("accept");
      continue;
    }

    int idx = -1;
    pthread_mutex_lock(&g_clients_mu);
    for (int i = 0; i < DVS_IPC_MAX_CLIENTS; i++) {
      if (!g_clients[i].active) {
        idx = i;
        break;
      }
    }
    if (idx >= 0) {
      g_clients[idx].fd = client;
      g_clients[idx].active = 1;
      g_clients[idx].gen++;
      idem_clear(&g_clients[idx]);
    }
    pthread_mutex_unlock(&g_clients_mu);

    if (idx < 0) {
      /* too many clients: reply inline without registering the slot */
      const char* busy =
          "{\"ok\":false,\"status\":503,\"code\":\"BUSY\",\"msg\":\"too many "
          "clients\",\"data\":{},\"requestId\":\"ipc\"}\n";
      (void)!write(client, busy, strlen(busy));
      close(client);
      continue;
    }

    pthread_t tid;
    if (pthread_create(&tid, NULL, client_thread, (void*)(intptr_t)idx) != 0) {
      pthread_mutex_lock(&g_clients_mu);
      g_clients[idx].active = 0;
      pthread_mutex_unlock(&g_clients_mu);
      close(client);
      continue;
    }
    pthread_detach(tid);
  }

  close(listen_fd);
  unlink(sock_path);
  return 0;
}
