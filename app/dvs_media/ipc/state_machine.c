#include "state_machine.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static dvs_state_t g_state;

void dvs_state_init(void) {
  pthread_mutex_lock(&g_lock);
  memset(&g_state, 0, sizeof(g_state));
  pthread_mutex_unlock(&g_lock);
}

static void set_err(char* err, size_t errlen, const char* msg) {
  if (err && errlen > 0) snprintf(err, errlen, "%s", msg);
}

/* Single shared revision guard for every verb that moves the operation
 * forward. Caller holds g_lock. Returns 0 when the revision may proceed,
 * DVS_ERR_STALE_REVISION otherwise. */
static int check_stale_locked(int revision, char* err, size_t errlen) {
  if (revision < g_state.revision) {
    set_err(err, errlen, "stale revision");
    return DVS_ERR_STALE_REVISION;
  }
  return 0;
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
  int stale = check_stale_locked(revision, err, errlen);
  if (stale != 0) {
    pthread_mutex_unlock(&g_lock);
    return stale;
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
  int stale = check_stale_locked(revision, err, errlen);
  if (stale != 0) {
    pthread_mutex_unlock(&g_lock);
    return stale;
  }
  if (g_state.op_state != DVS_OP_PREPARED || strcmp(g_state.operation_id, opid) != 0) {
    pthread_mutex_unlock(&g_lock);
    set_err(err, errlen, "commit without successful prepare");
    return DVS_ERR_INVALID_STATE;
  }
  g_state.revision = revision;
  g_state.op_state = DVS_OP_IDLE;
  g_state.prepared_revision = 0;
  if (applied_rev) *applied_rev = g_state.revision;
  pthread_mutex_unlock(&g_lock);
  return 0;
}

int dvs_state_abort(const char* opid) {
  pthread_mutex_lock(&g_lock);
  if (g_state.op_state != DVS_OP_PREPARING && g_state.op_state != DVS_OP_PREPARED) {
    pthread_mutex_unlock(&g_lock);
    return DVS_ERR_INVALID_STATE;
  }
  if (opid && opid[0] && strcmp(g_state.operation_id, opid) != 0) {
    pthread_mutex_unlock(&g_lock);
    return DVS_ERR_INVALID_ARG;
  }
  g_state.op_state = DVS_OP_IDLE;
  g_state.operation_id[0] = '\0';
  g_state.prepared_revision = 0;
  pthread_mutex_unlock(&g_lock);
  return 0;
}

void dvs_state_clear(void) {
  pthread_mutex_lock(&g_lock);
  g_state.op_state = DVS_OP_IDLE;
  g_state.operation_id[0] = '\0';
  g_state.prepared_revision = 0;
  pthread_mutex_unlock(&g_lock);
}

void dvs_state_snapshot(dvs_state_t* out) {
  if (!out) return;
  pthread_mutex_lock(&g_lock);
  *out = g_state;
  pthread_mutex_unlock(&g_lock);
}
