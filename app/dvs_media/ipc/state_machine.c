#include "state_machine.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static dvs_state_t g_state;

static void set_name_locked(const char* name) {
  if (!name) {
    g_state.state = 0;
    return;
  }
  if (strcmp(name, "prepared") == 0)
    g_state.state = 1;
  else if (strcmp(name, "committed") == 0)
    g_state.state = 2;
  else
    g_state.state = 0;
}

void dvs_state_init(void) {
  pthread_mutex_lock(&g_lock);
  memset(&g_state, 0, sizeof(g_state));
  pthread_mutex_unlock(&g_lock);
}

int dvs_state_get_revision(void) {
  pthread_mutex_lock(&g_lock);
  int r = g_state.revision;
  pthread_mutex_unlock(&g_lock);
  return r;
}

void dvs_state_set_revision(int r) {
  pthread_mutex_lock(&g_lock);
  g_state.revision = r;
  pthread_mutex_unlock(&g_lock);
}

const char* dvs_state_get_name(void) {
  pthread_mutex_lock(&g_lock);
  const char* name;
  switch (g_state.state) {
    case 1: name = "prepared"; break;
    case 2: name = "committed"; break;
    default: name = "idle"; break;
  }
  pthread_mutex_unlock(&g_lock);
  return name;
}

void dvs_state_set_name(const char* name) {
  pthread_mutex_lock(&g_lock);
  set_name_locked(name);
  pthread_mutex_unlock(&g_lock);
}

static void set_err(char* err, size_t errlen, const char* msg) {
  if (err && errlen > 0) snprintf(err, errlen, "%s", msg);
}

int dvs_state_begin_prepare(const char* opid, int revision, char* err, size_t errlen) {
  if (!opid || !opid[0]) {
    set_err(err, errlen, "missing operationId");
    return DVS_ERR_INVALID_ARG;
  }
  if (revision < 0) {
    set_err(err, errlen, "invalid revision");
    return DVS_ERR_INVALID_ARG;
  }

  pthread_mutex_lock(&g_lock);
  /* idempotent replay of the current prepare */
  if (g_state.op_state != DVS_OP_IDLE && strcmp(g_state.operation_id, opid) == 0) {
    if (revision == g_state.prepared_revision) {
      pthread_mutex_unlock(&g_lock);
      return 0;
    }
  }
  if (revision < g_state.revision) {
    pthread_mutex_unlock(&g_lock);
    set_err(err, errlen, "stale revision");
    return DVS_ERR_STALE_REVISION;
  }
  if (g_state.op_state != DVS_OP_IDLE) {
    /* a different operation is in flight */
    pthread_mutex_unlock(&g_lock);
    set_err(err, errlen, "another operation in progress");
    return DVS_ERR_INVALID_STATE;
  }
  snprintf(g_state.operation_id, sizeof(g_state.operation_id), "%s", opid);
  g_state.prepared_revision = revision;
  g_state.op_state = DVS_OP_PREPARING;
  set_name_locked("preparing");
  pthread_mutex_unlock(&g_lock);
  return 0;
}

int dvs_state_finish_prepare(const char* opid) {
  if (!opid || !opid[0]) return DVS_ERR_INVALID_ARG;
  pthread_mutex_lock(&g_lock);
  if (g_state.op_state != DVS_OP_PREPARING || strcmp(g_state.operation_id, opid) != 0) {
    pthread_mutex_unlock(&g_lock);
    return DVS_ERR_INVALID_STATE;
  }
  g_state.op_state = DVS_OP_PREPARED;
  set_name_locked("prepared");
  pthread_mutex_unlock(&g_lock);
  return 0;
}

void dvs_state_fail_prepare(const char* opid) {
  if (!opid) return;
  pthread_mutex_lock(&g_lock);
  if (g_state.op_state == DVS_OP_PREPARING && strcmp(g_state.operation_id, opid) == 0) {
    g_state.op_state = DVS_OP_IDLE;
    g_state.operation_id[0] = '\0';
    g_state.prepared_revision = 0;
    set_name_locked("idle");
  }
  pthread_mutex_unlock(&g_lock);
}

int dvs_state_commit(const char* opid, int revision, int* applied_rev,
                     char* err, size_t errlen) {
  if (!opid || !opid[0]) {
    set_err(err, errlen, "missing operationId");
    return DVS_ERR_INVALID_ARG;
  }

  pthread_mutex_lock(&g_lock);
  /* idempotent replay of the last committed operation */
  if (g_state.op_state == DVS_OP_IDLE && strcmp(g_state.operation_id, opid) == 0 &&
      revision == g_state.revision) {
    if (applied_rev) *applied_rev = g_state.revision;
    pthread_mutex_unlock(&g_lock);
    return 0;
  }
  if (revision < g_state.revision) {
    pthread_mutex_unlock(&g_lock);
    set_err(err, errlen, "stale revision");
    return DVS_ERR_STALE_REVISION;
  }
  if (g_state.op_state != DVS_OP_PREPARED || strcmp(g_state.operation_id, opid) != 0) {
    pthread_mutex_unlock(&g_lock);
    set_err(err, errlen, "commit without successful prepare");
    return DVS_ERR_INVALID_STATE;
  }
  g_state.revision = revision;
  g_state.op_state = DVS_OP_IDLE;
  g_state.prepared_revision = 0;
  set_name_locked("committed");
  if (applied_rev) *applied_rev = g_state.revision;
  pthread_mutex_unlock(&g_lock);
  return 0;
}

void dvs_state_abort(const char* opid) {
  pthread_mutex_lock(&g_lock);
  if (!opid || !opid[0] || strcmp(g_state.operation_id, opid) == 0) {
    g_state.op_state = DVS_OP_IDLE;
    g_state.operation_id[0] = '\0';
    g_state.prepared_revision = 0;
    set_name_locked("idle");
  }
  pthread_mutex_unlock(&g_lock);
}

void dvs_state_clear(void) {
  pthread_mutex_lock(&g_lock);
  g_state.op_state = DVS_OP_IDLE;
  g_state.operation_id[0] = '\0';
  g_state.prepared_revision = 0;
  set_name_locked("idle");
  pthread_mutex_unlock(&g_lock);
}

void dvs_state_snapshot(dvs_state_t* out) {
  if (!out) return;
  pthread_mutex_lock(&g_lock);
  *out = g_state;
  pthread_mutex_unlock(&g_lock);
}
