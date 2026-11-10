// dvs-agent entry: argument parsing, dependency wiring, route dispatch.
//
// Responsibility of this file is bootstrap only. Request handling lives in
// focused modules, each owning one reason to change:
//   agent_auth          browser sessions, login rate limit, auth gate/routes
//   agent_audit         bounded in-memory operational audit ring
//   agent_media         media-engine liaison (version gate, channels, caps,
//                       hotplug, playback projection, barrier reporter)
//   agent_config_routes six scope handlers + platform config/sync + retry
//   agent_media_routes  engine passthrough surface (/media/*)
//   agent_sys_routes    node info/role/capability/capacity/version,
//                       register/upload, maintenance, audit-events
//   agent_downlink      WS downlink dispatch (layout/config/upgrade)
//   agent_context       shared AgentContext + tiny json/time/http helpers
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <chrono>

#include "agent_audit.h"
#include "agent_auth.h"
#include "agent_config_routes.h"
#include "agent_context.h"
#include "agent_downlink.h"
#include "agent_media.h"
#include "agent_media_routes.h"
#include "agent_sys_routes.h"
#include "dvs_contract.h"
#include "dvs_node_bundle.h"
#include "http_server.h"
#include "local_shim.h"
#include "maintenance.h"
#include "media_ipc.h"
#include "node_store.h"
#include "registration.h"
#include "dvs_http_json.h"

// Heap-owned, never freed: detached reporter/config threads and the HTTP
// handler outlive main's frame, so the context must not sit on the stack.
static AgentContext* g_ctx = nullptr;

int main(int argc, char** argv) {
  std::string db_path = "/var/lib/dvs_agent/dvs_agent.db";
  std::string platform_url;
  std::string media_sock;
  std::string web_root;
  int port = 8080;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--version") {
      printf("dvs-agent bundle %s ipc %d\n", DVS_NODE_BUNDLE_VERSION,
             DVS_MEDIA_IPC_VERSION);
      return 0;
    }
    if (a == "--db" && i + 1 < argc)
      db_path = argv[++i];
    else if (a == "--port" && i + 1 < argc)
      port = std::stoi(argv[++i]);
    else if (a == "--platform-url" && i + 1 < argc)
      platform_url = argv[++i];
    else if (a == "--media-sock" && i + 1 < argc)
      media_sock = argv[++i];
    else if (a == "--web-root" && i + 1 < argc)
      web_root = argv[++i];
  }
  if (web_root.empty()) web_root = dvs::env_or("DVS_WEB_ROOT", "./web/src");
  if (media_sock.empty()) media_sock = dvs::env_or("DVS_MEDIA_SOCK", "");
  if (platform_url.empty())
    platform_url = dvs::env_or("DVS_PLATFORM_URL", "");

  g_ctx = new AgentContext();
  AgentContext& ctx = *g_ctx;
  ctx.store = new NodeStore(db_path);
  ctx.media = new MediaIpcClient(media_sock);
  ctx.media_q = new MediaIpcClient(media_sock);
  ctx.maint = new Maintenance(ctx.store, ctx.media);
  ctx.web_root = web_root;
  ctx.platform_url = platform_url;
  ctx.api_token = (std::getenv("DVS_API_TOKEN") ? std::getenv("DVS_API_TOKEN")
                                               : "");
  // Closed by default: an empty platform token disables the bearer channel
  // (browser login still works). DVS_ALLOW_INSECURE=1 restores the debug
  // bypass for single-node debugging only.
  if (agent_auth_insecure()) {
    fprintf(stderr,
            "dvs-agent: WARNING insecure mode (DVS_ALLOW_INSECURE=1), "
            "auth gate disabled\n");
  } else if (ctx.api_token.empty()) {
    fprintf(stderr,
            "dvs-agent: WARNING DVS_API_TOKEN empty, platform bearer "
            "channel disabled (browser login still required)\n");
  }

  // Bundle handshake: warn-only, the engine may start later. The verdict
  // seeds the continuous version gate in agent_media.
  {
    std::string hello = agent_media_query(ctx, "hello", nullptr);
    bool answered = false, match = false;
    std::string eb;
    int ei = -1;
    MediaIpcClient::parse_hello(hello, answered, match, eb, ei);
    agent_media_note_hello(ctx, hello);
    if (match)
      printf("dvs-agent: media engine bundle %s ipc %d confirmed\n",
             DVS_NODE_BUNDLE_VERSION, DVS_MEDIA_IPC_VERSION);
    else
      printf("dvs-agent: WARNING media engine handshake mismatch "
             "(see /api/v1/node/version)\n");
    fflush(stdout);
  }

  std::string agent_base = dvs::env_or("DVS_AGENT_BASE", "");
  if (agent_base.empty()) {
    // Reachable-base advertisement must never use the loopback placeholder.
    // Without an operator-configured base or a real network address the base
    // stays empty and the platform treats the node as address-unknown.
    std::string ip;
    json_object* nd = ctx.store->node();
    json_object* net = nullptr;
    if (nd && json_object_object_get_ex(nd, "network", &net) && net) {
      json_object* n2 = nullptr;
      if (json_object_object_get_ex(net, "ip", &n2) && n2 &&
          json_object_is_type(n2, json_type_string))
        ip = json_object_get_string(n2);
    }
    if (ip.empty() || ip == "127.0.0.1" || ip == "::1") ip.clear();
    if (!ip.empty()) agent_base = "http://" + ip + ":" + std::to_string(port);
  }
  ctx.reg =
      new Registration(ctx.store, ctx.media_q, platform_url, 3000, agent_base);
  agent_install_downlink(ctx);

  agent_refresh_capability(ctx);
  agent_init_cap_sig(ctx);
  std::thread([&]() { agent_hotplug_loop(ctx); }).detach();

  json_object* cs = ctx.store->all_states();
  json_object* patch = json_object_new_object();
  json_object_object_add(patch, "configStates", cs);
  json_object* cr = ctx.store->config_revision();
  json_object_object_add(patch, "configRevision", cr);
  ctx.store->update_node(patch);
  json_object_put(patch);

  // The agent owns its origin root: / serves the node-admin frontend and
  // /api serves the node interfaces. Never mount it under a sub-path and
  // never let the browser call the platform origin directly.
  HttpServer::Handler handler = [&](const HttpRequest& req,
                                    HttpResponse& res) {
    if (agent_handle_auth(ctx, req, res)) return;
    if (!agent_authorized(ctx, req)) {
      dvs::send_json(res, dvs::envelope_fail("UNAUTHORIZED", 401,
                                              "未登录或缺少令牌", "a"));
      return;
    }
    if (agent_handle_config(ctx, req, res)) return;
    if (route_local_nodes(ctx.store, req, res)) return;
    if (agent_handle_media(ctx, req, res)) return;
    if (agent_handle_sys(ctx, req, res)) return;
    if (req.method == "GET" &&
        HttpServer::serve_static_file(ctx.web_root, req.path, res))
      return;
    dvs::send_json(res, dvs::envelope_fail(
                             "NOT_FOUND", 404,
                             ("未定义的 API：" + req.method + " " + req.path)
                                 .c_str(), "a"));
  };

  HttpServer server(port, handler);
  server.start();
  printf("dvs-agent listening on 0.0.0.0:%d\n", port);
  fflush(stdout);

  ctx.reg->start();

  while (true) std::this_thread::sleep_for(std::chrono::seconds(3600));
  return 0;
}
