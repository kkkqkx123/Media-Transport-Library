#ifndef DVS_STATE_MACHINE_H
#define DVS_STATE_MACHINE_H

typedef struct { int revision; int state; } dvs_state_t;
void dvs_state_init(void);
int dvs_state_get_revision(void);
void dvs_state_set_revision(int r);
const char* dvs_state_get_name(void);
void dvs_state_set_name(const char* name);

#endif
