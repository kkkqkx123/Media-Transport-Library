#pragma once
#include "http_server.h"

struct PlatformContext;

// Docs-adjacent custom routes that need store scans beyond generic CRUD:
// scene dirty-compare, wall restore and wallpaper descriptors, playback
// transitions and multicast pool allocation. Returns true when handled.
bool route_media(PlatformContext& ctx, const HttpRequest& req,
                 HttpResponse& res);
