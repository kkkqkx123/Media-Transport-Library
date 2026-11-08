#pragma once
#include <json-c/json.h>

#include <string>

#include "http_server.h"
#include "node_store.h"

// Single-node platform-compatible view: the agent answers the platform
// /api/v1/nodes* surface with itself so local consoles and smoke probes
// keep working when the platform is unreachable. Returns true when the
// request path was claimed (handled or rejected), false to try the next
// route. Platform gateway, barrier and upgrade fanout stay in main.cpp.
bool route_local_nodes(NodeStore* store, const HttpRequest& req,
                       HttpResponse& res);
