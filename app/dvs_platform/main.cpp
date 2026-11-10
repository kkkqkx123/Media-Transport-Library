// dvs_platform main: service wiring only. Transport sessions, node state,
// layout fanout, barrier, routes, upgrades and sweepers live in their own
// modules; main owns the single PlatformContext, restores persisted ops,
// starts background workers and serves HTTP/WS.
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

#include "http_server.h"
#include "ops_sweeper.h"
#include "platform_context.h"
#include "platform_server.h"
#include "platform_store.h"
#include "upgrade_jobs.h"
#include "ws_channel.h"

int main(int argc, char** argv) {
    std::string db = "/var/lib/dvs_platform/dvs_platform.db";
    int port = 8081;
    std::string web_root_arg;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--db" && i + 1 < argc)
            db = argv[++i];
        else if (a == "--port" && i + 1 < argc)
            port = atoi(argv[++i]);
        else if (a == "--web-root" && i + 1 < argc)
            web_root_arg = argv[++i];
    }

    PlatformStore store(db);
    PlatformContext ctx;
    ctx.store = &store;
    ctx.ops.set_store(&store);
    if (const char* tok = std::getenv("DVS_API_TOKEN")) ctx.api_token = tok;

    restore_ops(ctx);

    if (!web_root_arg.empty()) ctx.web_root = web_root_arg;
    if (ctx.web_root.empty()) {
        if (const char* env = getenv("DVS_WEB_ROOT"))
            if (env && *env) ctx.web_root = env;
    }
    if (ctx.web_root.empty()) ctx.web_root = "./web/src";
    if (ctx.api_token.empty())
        fprintf(stderr,
                "dvs-platform: WARNING DVS_API_TOKEN unset, mutating APIs "
                "accept unauthenticated calls\n");

    HttpServer server(port, [&ctx](const HttpRequest& req, HttpResponse& res) {
        handle_platform_request(ctx, req, res);
    });
    server.set_ws_handler("/api/v1/channel/ws",
                          [&ctx](int fd, const HttpRequest& hs_req) {
                              ws_channel_loop(ctx, fd, hs_req);
                          });
    start_barrier_sweeper(ctx);
    start_upgrade_worker(ctx);
    server.start();
    printf("dvs-platform listening on 0.0.0.0:%d\n", port);
    fflush(stdout);
    for (;;) pause();
    return 0;
}
