#include "local_shim.h"

#include <cstring>
#include <ctime>

#include "dvs_contract.h"
#include "dvs_http_json.h"

namespace {

std::string shim_now() {
    char buf[64];
    time_t t = time(nullptr);
    struct tm* tm = gmtime(&t);
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", tm);
    return std::string(buf);
}

std::string shim_str(json_object* o, const char* key) {
    json_object* v = nullptr;
    if (o && json_object_object_get_ex(o, key, &v) && v)
        return json_object_get_string(v);
    return "";
}

void shim_send(HttpResponse& res, json_object* o) {
    const char* s = json_object_to_json_string(o);
    res.body = s ? s : "{}";
    res.content_type = "application/json; charset=utf-8";
    json_object_put(o);
}

json_object* shim_ok(json_object* data) {
    return dvs::contract::make_ok(data, "", "a");
}

void shim_heartbeat(NodeStore* store, json_object* body) {
    json_object* patch = nullptr;
    if (body) {
        if (!json_object_object_get_ex(body, "patch", &patch)) patch = body;
    }
    json_object* hp = json_object_new_object();
    json_object_object_add(hp, "lastHeartbeat",
                           json_object_new_string(shim_now().c_str()));
    if (patch) {
        json_object* m = nullptr;
        if (json_object_object_get_ex(patch, "metrics", &m) && m)
            json_object_object_add(hp, "metrics", json_object_get(m));
    }
    store->update_node(hp);
    json_object_put(hp);
}

}  // namespace

bool route_local_nodes(NodeStore* store, const HttpRequest& req,
                       HttpResponse& res) {
    if (req.method == "GET" && req.path == "/api/v1/nodes") {
        json_object* arr = json_object_new_array();
        json_object_array_add(arr, json_object_get(store->node()));
        json_object* d = json_object_new_object();
        json_object_object_add(d, "items", arr);
        json_object_object_add(d, "page", json_object_new_int(1));
        json_object_object_add(d, "pageSize", json_object_new_int(1));
        json_object_object_add(d, "total", json_object_new_int(1));
        shim_send(res, shim_ok(d));
        return true;
    }
    if (req.method == "POST" && req.path == "/api/v1/nodes/heartbeat") {
        json_object* body = json_tokener_parse(req.body.c_str());
        shim_heartbeat(store, body);
        if (body) json_object_put(body);
        shim_send(res, dvs::contract::make_ok(json_object_get(store->node()),
                                              "心跳已接收", "a"));
        return true;
    }
    if (req.method == "POST" && req.path == "/api/v1/nodes/upgrade") {
        json_object* body = json_tokener_parse(req.body.c_str());
        json_object* ids = nullptr;
        if (body) json_object_object_get_ex(body, "targetNodeIds", &ids);
        if (!ids && body) json_object_object_get_ex(body, "nodeIds", &ids);
        json_object* d = json_object_new_object();
        json_object* arr = json_object_new_array();
        if (ids) {
            int len = json_object_array_length(ids);
            for (int i = 0; i < len; i++)
                json_object_array_add(arr, json_object_get(
                    json_object_array_get_idx(ids, i)));
        }
        json_object_object_add(d, "nodeIds", arr);
        json_object_object_add(d, "status", json_object_new_string("applied"));
        if (body) json_object_put(body);
        shim_send(res, shim_ok(d));
        return true;
    }
    if (req.path.rfind("/api/v1/nodes/", 0) == 0) {
        std::string rest = req.path.substr(strlen("/api/v1/nodes/"));
        std::string id = rest;
        std::string action;
        size_t slash = rest.find('/');
        if (slash != std::string::npos) {
            id = rest.substr(0, slash);
            action = rest.substr(slash + 1);
        }
        std::string local_id = shim_str(store->node(), "id");
        if (!local_id.empty() && id != local_id) {
            dvs::send_json(res, dvs::envelope_fail("NOT_FOUND", 404,
                                                 "node unknown", "a"));
            return true;
        }
        if (req.method == "GET" && action.empty()) {
            shim_send(res, shim_ok(json_object_get(store->node())));
            return true;
        }
        if (req.method == "PUT" && action.empty()) {
            json_object* body = json_tokener_parse(req.body.c_str());
            std::string name = shim_str(body, "name");
            if (body) json_object_put(body);
            size_t begin = 0;
            while (begin < name.size() && (name[begin] == ' ' ||
                   name[begin] == '\t' || name[begin] == '\n' ||
                   name[begin] == '\r'))
                begin++;
            size_t end = name.size();
            while (end > begin && (name[end - 1] == ' ' ||
                   name[end - 1] == '\t' || name[end - 1] == '\n' ||
                   name[end - 1] == '\r'))
                end--;
            if (end <= begin) {
                dvs::send_json(res, dvs::envelope_fail("INVALID_ARGUMENT",
                                                     400, "missing name",
                                                     "a"));
                return true;
            }
            name = name.substr(begin, end - begin);
            json_object* patch = json_object_new_object();
            json_object_object_add(patch, "name",
                                   json_object_new_string(name.c_str()));
            store->update_node(patch);
            json_object_put(patch);
            shim_send(res, shim_ok(json_object_get(store->node())));
            return true;
        }
        if (req.method == "POST" && action == "retire") {
            json_object* patch = json_object_new_object();
            json_object_object_add(patch, "lifecycle",
                                   json_object_new_string("retired"));
            json_object_object_add(patch, "online", json_object_new_boolean(0));
            json_object_object_add(patch, "bind",
                                   json_object_new_string("—"));
            store->update_node(patch);
            json_object_put(patch);
            shim_send(res, shim_ok(json_object_get(store->node())));
            return true;
        }
        if (req.method == "POST" && action == "heartbeat") {
            json_object* body = json_tokener_parse(req.body.c_str());
            shim_heartbeat(store, body);
            if (body) json_object_put(body);
            shim_send(res, dvs::contract::make_ok(
                json_object_get(store->node()), "心跳已接收", "a"));
            return true;
        }
    }
    return false;
}
