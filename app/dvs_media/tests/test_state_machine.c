/* Unit tests for the dvs_media IPC state machine (no external deps). */
#include "../ipc/state_machine.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(cond, name)                                                     \
  do {                                                                        \
    if (cond) {                                                               \
      printf("PASS: %s\n", name);                                             \
    } else {                                                                  \
      printf("FAIL: %s\n", name);                                             \
      failures++;                                                             \
    }                                                                         \
  } while (0)

static int snapshot_rev(void) {
  dvs_state_t s;
  dvs_state_snapshot(&s);
  return s.revision;
}

int main(void) {
  char err[128];
  int applied = 0;

  dvs_state_init();
  CHECK(snapshot_rev() == 0, "init revision is 0");

  /* missing operationId */
  CHECK(dvs_state_begin_prepare(NULL, 1, err, sizeof(err)) == DVS_ERR_INVALID_ARG,
        "prepare without operationId rejected");

  /* normal prepare */
  CHECK(dvs_state_begin_prepare("op1", 1, err, sizeof(err)) == 0, "prepare accepted");
  CHECK(dvs_state_begin_prepare("op1", 1, err, sizeof(err)) == 0,
        "idempotent prepare replay accepted");
  {
    dvs_state_t s;
    dvs_state_snapshot(&s);
    CHECK(s.op_state == DVS_OP_PREPARING, "state is PREPARING");
  }
  CHECK(dvs_state_finish_prepare("op1") == 0, "finish_prepare ok");
  CHECK(dvs_state_finish_prepare("op1") == DVS_ERR_INVALID_STATE,
        "double finish_prepare rejected");

  /* commit without prepare on a different operation */
  CHECK(dvs_state_commit("op2", 2, &applied, err, sizeof(err)) == DVS_ERR_INVALID_STATE,
        "commit without matching prepare rejected");

  /* abort the prepared op, then drive revision to 5 via transitions */
  CHECK(dvs_state_abort("op1") == 0, "abort prepared op succeeds");
  CHECK(dvs_state_begin_prepare("op-base", 5, err, sizeof(err)) == 0, "prepare base rev5");
  CHECK(dvs_state_finish_prepare("op-base") == 0, "finish base");
  CHECK(dvs_state_commit("op-base", 5, &applied, err, sizeof(err)) == 0 && applied == 5,
        "commit base rev5");
  CHECK(dvs_state_begin_prepare("op3", 4, err, sizeof(err)) == DVS_ERR_STALE_REVISION,
        "stale revision prepare rejected");
  CHECK(dvs_state_commit("op3", 4, &applied, err, sizeof(err)) == DVS_ERR_STALE_REVISION,
        "stale revision commit rejected");

  /* full prepare -> commit */
  CHECK(dvs_state_begin_prepare("op4", 6, err, sizeof(err)) == 0, "prepare op4");
  CHECK(dvs_state_finish_prepare("op4") == 0, "finish op4");
  CHECK(dvs_state_commit("op4", 6, &applied, err, sizeof(err)) == 0, "commit op4");
  CHECK(applied == 6, "commit applied revision 6");
  CHECK(dvs_state_commit("op4", 6, &applied, err, sizeof(err)) == 0 && applied == 6,
        "idempotent commit replay");
  CHECK(snapshot_rev() == 6, "revision persisted");

  /* concurrent operation while one is in flight */
  CHECK(dvs_state_begin_prepare("op5", 7, err, sizeof(err)) == 0, "prepare op5");
  CHECK(dvs_state_begin_prepare("op6", 8, err, sizeof(err)) == DVS_ERR_INVALID_STATE,
        "second concurrent prepare rejected");
  dvs_state_fail_prepare("op5");
  {
    dvs_state_t s;
    dvs_state_snapshot(&s);
    CHECK(s.op_state == DVS_OP_IDLE, "fail_prepare returns to IDLE");
  }

  /* abort matrix: idle refuses, mismatch refuses, happy path clears */
  CHECK(dvs_state_abort("anything") == DVS_ERR_INVALID_STATE,
        "abort on idle rejected");
  CHECK(dvs_state_abort(NULL) == DVS_ERR_INVALID_STATE,
        "abort with no op on idle rejected");
  CHECK(dvs_state_begin_prepare("opA", 7, err, sizeof(err)) == 0, "prepare opA");
  CHECK(dvs_state_abort("opB") == DVS_ERR_INVALID_ARG,
        "abort with mismatched op rejected");
  {
    dvs_state_t s;
    dvs_state_snapshot(&s);
    CHECK(s.op_state == DVS_OP_PREPARING, "mismatched abort keeps PREPARING");
  }
  CHECK(dvs_state_abort(NULL) == 0, "abort without opid aborts in-flight op");
  {
    dvs_state_t s;
    dvs_state_snapshot(&s);
    CHECK(s.op_state == DVS_OP_IDLE && s.operation_id[0] == '\0',
          "abort clears in-flight op");
  }
  CHECK(dvs_state_begin_prepare("opB", 7, err, sizeof(err)) == 0, "prepare opB");
  CHECK(dvs_state_finish_prepare("opB") == 0, "finish opB");
  CHECK(dvs_state_abort("opB") == 0, "abort prepared op succeeds");
  CHECK(dvs_state_commit("opB", 7, &applied, err, sizeof(err)) == DVS_ERR_INVALID_STATE,
        "commit after abort rejected");

  /* clear is idempotent */
  dvs_state_clear();
  dvs_state_clear();
  {
    dvs_state_t s;
    dvs_state_snapshot(&s);
    CHECK(s.op_state == DVS_OP_IDLE && s.operation_id[0] == '\0', "clear resets state");
  }

  printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
  return failures ? 1 : 0;
}
