#pragma once
#include "http_server.h"

struct PlatformContext;

// Top-level HTTP dispatch: unified mutating write gate, then domain routers
// (nodes, ops barrier, media, upgrade), then generic docs CRUD and static
// files. Returns via res in all cases.
void handle_platform_request(PlatformContext& ctx, const HttpRequest& req,
                             HttpResponse& res);
