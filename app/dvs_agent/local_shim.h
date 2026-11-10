#pragma once
#include <json-c/json.h>

#include <string>

#include "http_server.h"
#include "node_store.h"

// Single-node debug shim: answers the platform /api/v1/nodes* surface with
// the local node so consoles keep working when the platform is unreachable.
// Read-only routes (GET list/get) are always available as diagnostics.
// Mutating routes (PUT rename, POST retire/upgrade/heartbeat) are disabled
// in production with DVS_DISABLE_LOCAL_SHIM=1 and return false so the
// caller falls through to 404. Returns true when claimed.
bool route_local_nodes(NodeStore* store, const HttpRequest& req,
                       HttpResponse& res);
