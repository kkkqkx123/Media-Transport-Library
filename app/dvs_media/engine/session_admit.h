#ifndef _DVS_SESSION_ADMIT_H_
#define _DVS_SESSION_ADMIT_H_

#include <stdint.h>

/* Per-session admission: maintains (mcast_ip, udp_port) -> lane mapping with a
 * whitelist and an 8-slot LRU table (req 03 §7.3 / S4). Self-contained: no
 * MTL/MPP dependency, so it builds and can be unit-tested on x86. */

#define DVS_MAX_SESSIONS 8
#define DVS_SESSION_KEY_LEN 64

/* Set the allowed-sources whitelist. csv is a comma-separated list of
 * "ip:port" (exact) or "ip" (any port on that host). NULL/"" => allow all. */
void dvs_session_whitelist_set(const char* csv);

/* Check whether a source is allowed by the current whitelist. Returns 1 if
 * allowed (or whitelist disabled), 0 if rejected. */
int dvs_session_whitelist_check(const char* mcast_ip, uint16_t port);

/* Admit a source. On success stores/refreshes its lane and writes it to
 * *lane_out, returning 0. Rejects (return -1) if not whitelisted, or evicts
 * the least-recently-used active session when the table is full. */
int dvs_session_admit(const char* mcast_ip, uint16_t port, int* lane_out);

/* Release a previously admitted source. */
void dvs_session_release(const char* mcast_ip, uint16_t port);

/* Snapshot active sessions into caller arrays. Returns active count.
 * Each entry pairs the session key with its lane. Empty slots omitted. */
int dvs_session_snapshot(char keys[][DVS_SESSION_KEY_LEN], int* lanes, int max);

/* Advance the LRU clock (call once per admission cycle). */
void dvs_session_tick(void);

/* Number of currently active sessions. */
int dvs_session_count(void);

#endif /* _DVS_SESSION_ADMIT_H_ */
