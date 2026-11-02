#include "state_machine.h"

#include <string.h>

static dvs_state_t g_state = {0, 0};

void dvs_state_init(void) {
  g_state.revision = 0;
  g_state.state = 0;
}

int dvs_state_get_revision(void) { return g_state.revision; }
void dvs_state_set_revision(int r) { g_state.revision = r; }

const char* dvs_state_get_name(void) {
  switch (g_state.state) {
    case 1: return "prepared";
    case 2: return "committed";
    default: return "idle";
  }
}
void dvs_state_set_name(const char* name) {
  if (!name) return;
  if (strcmp(name, "prepared") == 0) g_state.state = 1;
  else if (strcmp(name, "committed") == 0) g_state.state = 2;
  else g_state.state = 0;
}
