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
