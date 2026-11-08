#pragma once
#include <string>

#include "http_server.h"

struct PlatformContext;

// Node gateway: register, heartbeat, per-node fetch/rename/heartbeat,
// current-task projection, retire and config-push. Returns true when the
// request path belongs to this domain (handled or rejected with envelope).
bool route_nodes(PlatformContext& ctx, const HttpRequest& req,
                 HttpResponse& res);
