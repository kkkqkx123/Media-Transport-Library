#pragma once

/* DVS node bundle contract shared by dvs_agent and dvs_media.
 *
 * Single source of truth for the co-released node bundle version and the
 * media IPC protocol version. Both processes are versioned and released as
 * one bundle; IPC peers compare these values in the hello handshake instead
 * of assuming a matching counterpart. Plain defines only, so the header is
 * consumable from both C (dvs_media) and C++ (dvs_agent). */

/* Bundle release line. Stays at 1 during development. */
#define DVS_NODE_BUNDLE_VERSION "1"

/* Media engine IPC protocol generation. Bumped only with an incompatible
 * change to the JSON line protocol; both peers reject unknown generations. */
#define DVS_MEDIA_IPC_VERSION 1

/* Engine transport lane budget (output windows per node). Single source for
 * the agent backstop; the tighter per-node admission budget lives on the
 * platform and is enforced before fanout. */
#define DVS_MAX_ENGINE_LANES 8

/* Output port absolute hardware redlines (RK3588 wall). Used only when a
 * reported per-port ceiling is absent; a missing capability report must
 * reject the output change instead of falling back to these. */
#define DVS_OUT_MAX_W 7680
#define DVS_OUT_MAX_H 4320
#define DVS_OUT_MAX_HZ 60
#define DVS_OUT_MIN_W 640
#define DVS_OUT_MIN_H 480
#define DVS_OUT_MIN_HZ 24

/* Capability freshness defaults (ms). Overridable via DVS_VER_CACHE_MS /
 * DVS_CAP_STALE_MS / DVS_HOTPLUG_INTERVAL_MS for field tuning. */
#define DVS_VER_CACHE_MS_DEFAULT 5000
#define DVS_CAP_STALE_MS_DEFAULT 30000
#define DVS_HOTPLUG_INTERVAL_MS_DEFAULT 10000
