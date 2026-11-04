#ifndef DVS_STATE_MACHINE_H
#define DVS_STATE_MACHINE_H

#include <stddef.h>

/* operation lifecycle tracked alongside the coarse engine state */
typedef enum {
  DVS_OP_IDLE = 0,
  DVS_OP_PREPARING,  /* prepare accepted, result not yet delivered */
  DVS_OP_PREPARED,   /* prepare finished, waiting for commit */
} dvs_op_state_t;

/* error codes returned by the transition helpers */
#define DVS_ERR_INVALID_ARG      (-1)
#define DVS_ERR_STALE_REVISION   (-2)
#define DVS_ERR_INVALID_STATE    (-3)
#define DVS_ERR_INTERNAL         (-4)

typedef struct {
  int revision;            /* last applied (committed) revision */
  char operation_id[64];   /* current / most recent operation */
  dvs_op_state_t op_state;
  int prepared_revision;   /* revision accepted by the last successful prepare */
} dvs_state_t;

void dvs_state_init(void);

/* Thread-safe transition helpers. On success return 0; on failure return a
 * negative DVS_ERR_* code and fill err (when provided) with a short reason. */
int dvs_state_begin_prepare(const char* opid, int revision, char* err, size_t errlen);
int dvs_state_finish_prepare(const char* opid);
void dvs_state_fail_prepare(const char* opid);
int dvs_state_commit(const char* opid, int revision, int* applied_rev,
                     char* err, size_t errlen);
int dvs_state_abort(const char* opid);
void dvs_state_clear(void);
void dvs_state_snapshot(dvs_state_t* out);

#endif
