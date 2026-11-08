#pragma once
#include "http_server.h"

struct PlatformContext;

// Serial firmware upgrade queue: enqueue via POST, progress aggregation via
// GET, and a single worker that delivers task-by-task over WS with short
// HTTP fallback. Failures isolate to one node.
bool route_upgrade(PlatformContext& ctx, const HttpRequest& req,
                   HttpResponse& res);
void start_upgrade_worker(PlatformContext& ctx);
