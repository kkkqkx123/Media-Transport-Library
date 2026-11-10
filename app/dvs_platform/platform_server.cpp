#include "platform_server.h"

#include "docs_routes.h"
#include "dvs_http_json.h"
#include "media_routes.h"
#include "node_routes.h"
#include "op_routes.h"
#include "platform_context.h"
#include "platform_store.h"
#include "upgrade_jobs.h"

// The platform owns a separate origin root: / serves the management
// frontend and /api serves the platform interfaces. Cross-system browser
// navigation uses hyperlinks to the stored agent base, never cross-origin
// api calls; node writes always go through the platform backend.
void handle_platform_request(PlatformContext& ctx, const HttpRequest& req,
                             HttpResponse& res) {
    if (req.path.rfind("/api/", 0) == 0 && req.method != "GET" &&
        !platform_mutation_allowed(ctx, req)) {
        dvs::send_json(res,
                  dvs::envelope_fail("UNAUTHORIZED", 401, "missing or bad token"));
        return;
    }
    if (route_nodes(ctx, req, res)) return;
    if (route_ops(ctx, req, res)) return;
    if (route_upgrade(ctx, req, res)) return;
    if (route_media(ctx, req, res)) return;
    if (req.method == "GET" && req.path == "/api/v1/audit-events") {
        dvs::send_json(res, dvs::envelope_ok(ctx.store->audit_list(100), ""));
        return;
    }
    const char* kinds[] = {"groups", "walls", "scenes", "bindings",
                           "whitelist", "playback-tasks"};
    DocsCtx docs_ctx{ctx.store, &ctx.ops, &ctx.ops_mu, &ctx.alloc_mu};
    for (auto k : kinds)
        if (route_docs(docs_ctx, k, req, res)) return;
    if (req.method == "GET" &&
        HttpServer::serve_static_file(ctx.web_root, req.path, res))
        return;
    dvs::send_json(res, dvs::envelope_fail("NOT_FOUND", 404, "unknown api"));
}
