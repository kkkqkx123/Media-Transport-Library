#include <json-c/json.h>
#include <sqlite3.h>

#include "node_store.h"
#include "media_ipc.h"
#include "http_server.h"
#include "maintenance.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <chrono>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

static NodeStore* g_store = nullptr;
static MediaIpcClient* g_media = nullptr;
static Maintenance* g_maint = nullptr;
static std::string g_web_root;

static std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string guess_ct(const std::string& path) {
    if (path.find(".html") != std::string::npos) return "text/html; charset=utf-8";
    if (path.find(".css") != std::string::npos) return "text/css; charset=utf-8";
    if (path.find(".js") != std::string::npos) return "text/javascript; charset=utf-8";
    if (path.find(".json") != std::string::npos) return "application/json; charset=utf-8";
    if (path.find(".svg") != std::string::npos) return "image/svg+xml";
    if (path.find(".png") != std::string::npos) return "image/png";
    return "application/octet-stream";
}

static std::string env_or(const char* k, const char* d) {
    const char* v = std::getenv(k);
    return v ? std::string(v) : std::string(d ? d : "");
}

static json_object* envelope_ok(json_object* data, const char* msg) {
    json_object* o = json_object_new_object();
    json_object_object_add(o, "ok", json_object_new_boolean(1));
    json_object_object_add(o, "status", json_object_new_int(200));
    json_object_object_add(o, "code", json_object_new_string("OK"));
    json_object_object_add(o, "msg", json_object_new_string(msg ? msg : ""));
    json_object_object_add(o, "data", data ? data : json_object_new_object());
    json_object_object_add(o, "requestId", json_object_new_string("r-1"));
    return o;
}

static json_object* envelope_fail(const char* code, int status, const char* msg) {
    json_object* o = json_object_new_object();
    json_object_object_add(o, "ok", json_object_new_boolean(0));
    json_object_object_add(o, "status", json_object_new_int(status));
    json_object_object_add(o, "code", json_object_new_string(code));
    json_object_object_add(o, "msg", json_object_new_string(msg ? msg : code));
    json_object_object_add(o, "data", json_object_new_object());
    json_object_object_add(o, "requestId", json_object_new_string("r-1"));
    return o;
}

static void send_json(HttpResponse& res, json_object* o) {
    const char* s = json_object_to_json_string(o);
    res.body = s ? s : "{}";
    res.content_type = "application/json; charset=utf-8";
    json_object_put(o);
}

static std::string get_str(json_object* o, const char* key) {
    json_object* v = nullptr;
    if (o && json_object_object_get_ex(o, key, &v) && v) return json_object_get_string(v);
    return "";
}

int main(int argc, char** argv) {
    std::string db_path = "/tmp/dvs_agent.db";
    std::string platform_url;
    std::string media_sock;
    int port = 8080;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--db" && i + 1 < argc) db_path = argv[++i];
        else if (a == "--port" && i + 1 < argc) port = std::stoi(argv[++i]);
        else if (a == "--platform-url" && i + 1 < argc) platform_url = argv[++i];
        else if (a == "--media-sock" && i + 1 < argc) media_sock = argv[++i];
        else if (a == "--web-root" && i + 1 < argc) g_web_root = argv[++i];
    }
    if (g_web_root.empty()) g_web_root = env_or("DVS_WEB_ROOT", "./web/src");
    if (media_sock.empty()) media_sock = env_or("DVS_MEDIA_SOCK", "");
    if (platform_url.empty()) platform_url = env_or("DVS_PLATFORM_URL", "");

    g_store = new NodeStore(db_path);
    g_media = new MediaIpcClient(media_sock);
    g_maint = new Maintenance(g_store, g_media);

    // sync configStates into node at startup
    json_object* cs = g_store->all_states();
    json_object* patch = json_object_new_object();
    json_object_object_add(patch, "configStates", cs);
    json_object* cr = g_store->config_revision();
    json_object_object_add(patch, "configRevision", cr);
    g_store->update_node(patch);
    json_object_put(patch);

    HttpServer::Handler handler = [&](const HttpRequest& req, HttpResponse& res) {
        // login
        if (req.method == "POST" && req.path == "/api/v1/node/login") {
            json_object* body = json_tokener_parse(req.body.c_str());
            std::string u = get_str(body, "username");
            std::string p = get_str(body, "password");
            if (body) json_object_put(body);
            if (g_store->verify_password(u, p)) {
                json_object* d = json_object_new_object();
                json_object_object_add(d, "session", json_object_new_string("sess-1"));
                json_object_object_add(d, "expiresIn", json_object_new_int(3600));
                res.headers["Set-Cookie"] = "session=sess-1; Path=/; HttpOnly";
                send_json(res, envelope_ok(d, "登录成功"));
            } else {
                send_json(res, envelope_fail("UNAUTHORIZED", 401, "账号或密码错误"));
            }
            return;
        }

        // auth check (cookie)
        bool authed = (req.headers.count("Cookie") && req.headers.at("Cookie").find("session=sess-1") != std::string::npos);
        // allow media/config reads without auth? keep simple: require auth for /api except login
        if (req.path.rfind("/api/", 0) == 0 && req.path != "/api/v1/node/login" && !authed) {
            // For prototype, allow anonymous to ease frontend; comment out strictness:
            // send_json(res, envelope_fail("UNAUTHORIZED", 401, "未登录"));
            // return;
        }

        if (req.method == "GET" && req.path == "/api/v1/node/info") {
            send_json(res, envelope_ok(json_object_get(g_store->node()), ""));
            return;
        }
        if (req.method == "GET" && req.path == "/api/v1/node/role") {
            json_object* n = g_store->node();
            json_object* d = json_object_new_object();
            json_object* role = nullptr; json_object_object_get_ex(n, "role", &role);
            json_object* pr = nullptr; json_object_object_get_ex(n, "pendingRole", &pr);
            json_object_object_add(d, "role", json_object_get(role));
            json_object_object_add(d, "pendingRole", json_object_get(pr));
            json_object* cr = nullptr; json_object_object_get_ex(n, "configRevision", &cr);
            json_object* rrev = nullptr;
            if (cr) json_object_object_get_ex(cr, "role", &rrev);
            int rev = 1;
            if (rrev) { json_object* dd=nullptr; if (json_object_object_get_ex(rrev,"desired",&dd)) rev=json_object_get_int(dd); }
            json_object_object_add(d, "revision", json_object_new_int(rev));
            send_json(res, envelope_ok(d, ""));
            return;
        }

        // config scope
        {
            // regex manual
            std::string prefix = "/api/v1/node/config/";
            if (req.path.rfind(prefix, 0) == 0) {
                std::string rest = req.path.substr(prefix.size());
                std::string scope = rest;
                std::string action;
                size_t slash = rest.find('/');
                if (slash != std::string::npos) { scope = rest.substr(0, slash); action = rest.substr(slash+1); }
                // validate scope
                bool valid = false;
                for (int i = 0; i < 6; i++) if (scope == NodeStore::SCOPES[i]) valid = true;
                if (!valid) { send_json(res, envelope_fail("INVALID_ARGUMENT", 400, "不支持的配置范围")); return; }
                std::string node_id = req.query.count("nodeId") ? req.query.at("nodeId") : get_str(g_store->node(), "id");
                if (req.method == "GET" && action.empty()) {
                    json_object* cfg = g_store->get_config(scope, node_id);
                    send_json(res, envelope_ok(cfg, ""));
                    return;
                }
                if (req.method == "POST" && action.empty()) {
                    json_object* body = json_tokener_parse(req.body.c_str());
                    json_object* dv = nullptr;
                    if (body) {
                        if (!json_object_object_get_ex(body, "desiredValue", &dv)) json_object_object_get_ex(body, "value", &dv);
                    }
                    json_object* val = dv ? json_object_get(dv) : json_object_new_object();
                    const char* opid = nullptr;
                    std::string opstr;
                    if (body) {
                        json_object* o = nullptr;
                        if (json_object_object_get_ex(body, "operationId", &o) && o) { opstr = json_object_get_string(o); opid = opstr.c_str(); }
                    }
                    g_store->set_desired(scope, val, opid);
                    // update node configStates/revision
                    json_object* patch = json_object_new_object();
                    json_object_object_add(patch, "configStates", g_store->all_states());
                    json_object_object_add(patch, "configRevision", g_store->config_revision());
                    if (scope == "role") {
                        json_object* v = val;
                        json_object* r = nullptr;
                        if (v && json_object_object_get_ex(v, "role", &r)) {
                            json_object* pr = json_object_new_string(json_object_get_string(r));
                            json_object_object_add(patch, "pendingRole", pr);
                        }
                    }
                    g_store->update_node(patch);
                    json_object_put(patch);
                    json_object* cfg = g_store->get_config(scope, node_id);
                    if (body) json_object_put(body);
                    send_json(res, envelope_ok(cfg, "配置已提交"));
                    return;
                }
                if (req.method == "POST" && action == "retry") {
                    g_store->retry(scope);
                    json_object* patch = json_object_new_object();
                    json_object_object_add(patch, "configStates", g_store->all_states());
                    g_store->update_node(patch);
                    json_object_put(patch);
                    json_object* cfg = g_store->get_config(scope, node_id);
                    send_json(res, envelope_ok(cfg, "配置已重新提交，等待应用"));
                    return;
                }
                send_json(res, envelope_fail("NOT_FOUND", 404, "未定义的节点配置操作"));
                return;
            }
        }

        if (req.method == "GET" && req.path == "/api/v1/nodes") {
            json_object* arr = json_object_new_array();
            json_object_array_add(arr, json_object_get(g_store->node()));
            json_object* d = json_object_new_object();
            json_object_object_add(d, "items", arr);
            json_object_object_add(d, "page", json_object_new_int(1));
            json_object_object_add(d, "pageSize", json_object_new_int(1));
            json_object_object_add(d, "total", json_object_new_int(1));
            send_json(res, envelope_ok(d, ""));
            return;
        }

        if (req.method == "POST" && req.path == "/api/v1/nodes/heartbeat") {
            json_object* body = json_tokener_parse(req.body.c_str());
            json_object* patch = nullptr;
            if (body) {
                if (!json_object_object_get_ex(body, "patch", &patch)) patch = body;
            }
            // update lastHeartbeat
            json_object* n = g_store->node();
            json_object* hp = json_object_new_object();
            json_object_object_add(hp, "lastHeartbeat", json_object_new_string("now"));
            if (patch) {
                json_object* m = nullptr;
                if (json_object_object_get_ex(patch, "metrics", &m)) json_object_object_add(hp, "metrics", json_object_get(m));
            }
            g_store->update_node(hp);
            json_object_put(hp);
            if (body) json_object_put(body);
            send_json(res, envelope_ok(json_object_get(g_store->node()), "心跳已接收"));
            return;
        }

        if (req.method == "POST" && req.path == "/api/v1/nodes/upgrade") {
            json_object* body = json_tokener_parse(req.body.c_str());
            json_object* ids = nullptr;
            if (body) json_object_object_get_ex(body, "targetNodeIds", &ids);
            if (!ids && body) json_object_object_get_ex(body, "nodeIds", &ids);
            json_object* d = json_object_new_object();
            json_object* arr = json_object_new_array();
            if (ids) {
                // assume array
                int len = json_object_array_length(ids);
                for (int i = 0; i < len; i++) json_object_array_add(arr, json_object_get(json_object_array_get_idx(ids, i)));
            }
            json_object_object_add(d, "nodeIds", arr);
            json_object_object_add(d, "status", json_object_new_string("applied"));
            if (body) json_object_put(body);
            send_json(res, envelope_ok(d, ""));
            return;
        }

        // node by id routes
        if (req.path.rfind("/api/v1/nodes/", 0) == 0) {
            std::string rest = req.path.substr(strlen("/api/v1/nodes/"));
            std::string id = rest;
            std::string action;
            size_t slash = rest.find('/');
            if (slash != std::string::npos) { id = rest.substr(0, slash); action = rest.substr(slash+1); }
            if (req.method == "GET" && action.empty()) {
                send_json(res, envelope_ok(json_object_get(g_store->node()), ""));
                return;
            }
            if (req.method == "PUT" && action.empty()) {
                json_object* body = json_tokener_parse(req.body.c_str());
                std::string name = get_str(body, "name");
                if (body) json_object_put(body);
                json_object* patch = json_object_new_object();
                json_object_object_add(patch, "name", json_object_new_string(name.c_str()));
                g_store->update_node(patch);
                json_object_put(patch);
                send_json(res, envelope_ok(json_object_get(g_store->node()), ""));
                return;
            }
            if (req.method == "POST" && action == "retire") {
                json_object* patch = json_object_new_object();
                json_object_object_add(patch, "lifecycle", json_object_new_string("retired"));
                json_object_object_add(patch, "online", json_object_new_boolean(0));
                json_object_object_add(patch, "bind", json_object_new_string("—"));
                g_store->update_node(patch);
                json_object_put(patch);
                send_json(res, envelope_ok(json_object_get(g_store->node()), ""));
                return;
            }
            if (req.method == "POST" && action == "heartbeat") {
                json_object* body = json_tokener_parse(req.body.c_str());
                json_object* patch = nullptr;
                if (body) {
                    if (!json_object_object_get_ex(body, "patch", &patch)) patch = body;
                }
                json_object* hp = json_object_new_object();
                json_object_object_add(hp, "lastHeartbeat", json_object_new_string("now"));
                if (patch) {
                    json_object* m = nullptr;
                    if (json_object_object_get_ex(patch, "metrics", &m)) json_object_object_add(hp, "metrics", json_object_get(m));
                }
                g_store->update_node(hp);
                json_object_put(hp);
                if (body) json_object_put(body);
                send_json(res, envelope_ok(json_object_get(g_store->node()), "心跳已接收"));
                return;
            }
        }

        // media endpoints
        if (req.method == "GET" && req.path == "/api/v1/node/media/status") {
            std::string r = g_media->query_state();
            json_object* o = r.empty() ? json_object_new_object() : json_tokener_parse(r.c_str());
            if (!o) o = json_object_new_object();
            send_json(res, envelope_ok(o, ""));
            return;
        }
        if (req.method == "GET" && req.path == "/api/v1/node/media/sources") {
            send_json(res, envelope_ok(json_object_new_object(), ""));
            return;
        }
        if (req.method == "GET" && req.path == "/api/v1/node/media/logs") {
            json_object* d = json_object_new_object();
            json_object_object_add(d, "items", json_object_new_array());
            send_json(res, envelope_ok(d, ""));
            return;
        }
        if (req.method == "GET" && req.path == "/api/v1/node/capability") {
            json_object* n = g_store->node();
            json_object* cap = nullptr;
            json_object_object_get_ex(n, "capabilities", &cap);
            send_json(res, envelope_ok(cap ? json_object_get(cap) : json_object_new_object(), ""));
            return;
        }

        // maintenance
        if (req.method == "POST" && req.path == "/api/v1/node/reboot") {
            send_json(res, envelope_ok(g_maint->reboot(), ""));
            return;
        }
        if (req.method == "POST" && req.path == "/api/v1/node/factory-reset") {
            send_json(res, envelope_ok(g_maint->factory_reset(), ""));
            return;
        }
        if (req.method == "POST" && req.path == "/api/v1/node/upgrade") {
            json_object* body = json_tokener_parse(req.body.c_str());
            std::string fn = body ? get_str(body, "filename") : std::string("fw.bin");
            if (fn.empty()) fn = "fw.bin";
            std::string sha = body ? get_str(body, "sha256") : std::string();
            if (body) json_object_put(body);
            send_json(res, envelope_ok(g_maint->upgrade(fn, sha), ""));
            return;
        }

        if (req.method == "GET" && req.path == "/api/v1/audit-events") {
            json_object* d = json_object_new_object();
            json_object_object_add(d, "items", json_object_new_array());
            json_object_object_add(d, "page", json_object_new_int(1));
            json_object_object_add(d, "pageSize", json_object_new_int(100));
            json_object_object_add(d, "total", json_object_new_int(0));
            send_json(res, envelope_ok(d, ""));
            return;
        }

        // static files
        if (req.method == "GET") {
            std::string path = req.path;
            if (path == "/" || path.empty()) path = "/index.html";
            std::string full = g_web_root + path;
            struct stat st;
            if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                std::string content = read_file(full);
                res.status = 200;
                res.body = content;
                res.content_type = guess_ct(full);
                return;
            }
        }

        send_json(res, envelope_fail("NOT_FOUND", 404, ("未定义的 API：" + req.method + " " + req.path).c_str()));
    };

    HttpServer server(port, handler);
    server.start();
    printf("dvs-agent listening on 0.0.0.0:%d\n", port);
    fflush(stdout);

    // heartbeat thread
    std::thread hb([&]() {
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            if (!platform_url.empty()) {
                // minimal: skip actual HTTP client for brevity
            }
        }
    });
    hb.detach();

    while (true) std::this_thread::sleep_for(std::chrono::seconds(3600));
    return 0;
}
