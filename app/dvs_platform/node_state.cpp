#include "node_state.h"

#include "dvs_http_json.h"
#include "layout.h"
#include "platform_context.h"
#include "platform_store.h"

bool node_doc_ready(json_object* doc) {
    if (!doc || !json_object_is_type(doc, json_type_object)) return false;
    if (dvs::json_get_str(doc, "agentBase").empty()) return false;
    json_object* geo = nullptr;
    if (!json_object_object_get_ex(doc, "geometry", &geo) || !geo)
        return false;
    return dvs::json_rect_int(geo, "w") > 0 &&
           dvs::json_rect_int(geo, "h") > 0;
}

std::set<std::string> derive_wall_members(PlatformContext& ctx,
                                          const std::string& wall_id,
                                          std::set<std::string>* pending_out) {
    std::set<std::string> out;
    for (auto& bid : ctx.store->list_ids("bindings")) {
        json_object* b = ctx.store->get_doc("bindings", bid);
        if (!b) continue;
        if (binding_wall_of(b) != wall_id) {
            json_object_put(b);
            continue;
        }
        std::string node = binding_node_of(b);
        std::string st = dvs::json_get_str(b, "status");
        if (node.empty()) {
            json_object_put(b);
            continue;
        }
        json_object_put(b);
        if (st == "pending_offline" || st == "pending") {
            if (pending_out) pending_out->insert(node);
            continue;
        }
        json_object* nd = ctx.store->get_doc("node", node);
        bool ready = node_doc_ready(nd);
        if (nd) json_object_put(nd);
        if (!ready) {
            if (pending_out) pending_out->insert(node);
            continue;
        }
        out.insert(node);
    }
    return out;
}

json_object* heartbeat_ack_for_node(PlatformContext& ctx,
                                    const std::string& node_id) {
    std::string wall_id;
    for (auto& bid : ctx.store->list_ids("bindings")) {
        json_object* b = ctx.store->get_doc("bindings", bid);
        if (!b) continue;
        bool hit = (binding_node_of(b) == node_id);
        std::string w = hit ? binding_wall_of(b) : "";
        json_object_put(b);
        if (hit && !w.empty()) {
            wall_id = w;
            break;
        }
    }
    int applied = 0;
    std::string scene;
    if (!wall_id.empty()) {
        json_object* w = ctx.store->get_doc("walls", wall_id);
        if (w) {
            json_object* av = nullptr;
            if (json_object_object_get_ex(w, "appliedRevision", &av) && av)
                applied = json_object_get_int(av);
            scene = dvs::json_get_str(w, "currentSceneId");
            json_object_put(w);
        }
        if (applied <= 0) {
            std::lock_guard<std::mutex> lk(ctx.ops_mu);
            applied = ctx.ops.wall_committed_revision(wall_id);
        }
    }
    int pending = 0;
    {
        std::vector<std::string> op_ids = ctx.store->list_ids("op");
        std::lock_guard<std::mutex> lk(ctx.ops_mu);
        for (const auto& id : op_ids) {
            const Operation* op = ctx.ops.find(id);
            if (!op || op->state != "preparing") continue;
            if (op->members.count(node_id) && op->revision > pending)
                pending = op->revision;
        }
    }
    json_object* d = json_object_new_object();
    json_object_object_add(d, "revision", json_object_new_int(applied));
    json_object_object_add(d, "appliedRevision", json_object_new_int(applied));
    json_object_object_add(d, "pendingRevision", json_object_new_int(pending));
    if (!wall_id.empty())
        json_object_object_add(d, "wallId",
                               json_object_new_string(wall_id.c_str()));
    if (!scene.empty())
        json_object_object_add(d, "currentSceneId",
                               json_object_new_string(scene.c_str()));
    return d;
}

void mark_wall_committed(PlatformContext& ctx, const std::string& wall_id,
                         const std::string& scene_id, int revision,
                         const std::string& hash) {
    if (wall_id.empty()) return;
    json_object* w = ctx.store->get_doc("walls", wall_id);
    if (!w) return;
    if (!scene_id.empty()) {
        json_object_object_del(w, "currentSceneId");
        json_object_object_add(w, "currentSceneId",
                               json_object_new_string(scene_id.c_str()));
    }
    json_object_object_del(w, "appliedRevision");
    json_object_object_add(w, "appliedRevision", json_object_new_int(revision));
    if (!hash.empty()) {
        json_object_object_del(w, "layoutHash");
        json_object_object_add(w, "layoutHash",
                               json_object_new_string(hash.c_str()));
    }
    ctx.store->put_doc("walls", wall_id, w);
    json_object_put(w);
}

void store_role_intent(json_object* cur, json_object* data) {
    if (!cur || !data) return;
    json_object* v = nullptr;
    if (json_object_object_get_ex(data, "pendingRole", &v)) {
        json_object_object_del(cur, "pendingRole");
        json_object_object_add(cur, "pendingRole",
                               v ? json_object_get(v) : json_object_new_null());
    }
    if (json_object_object_get_ex(data, "roleCleanup", &v)) {
        json_object_object_del(cur, "roleCleanup");
        json_object_object_add(cur, "roleCleanup",
                               v ? json_object_get(v) : json_object_new_null());
    }
}

void promote_pending_bindings(PlatformContext& ctx,
                              const std::string& node_id) {
    if (node_id.empty() || !ctx.store) return;
    for (auto& bid : ctx.store->list_ids("bindings")) {
        json_object* b = ctx.store->get_doc("bindings", bid);
        if (!b) continue;
        if (binding_node_of(b) != node_id) {
            json_object_put(b);
            continue;
        }
        std::string st = dvs::json_get_str(b, "status");
        if (st != "pending" && st != "pending_offline") {
            json_object_put(b);
            continue;
        }
        std::string w = binding_wall_of(b);
        std::string outp = binding_output_of(b);
        std::string sig = binding_sig_of(b);
        std::string why;
        json_object* wall = ctx.store->get_doc("walls", w);
        if (!wall || wall_is_deleted(wall)) why = "wall unknown or deleted";
        if (wall) json_object_put(wall);
        if (why.empty() && !sig.empty()) {
            bool found = false;
            for (auto& wid : ctx.store->list_ids("whitelist")) {
                json_object* o = ctx.store->get_doc("whitelist", wid);
                if (!o) continue;
                bool hit = (dvs::json_get_str(o, "sig") == sig);
                json_object_put(o);
                if (hit) {
                    found = true;
                    break;
                }
            }
            if (!found) why = "binding signal not in whitelist";
        }
        if (why.empty() && !outp.empty()) {
            for (auto& obid : ctx.store->list_ids("bindings")) {
                if (obid == bid) continue;
                json_object* o = ctx.store->get_doc("bindings", obid);
                if (!o) continue;
                std::string ost = dvs::json_get_str(o, "status");
                bool busy = (binding_node_of(o) == node_id &&
                             binding_output_of(o) == outp &&
                             ost != "pending" && ost != "pending_offline");
                json_object_put(o);
                if (busy) {
                    why = "node output port already bound";
                    break;
                }
            }
        }
        if (why.empty()) {
            json_object_object_del(b, "status");
            json_object_object_add(b, "status",
                                   json_object_new_string("active"));
            ctx.store->put_doc("bindings", bid, b);
            json_object_put(b);
            ctx.store->audit("{\"binding-promoted\":\"" + dvs::json_escape(bid) + "\"}");
        } else {
            json_object_put(b);
            ctx.store->audit("{\"binding-still-pending\":\"" + dvs::json_escape(bid) +
                             "\",\"reason\":\"" + dvs::json_escape(why) + "\"}");
        }
    }
}

void store_ptp_playback(json_object* cur, json_object* data) {
    if (!cur || !data) return;
    json_object* p = nullptr;
    if (json_object_object_get_ex(data, "ptp", &p) && p) {
        json_object_object_del(cur, "ptp");
        json_object_object_add(cur, "ptp", json_object_get(p));
    }
    json_object* pb = nullptr;
    if (json_object_object_get_ex(data, "currentPlayback", &pb) && pb) {
        json_object_object_del(cur, "currentPlayback");
        json_object_object_add(cur, "currentPlayback", json_object_get(pb));
    }
}

void project_hello_fields(json_object* doc) {
    if (!doc || !json_object_is_type(doc, json_type_object)) return;
    std::string id = dvs::json_get_str(doc, "id");
    json_object_object_del(doc, "registerState");
    json_object_object_add(doc, "registerState",
                           json_object_new_string("registered"));
    if (dvs::json_get_str(doc, "name").empty() && !id.empty()) {
        json_object_object_add(doc, "name",
                               json_object_new_string(id.c_str()));
    }
    std::string fw = dvs::json_get_str(doc, "firmware");
    if (!fw.empty()) {
        json_object_object_del(doc, "fw");
        json_object_object_add(doc, "fw",
                               json_object_new_string(fw.c_str()));
    }
    json_object* net = nullptr;
    if (json_object_object_get_ex(doc, "network", &net) && net &&
        json_object_is_type(net, json_type_object)) {
        std::string ip = dvs::json_get_str(net, "ip");
        if (!ip.empty() && dvs::json_get_str(doc, "ip").empty()) {
            json_object_object_add(doc, "ip",
                                   json_object_new_string(ip.c_str()));
        }
    }
}

void project_heartbeat_fields(json_object* doc, json_object* data) {
    if (!doc || !json_object_is_type(doc, json_type_object)) return;
    std::string fw = data ? dvs::json_get_str(data, "firmware") : "";
    if (!fw.empty()) {
        json_object_object_del(doc, "fw");
        json_object_object_add(doc, "fw",
                               json_object_new_string(fw.c_str()));
    }
    json_object* up = nullptr;
    if (data && json_object_object_get_ex(data, "uptimeSec", &up) && up &&
        json_object_is_type(up, json_type_int)) {
        json_object_object_del(doc, "uptime");
        json_object_object_add(doc, "uptime",
                               json_object_new_int(json_object_get_int(up)));
    }
    json_object* res = nullptr;
    if (data && json_object_object_get_ex(data, "resources", &res) && res &&
        json_object_is_type(res, json_type_object)) {
        json_object* met = nullptr;
        if (!json_object_object_get_ex(doc, "metrics", &met) || !met ||
            !json_object_is_type(met, json_type_object)) {
            met = json_object_new_object();
            json_object_object_add(doc, "metrics", met);
        }
        const char* keys[] = {"cpu", "memMb", "tempC"};
        for (auto k : keys) {
            json_object* v = nullptr;
            if (json_object_object_get_ex(res, k, &v) && v) {
                json_object_object_del(met, k);
                json_object_object_add(met, k, json_object_get(v));
            }
        }
    }
}

void apply_hello_doc(PlatformContext& ctx, json_object* data) {
    std::string id = dvs::json_get_str(data, "id");
    if (id.empty()) return;
    json_object* merged = json_object_get(data);
    json_object_object_add(merged, "online", json_object_new_boolean(1));
    json_object_object_add(merged, "lastHeartbeat",
                           json_object_new_string(dvs::iso_now().c_str()));
    json_object_object_add(merged, "transport", json_object_new_string("ws"));
    project_hello_fields(merged);
    ctx.store->put_doc("node", id, merged);
    json_object_put(merged);
    ctx.channels.set_transport(id, "ws");
    promote_pending_bindings(ctx, id);
}

void apply_heartbeat_doc(PlatformContext& ctx, const std::string& id,
                         json_object* data) {
    json_object* cur = ctx.store->get_doc("node", id);
    if (!cur) return;
    json_object_object_add(cur, "online", json_object_new_boolean(1));
    json_object_object_add(cur, "lastHeartbeat",
                           json_object_new_string(dvs::iso_now().c_str()));
    json_object_object_add(cur, "transport", json_object_new_string("ws"));
    json_object* ld = nullptr;
    if (data && json_object_object_get_ex(data, "lanesDigest", &ld) && ld) {
        json_object* curm = nullptr;
        if (!json_object_object_get_ex(cur, "metrics", &curm) || !curm) {
            curm = json_object_new_object();
            json_object_object_add(cur, "metrics", curm);
        }
        json_object_object_del(curm, "lanesDigest");
        json_object_object_add(curm, "lanesDigest", json_object_get(ld));
    }
    json_object* txd = nullptr;
    if (data && json_object_object_get_ex(data, "txDigest", &txd) && txd) {
        json_object* curm = nullptr;
        if (!json_object_object_get_ex(cur, "metrics", &curm) || !curm) {
            curm = json_object_new_object();
            json_object_object_add(cur, "metrics", curm);
        }
        json_object_object_del(curm, "txDigest");
        json_object_object_add(curm, "txDigest", json_object_get(txd));
    }
    json_object* sipd = nullptr;
    if (data && json_object_object_get_ex(data, "sip", &sipd) && sipd) {
        json_object_object_del(cur, "sipDigest");
        json_object_object_add(cur, "sipDigest", json_object_get(sipd));
    }
    store_ptp_playback(cur, data);
    store_role_intent(cur, data);
    project_heartbeat_fields(cur, data);
    ctx.store->put_doc("node", id, cur);
    json_object_put(cur);
    ctx.channels.set_transport(id, "ws");
    promote_pending_bindings(ctx, id);
}
