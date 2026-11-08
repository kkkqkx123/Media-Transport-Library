#include "platform_server.h"

#include "docs_routes.h"
#include "http_json.h"
#include "media_routes.h"
#include "node_routes.h"
#include "op_routes.h"
#include "platform_context.h"
#include "platform_store.h"
#include "upgrade_jobs.h"

void handle_platform_request(PlatformContext& ctx, const HttpRequest& req,
                             HttpResponse& res) {
    if (req.path.rfind("/api/", 0) == 0 && req.method != "GET" &&
        !http_bearer_ok(req, ctx.api_token)) {
        send_json(res,
                  fail_envelope("UNAUTHORIZED", 401, "missing or bad token"));
        return;
    }
    if (route_nodes(ctx, req, res)) return;
    if (route_ops(ctx, req, res)) return;
    if (route_upgrade(ctx, req, res)) return;
    if (route_media(ctx, req, res)) return;
    if (req.method == "GET" && req.path == "/api/v1/audit-events") {
        send_json(res, ok_envelope(ctx.store->audit_list(100), ""));
        return;
    }
    const char* kinds[] = {"groups", "walls", "scenes", "bindings",
                           "whitelist", "playback-tasks"};
    DocsCtx docs_ctx{ctx.store, &ctx.ops, &ctx.ops_mu};
    for (auto k : kinds)
        if (route_docs(docs_ctx, k, req, res)) return;
    if (req.method == "GET" &&
        HttpServer::serve_static_file(ctx.web_root, req.path, res))
        return;
    send_json(res, fail_envelope("NOT_FOUND", 404, "unknown api"));
}
