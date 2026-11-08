#pragma once
#include <string>

#include "http_server.h"

struct PlatformContext;

// Long-lived WS channel from a node: handshake, heartbeat, barrier acks,
// upgrade/config acks. Owns the fd from handshake until close.
void ws_channel_loop(PlatformContext& ctx, int fd, HttpRequest hs_req);
