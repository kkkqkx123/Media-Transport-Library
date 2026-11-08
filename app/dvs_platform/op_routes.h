#pragma once
#include "http_server.h"

struct PlatformContext;

// Cross-node transaction barrier: prepare fanout, commit acks, short-conn
// acks and op status query (including long-poll). Returns true when handled.
bool route_ops(PlatformContext& ctx, const HttpRequest& req, HttpResponse& res);
