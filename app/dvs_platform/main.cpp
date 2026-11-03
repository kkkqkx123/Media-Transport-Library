// dvs_platform main: node gateway + docs CRUD + op barrier + serial upgrade.
#include <json-c/json.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <unistd.h>

#include "http_client.h"
#include "http_server.h"
#include "layout.h"
#include "ops.h"
#include "platform_store.h"

static PlatformStore* g_store = nullptr;
static OpCoordinator g_ops;

namespace {
std::string now_iso() {
    char b[32];
    time_t t = time(nullptr);
    strftime(b, sizeof(b), "%FT%TZ", gmtime(&t));
    return b;
}
int64_t now_sec() { return (int64_t)time(nullptr); }
uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
json_object* ok_envelope(json_object* data, const std::string& msg) {
    json_object* o = json_object_new_object();
    json_object_object_add(o, "ok", json_object_new_boolean(1));
    json_object_object_add(o, "status", json_object_new_int(200));
    json_object_object_add(o, "code", json_object_new_string("OK"));
    json_object_object_add(o, "msg", json_object_new_string(msg.c_str()));
    json_object_object_add(o, "data", data ? data : json_object_new_object());
    json_object_object_add(o, "requestId", json_object_new_string("p-1"));
    return o;
}
json_object* fail_envelope(const char* code, int status, const std::string& msg) {
    json_object* o = json_object_new_object();
    json_object_object_add(o, "ok", json_object_new_boolean(0));
    json_object_object_add(o, "status", json_object_new_int(status));
    json_object_object_add(o, "code", json_object_new_string(code));
    json_object_object_add(o, "msg", json_object_new_string(msg.c_str()));
    json_object_object_add(o, "data", json_object_new_object());
    json_object_object_add(o, "requestId", json_object_new_string("p-1"));
    return o;
}
void send_json(HttpResponse& res, json_object* o) {
    const char* s = json_object_to_json_string(o);
    res.body = s ? s : "{}";
    res.content_type = "application/json; charset=utf-8";
    json_object_put(o);
}
std::string body_str(json_object* o, const char* key) {
    json_object* v = nullptr;
    if (o && json_object_object_get_ex(o, key, &v) && v)
        return json_object_get_string(v);
    return "";
}
// Generic docs CRUD mounted at /api/v1/<kind>[/<id>].
bool route_docs(const std::string& kind, const HttpRequest& req, HttpResponse& res) {
    std::string base = "/api/v1/" + kind;
    if (req.path != base && req.path.rfind(base + "/", 0) != 0) return false;
    std::string id = (req.path.size() > base.size()) ? req.path.substr(base.size() + 1) : "";
    if (req.method == "GET" && id.empty()) {
        json_object* arr = json_object_new_array();
        for (auto& i : g_store->list_ids(kind)) {
            json_object* d = g_store->get_doc(kind, i);
            if (d) json_object_array_add(arr, d);
        }
        json_object* d = json_object_new_object();
        json_object_object_add(d, "items", arr);
        send_json(res, ok_envelope(d, ""));
        return true;
    }
    if (req.method == "GET") {
        json_object* d = g_store->get_doc(kind, id);
        if (!d) { send_json(res, fail_envelope("NOT_FOUND", 404, "not found")); return true; }
        send_json(res, ok_envelope(d, ""));
        return true;
    }
    if (req.method == "POST" || req.method == "PUT") {
        json_object* b = json_tokener_parse(req.body.c_str());
        if (!b) b = json_object_new_object();
        if (id.empty()) {
            id = body_str(b, "id");
            if (id.empty()) {
                char nb[32];
                snprintf(nb, sizeof(nb), "%s-%ld", kind.c_str(), (long)now_sec());
                id = nb;
                json_object_object_add(b, "id", json_object_new_string(id.c_str()));
            }
        }
        g_store->put_doc(kind, id, b);
        g_store->audit("{\"kind\":\"" + kind + "\",\"id\":\"" + id + "\"}");
        json_object* d = g_store->get_doc(kind, id);
        json_object_put(b);
        send_json(res, ok_envelope(d ? d : json_object_new_object(), ""));
        return true;
    }
    if (req.method == "DELETE") {
        g_store->del_doc(kind, id);
        send_json(res, ok_envelope(json_object_new_object(), ""));
        return true;
    }
    return false;
}
} // namespace

int main(int argc, char** argv) {
    std::string db = "/tmp/dvs_platform.db";
    int port = 8081;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--db" && i + 1 < argc) db = argv[++i];
        else if (a == "--port" && i + 1 < argc) port = atoi(argv[++i]);
    }
    g_store = new PlatformStore(db);

    HttpServer::Handler h = [](const HttpRequest& req, HttpResponse& res) {
        if (req.method == "POST" && req.path == "/api/v1/nodes/register") {
            json_object* b = json_tokener_parse(req.body.c_str());
            std::string id = body_str(b, "id");
            if (id.empty()) id = body_str(b, "sn");
            if (id.empty()) {
                if (b) json_object_put(b);
                send_json(res, fail_envelope("INVALID_ARGUMENT", 400, "missing id"));
                return;
            }
            g_store->tombstone_gc(now_sec());
            if (g_store->tombstone_check(id, now_sec())) {
                if (b) json_object_put(b);
                send_json(res, fail_envelope("RETIRED", 410, "node retired"));
                return;
            }
            if (!b) b = json_object_new_object();
            json_object_object_add(b, "online", json_object_new_boolean(1));
            json_object_object_add(b, "lastHeartbeat",
                                   json_object_new_string(now_iso().c_str()));
            g_store->put_doc("node", id, b);
            g_store->audit("{\"register\":\"" + id + "\"}");
            json_object* d = g_store->get_doc("node", id);
            json_object_put(b);
            send_json(res, ok_envelope(d ? d : json_object_new_object(), ""));
            return;
        }
        if (req.method == "POST" && req.path == "/api/v1/nodes/heartbeat") {
            json_object* b = json_tokener_parse(req.body.c_str());
            std::string id = body_str(b, "id");
            json_object* cur = id.empty() ? nullptr : g_store->get_doc("node", id);
            if (!cur) {
                if (b) json_object_put(b);
                send_json(res, fail_envelope("NOT_FOUND", 404, "node unknown"));
                return;
            }
            json_object_object_add(cur, "online", json_object_new_boolean(1));
            json_object_object_add(cur, "lastHeartbeat",
                                   json_object_new_string(now_iso().c_str()));
            json_object* m = nullptr;
            if (b && json_object_object_get_ex(b, "metrics", &m) && m)
                json_object_object_add(cur, "metrics", json_object_get(m));
            g_store->put_doc("node", id, cur);
            json_object_put(b);
            json_object* d = json_object_new_object();
            json_object_object_add(d, "revision", json_object_new_int(1));
            send_json(res, ok_envelope(d, ""));
            json_object_put(cur);
            return;
        }
        if (req.path.rfind("/api/v1/nodes/", 0) == 0) {
            std::string rest = req.path.substr(strlen("/api/v1/nodes/"));
            size_t sl = rest.find('/');
            std::string id = (sl == std::string::npos) ? rest : rest.substr(0, sl);
            std::string act = (sl == std::string::npos) ? "" : rest.substr(sl + 1);
            if (req.method == "GET" && act.empty()) {
                json_object* d = g_store->get_doc("node", id);
                if (!d) { send_json(res, fail_envelope("NOT_FOUND", 404, "not found")); return; }
                send_json(res, ok_envelope(d, ""));
                return;
            }
            if (req.method == "POST" && act == "retire") {
                g_store->tombstone_add(id, now_sec());
                g_store->del_doc("node", id);
                g_store->audit("{\"retire\":\"" + id + "\"}");
                send_json(res, ok_envelope(json_object_new_object(), "retired"));
                return;
            }
        }
        if (req.method == "GET" && req.path == "/api/v1/nodes") {
            json_object* arr = json_object_new_array();
            for (auto& i : g_store->list_ids("node")) {
                json_object* d = g_store->get_doc("node", i);
                if (d) json_object_array_add(arr, d);
            }
            json_object* d = json_object_new_object();
            json_object_object_add(d, "items", arr);
            send_json(res, ok_envelope(d, ""));
            return;
        }
        // Operation barrier (T8): prepare collects members, commit fans out.
        if (req.method == "POST" && req.path == "/api/v1/ops/prepare") {
            json_object* b = json_tokener_parse(req.body.c_str());
            std::string opid = body_str(b, "operationId");
            int rev = 1;
            json_object* v = nullptr;
            if (b && json_object_object_get_ex(b, "revision", &v)) rev = json_object_get_int(v);
            double fps = 0;
            if (b && json_object_object_get_ex(b, "fps", &v)) fps = json_object_get_double(v);
            std::set<std::string> members;
            json_object* arr = nullptr;
            if (b && json_object_object_get_ex(b, "members", &arr) &&
                json_object_is_type(arr, json_type_array)) {
                for (size_t i = 0; i < json_object_array_length(arr); i++)
                    members.insert(json_object_get_string(json_object_array_get_idx(arr, i)));
            }
            if (opid.empty() || members.empty()) {
                if (b) json_object_put(b);
                send_json(res, fail_envelope("INVALID_ARGUMENT", 400, "need operationId+members"));
                return;
            }
            Operation& op = g_ops.begin(opid, rev, members, layout_apply_at_ns(now_ns(), fps));
            if (b) json_object_put(b);
            json_object* d = json_object_new_object();
            json_object_object_add(d, "operationId", json_object_new_string(op.op_id.c_str()));
            json_object_object_add(d, "applyAt",
                                   json_object_new_int64((int64_t)op.apply_at_ns));
            send_json(res, ok_envelope(d, "preparing"));
            return;
        }
        if (req.method == "POST" && req.path == "/api/v1/ops/commit") {
            json_object* b = json_tokener_parse(req.body.c_str());
            std::string opid = body_str(b, "operationId");
            std::string node = body_str(b, "node");
            std::string st = body_str(b, "status");
            if (st.empty()) st = "ready";
            std::string state = g_ops.on_reply(opid, node, st != "blocked", st);
            const Operation* op = g_ops.find(opid);
            if (b) json_object_put(b);
            if (!op) { send_json(res, fail_envelope("NOT_FOUND", 404, "op unknown")); return; }
            if (state == "blocked") {
                int to_rev = 0;
                std::string rb = g_ops.rollback_op(opid, &to_rev);
                json_object* d = json_object_new_object();
                json_object_object_add(d, "rollback", json_object_new_string(rb.c_str()));
                json_object_object_add(d, "toRevision", json_object_new_int(to_rev));
                send_json(res, ok_envelope(d, "rolled back"));
                return;
            }
            json_object* d = json_object_new_object();
            json_object_object_add(d, "state", json_object_new_string(state.c_str()));
            json_object_object_add(d, "applyAt",
                                   json_object_new_int64((int64_t)op->apply_at_ns));
            send_json(res, ok_envelope(d, state));
            return;
        }
        // Serial batch upgrade (T9): one node at a time, failures isolated.
        if (req.method == "POST" && req.path == "/api/v1/nodes/upgrade") {
            json_object* b = json_tokener_parse(req.body.c_str());
            json_object* arr = nullptr;
            if (b) json_object_object_get_ex(b, "targetNodeIds", &arr);
            json_object* results = json_object_new_array();
            HttpClient cli;
            if (arr && json_object_is_type(arr, json_type_array)) {
                for (size_t i = 0; i < json_object_array_length(arr); i++) {
                    std::string nid = json_object_get_string(json_object_array_get_idx(arr, i));
                    json_object* nd = g_store->get_doc("node", nid);
                    std::string base = nd ? body_str(nd, "agentBase") : "";
                    if (nd) json_object_put(nd);
                    json_object* r = json_object_new_object();
                    json_object_object_add(r, "node", json_object_new_string(nid.c_str()));
                    if (base.empty()) {
                        json_object_object_add(r, "status", json_object_new_string("skipped"));
                    } else {
                        std::string resp;
                        int sc = cli.post_json(base + "/api/v1/node/upgrade",
                                               req.body, resp, 10000);
                        json_object_object_add(r, "status", json_object_new_string(
                            sc == 200 ? "applied" : "failed"));
                    }
                    json_object_array_add(results, r);
                }
            }
            if (b) json_object_put(b);
            json_object* d = json_object_new_object();
            json_object_object_add(d, "results", results);
            send_json(res, ok_envelope(d, ""));
            return;
        }
        if (req.method == "GET" && req.path == "/api/v1/audit-events") {
            send_json(res, ok_envelope(g_store->audit_list(100), ""));
            return;
        }
        const char* kinds[] = {"groups", "walls", "scenes", "bindings",
                               "whitelist", "playback-tasks"};
        for (auto k : kinds)
            if (route_docs(k, req, res)) return;
        send_json(res, fail_envelope("NOT_FOUND", 404, "unknown api"));
    };

    HttpServer server(port, h);
    server.start();
    printf("dvs-platform listening on 0.0.0.0:%d\n", port);
    fflush(stdout);
    for (;;) pause();
    return 0;
}
