#pragma once
#include <json-c/json.h>

#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "http_server.h"
#include "ops.h"
#include "platform_store.h"

// Document CRUD router: generic docs mounted at /api/v1/<kind>[/<id>].
// Dependencies are injected so main.cpp keeps owning the store, the op
// coordinator and their locks.
struct DocsCtx {
    PlatformStore* store = nullptr;
    OpCoordinator* ops = nullptr;
    std::mutex* ops_mu = nullptr;
    std::mutex* alloc_mu = nullptr;
};

// Returns true when the path belongs to a known kind (handled or rejected
// with an envelope), false to let the caller try the next route.
bool route_docs(DocsCtx& ctx, const std::string& kind, const HttpRequest& req,
                HttpResponse& res);
