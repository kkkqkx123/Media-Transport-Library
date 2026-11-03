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

int main(void) {
  char err[128];
  int applied = 0;

  dvs_state_init();
  CHECK(dvs_state_get_revision() == 0, "init revision is 0");

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

  /* stale revision */
  dvs_state_abort("op1");
  dvs_state_set_revision(5);
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
  CHECK(dvs_state_get_revision() == 6, "revision persisted");

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
