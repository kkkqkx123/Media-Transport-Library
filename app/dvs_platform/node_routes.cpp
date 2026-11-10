#include "node_routes.h"

#include <cstring>
#include <vector>

#include "channels.h"
#include "dvs_contract.h"
#include "http_client.h"
#include "dvs_http_json.h"
#include "layout.h"
#include "node_state.h"
#include "platform_context.h"
#include "platform_store.h"

namespace ct = dvs::contract;

bool route_nodes(PlatformContext& ctx, const HttpRequest& req,
                 HttpResponse& res) {
    if (req.method == "POST" && req.path == "/api/v1/nodes/register") {
        if (!dvs::require_bearer(req, res, ctx.api_token)) return true;
        json_object* b = json_tokener_parse(req.body.c_str());
        std::string verr =
            b ? ct::validate_hello(b) : "INVALID_ARGUMENT:empty body";
        if (!verr.empty()) {
            if (b) json_object_put(b);
            dvs::send_json(res, dvs::envelope_fail("INVALID_ARGUMENT", 400, verr.c_str()));
            return true;
        }
        std::string id = dvs::json_get_str(b, "id");
        if (id.empty()) {
            if (b) json_object_put(b);
            dvs::send_json(res, dvs::envelope_fail("INVALID_ARGUMENT", 400, "missing id"));
            return true;
        }
        ctx.store->tombstone_gc(dvs::wall_sec_now());
        if (ctx.store->tombstone_check(id, dvs::wall_sec_now())) {
            if (b) json_object_put(b);
            dvs::send_json(res, dvs::envelope_fail("RETIRED", 410, "node retired"));
            return true;
        }
        if (!b) b = json_object_new_object();
        bool has_base = !dvs::json_get_str(b, "agentBase").empty();
        json_object* geo0 = nullptr;
        bool has_geo =
            b && json_object_object_get_ex(b, "geometry", &geo0) && geo0;
        json_object_object_add(b, "online", json_object_new_boolean(1));
        json_object_object_add(b, "lastHeartbeat",
                               json_object_new_string(dvs::iso_now().c_str()));
        json_object_object_add(b, "transport",
                               json_object_new_string("short"));
        ctx.store->put_doc("node", id, b);
        ctx.channels.set_transport(id, "short");
        ctx.store->audit("{\"register\":\"" + dvs::json_escape(id) + "\"}");
        promote_pending_bindings(ctx, id);
        if (!has_base || !has_geo)
            ctx.store->audit("{\"incomplete-register\":\"" + dvs::json_escape(id) +
                             "\"}");
        json_object* d = ctx.store->get_doc("node", id);
        json_object_put(b);
        dvs::send_json(res, dvs::envelope_ok(d ? d : json_object_new_object(), ""));
        return true;
    }
    if (req.method == "POST" && req.path == "/api/v1/nodes/heartbeat") {
        if (!dvs::require_bearer(req, res, ctx.api_token)) return true;
        json_object* b = json_tokener_parse(req.body.c_str());
        std::string verr =
            b ? ct::validate_heartbeat(b) : "INVALID_ARGUMENT:empty body";
        if (!verr.empty()) {
            if (b) json_object_put(b);
            dvs::send_json(res, dvs::envelope_fail("INVALID_ARGUMENT", 400, verr.c_str()));
            return true;
        }
        std::string id = dvs::json_get_str(b, "id");
        json_object* cur =
            id.empty() ? nullptr : ctx.store->get_doc("node", id);
        if (!cur) {
            if (b) json_object_put(b);
            dvs::send_json(res, dvs::envelope_fail("NOT_FOUND", 404, "node unknown"));
            return true;
        }
        json_object_object_add(cur, "online", json_object_new_boolean(1));
        json_object_object_add(cur, "lastHeartbeat",
                               json_object_new_string(dvs::iso_now().c_str()));
        json_object* m = nullptr;
        if (b && json_object_object_get_ex(b, "metrics", &m) && m)
            json_object_object_add(cur, "metrics", json_object_get(m));
        json_object* ld = nullptr;
        if (b && json_object_object_get_ex(b, "lanesDigest", &ld) && ld) {
            json_object* curm = nullptr;
            if (!json_object_object_get_ex(cur, "metrics", &curm) || !curm) {
                curm = json_object_new_object();
                json_object_object_add(cur, "metrics", curm);
            }
            json_object_object_del(curm, "lanesDigest");
            json_object_object_add(curm, "lanesDigest", json_object_get(ld));
        }
        json_object* txd = nullptr;
        if (b && json_object_object_get_ex(b, "txDigest", &txd) && txd) {
            json_object* curm = nullptr;
            if (!json_object_object_get_ex(cur, "metrics", &curm) || !curm) {
                curm = json_object_new_object();
                json_object_object_add(cur, "metrics", curm);
            }
            json_object_object_del(curm, "txDigest");
            json_object_object_add(curm, "txDigest", json_object_get(txd));
        }
        json_object* sipd = nullptr;
        if (b && json_object_object_get_ex(b, "sip", &sipd) && sipd) {
            json_object_object_del(cur, "sipDigest");
            json_object_object_add(cur, "sipDigest", json_object_get(sipd));
        }
        store_ptp_playback(cur, b);
        store_role_intent(cur, b);
        json_object_object_add(cur, "transport",
                               json_object_new_string("short"));
        ctx.store->put_doc("node", id, cur);
        ctx.channels.set_transport(id, "short");
        json_object_put(b);
        promote_pending_bindings(ctx, id);
        json_object* d = json_object_new_object();
        json_object_object_add(d, "revision", json_object_new_int(1));
        dvs::send_json(res, dvs::envelope_ok(d, ""));
        json_object_put(cur);
        return true;
    }
    if (req.path.rfind("/api/v1/nodes/", 0) == 0) {
        // Keep the upgrade-task query out of the per-node branch: it is
        // owned by the upgrade module and shares the same prefix.
        if (req.method == "GET" &&
            req.path.rfind("/api/v1/nodes/upgrade/", 0) == 0)
            return false;
        std::string rest = req.path.substr(strlen("/api/v1/nodes/"));
        size_t sl = rest.find('/');
        std::string id =
            (sl == std::string::npos) ? rest : rest.substr(0, sl);
        std::string act =
            (sl == std::string::npos) ? "" : rest.substr(sl + 1);
        if (id.empty() || id.find('/') != std::string::npos) return false;
        if (act.find('/') != std::string::npos) return false;
        if (req.method == "GET" && act.empty()) {
            json_object* d = ctx.store->get_doc("node", id);
            if (!d) {
                dvs::send_json(res, dvs::envelope_fail("NOT_FOUND", 404, "not found"));
                return true;
            }
            dvs::send_json(res, dvs::envelope_ok(d, ""));
            return true;
        }
        if (req.method == "PUT" && act.empty()) {
            json_object* cur = ctx.store->get_doc("node", id);
            if (!cur) {
                dvs::send_json(res, dvs::envelope_fail("NOT_FOUND", 404, "not found"));
                return true;
            }
            json_object* b = json_tokener_parse(req.body.c_str());
            std::string nm = dvs::json_get_str(b, "name");
            if (b) json_object_put(b);
            if (nm.empty()) {
                dvs::send_json(res, dvs::envelope_fail("INVALID_ARGUMENT", 400,
                                             "missing name"));
                json_object_put(cur);
                return true;
            }
            json_object_object_del(cur, "name");
            json_object_object_add(cur, "name",
                                   json_object_new_string(nm.c_str()));
            ctx.store->put_doc("node", id, cur);
            json_object_put(cur);
            ctx.store->audit("{\"rename\":\"" + dvs::json_escape(id) + "\"}");
            json_object* d = ctx.store->get_doc("node", id);
            dvs::send_json(res, dvs::envelope_ok(d ? d : json_object_new_object(), ""));
            return true;
        }
        if (req.method == "POST" && act == "heartbeat") {
            json_object* cur = ctx.store->get_doc("node", id);
            if (!cur) {
                dvs::send_json(res,
                          dvs::envelope_fail("NOT_FOUND", 404, "node unknown"));
                return true;
            }
            json_object* body2 = json_tokener_parse(req.body.c_str());
            json_object_object_del(cur, "online");
            json_object_object_add(cur, "online", json_object_new_boolean(1));
            json_object_object_del(cur, "lastHeartbeat");
            json_object_object_add(cur, "lastHeartbeat",
                                   json_object_new_string(dvs::iso_now().c_str()));
            json_object* m = nullptr;
            if (body2 && json_object_object_get_ex(body2, "metrics", &m) && m)
                json_object_object_add(cur, "metrics", json_object_get(m));
            json_object* ld = nullptr;
            if (body2 && json_object_object_get_ex(body2, "lanesDigest", &ld) &&
                ld) {
                json_object* curm = nullptr;
                if (!json_object_object_get_ex(cur, "metrics", &curm) || !curm) {
                    curm = json_object_new_object();
                    json_object_object_add(cur, "metrics", curm);
                }
                json_object_object_del(curm, "lanesDigest");
                json_object_object_add(curm, "lanesDigest",
                                       json_object_get(ld));
            }
            json_object* txd = nullptr;
            if (body2 && json_object_object_get_ex(body2, "txDigest", &txd) &&
                txd) {
                json_object* curm = nullptr;
                if (!json_object_object_get_ex(cur, "metrics", &curm) || !curm) {
                    curm = json_object_new_object();
                    json_object_object_add(cur, "metrics", curm);
                }
                json_object_object_del(curm, "txDigest");
                json_object_object_add(curm, "txDigest", json_object_get(txd));
            }
            json_object* sipd = nullptr;
            if (body2 && json_object_object_get_ex(body2, "sip", &sipd) &&
                sipd) {
                json_object_object_del(cur, "sipDigest");
                json_object_object_add(cur, "sipDigest",
                                       json_object_get(sipd));
            }
            store_ptp_playback(cur, body2);
            store_role_intent(cur, body2);
            ctx.store->put_doc("node", id, cur);
            if (body2) json_object_put(body2);
            json_object_put(cur);
            promote_pending_bindings(ctx, id);
            json_object* d = ctx.store->get_doc("node", id);
            dvs::send_json(res, dvs::envelope_ok(d ? d : json_object_new_object(),
                                       "heartbeat received"));
            return true;
        }
        if (req.method == "GET" && act == "current-task") {
            json_object* proj = nullptr;
            for (auto& tid : ctx.store->list_ids("playback-tasks")) {
                json_object* t = ctx.store->get_doc("playback-tasks", tid);
                if (!t) continue;
                std::string tn = dvs::json_get_str(t, "targetNodeId");
                bool active = (tn == id &&
                               playback_active_status(playback_status_of(t)));
                if (active) {
                    proj = t;
                    break;
                }
                json_object_put(t);
            }
            json_object* d = json_object_new_object();
            if (proj) {
                json_object_object_add(d, "task", proj);
            } else {
                json_object* nd = ctx.store->get_doc("node", id);
                json_object* pb = nullptr;
                if (nd) json_object_object_get_ex(nd, "currentPlayback", &pb);
                json_object_object_add(d, "task",
                                       pb ? json_object_get(pb)
                                          : json_object_new_null());
                if (nd) json_object_put(nd);
            }
            dvs::send_json(res, dvs::envelope_ok(d, ""));
            return true;
        }
        if (req.method == "POST" && act == "retire") {
            std::vector<std::string> refs;
            for (auto& bid : ctx.store->list_ids("bindings")) {
                json_object* b = ctx.store->get_doc("bindings", bid);
                if (!b) continue;
                bool hit = (binding_node_of(b) == id);
                json_object_put(b);
                if (hit) refs.push_back("binding:" + bid);
            }
            for (auto& wid : ctx.store->list_ids("whitelist")) {
                json_object* w = ctx.store->get_doc("whitelist", wid);
                if (!w) continue;
                bool hit = (dvs::json_get_str(w, "nodeId") == id);
                json_object_put(w);
                if (hit) refs.push_back("whitelist:" + wid);
            }
            if (!refs.empty()) {
                json_object* d = json_object_new_object();
                json_object* ra = json_object_new_array();
                for (auto& r : refs)
                    json_object_array_add(ra,
                                          json_object_new_string(r.c_str()));
                json_object_object_add(d, "refs", ra);
                ctx.store->audit("{\"retire-rejected\":\"" + dvs::json_escape(id) +
                                 "\"}");
                dvs::send_json(res,
                          dvs::envelope_fail("RESOURCE_IN_USE", 409,
                                        "node still referenced; "
                                        "unbind/unlist first"));
                return true;
            }
            json_object* nd = ctx.store->get_doc("node", id);
            if (!nd) {
                dvs::send_json(res, dvs::envelope_fail("NOT_FOUND", 404, "node unknown"));
                return true;
            }
            bool online = false;
            {
                json_object* v = nullptr;
                if (json_object_object_get_ex(nd, "online", &v) && v)
                    online = json_object_get_boolean(v) != 0;
            }
            json_object_put(nd);
            if (!online) {
                json_object* q = json_object_new_object();
                json_object_object_add(q, "id",
                                       json_object_new_string(id.c_str()));
                json_object_object_add(q, "nodeId",
                                       json_object_new_string(id.c_str()));
                json_object_object_add(q, "status",
                                       json_object_new_string("retire_pending"));
                ctx.store->put_doc("retire-queue", id, q);
                json_object_put(q);
                ctx.store->audit("{\"retire-pending\":\"" + dvs::json_escape(id) +
                                 "\"}");
                json_object* d = json_object_new_object();
                json_object_object_add(d, "status",
                                       json_object_new_string("retire_pending"));
                dvs::send_json(res, dvs::envelope_ok(d, "retire pending, offline"));
                return true;
            }
            ctx.store->tombstone_add(id, dvs::wall_sec_now());
            ctx.store->del_doc("node", id);
            ctx.store->del_doc("retire-queue", id);
            ctx.store->audit("{\"retire\":\"" + dvs::json_escape(id) + "\"}");
            dvs::send_json(res, dvs::envelope_ok(json_object_new_object(), "retired"));
            return true;
        }
        if (req.method == "POST" && act == "config-push") {
            json_object* cur = ctx.store->get_doc("node", id);
            if (!cur) {
                dvs::send_json(res, dvs::envelope_fail("NOT_FOUND", 404, "node unknown"));
                return true;
            }
            std::string base = dvs::json_get_str(cur, "agentBase");
            json_object* body2 = json_tokener_parse(req.body.c_str());
            json_object* cfg = nullptr;
            if (body2) json_object_object_get_ex(body2, "config", &cfg);
            json_object* d = json_object_new_object();
            json_object* ignored = json_object_new_array();
            json_object* fwd = json_object_new_object();
            if (cfg && json_object_is_type(cfg, json_type_object)) {
                json_object_object_foreach(cfg, key, val) {
                    std::string k(key);
                    if (ct::config_update_key_allowed(k))
                        json_object_object_add(fwd, k.c_str(),
                                               json_object_get(val));
                    else {
                        json_object_array_add(
                            ignored, json_object_new_string(k.c_str()));
                        ctx.store->audit("{\"config-push-ignored\":\"" +
                                         dvs::json_escape(k) + "\"}");
                    }
                }
            }
            json_object_object_add(d, "ignored", ignored);
            if (!json_object_object_length(fwd)) {
                json_object_put(fwd);
                json_object_object_add(d, "status",
                                       json_object_new_string("skipped"));
            } else {
                json_object* sp = json_object_new_object();
                json_object_object_add(sp, "config", json_object_get(fwd));
                json_object_put(fwd);
                std::string verr = ct::validate_config_update(sp);
                bool via_ws = false;
                if (verr.empty())
                    via_ws = ws_push(ctx.channels, id, ct::kConfigUpdate,
                                     json_object_get(sp));
                if (via_ws) {
                    json_object_object_add(d, "status",
                                           json_object_new_string("pushed"));
                    json_object_object_add(d, "via",
                                           json_object_new_string("ws"));
                    json_object_put(sp);
                } else if (!base.empty()) {
                    HttpClient cli;
                    const char* spb = json_object_to_json_string(sp);
                    std::string resp;
                    int sc = cli.post_json(dvs::url_join(base, "/api/v1/node/config/sync"),
                                           std::string(spb ? spb : "{}"), resp,
                                           5000, 2, ctx.api_token);
                    json_object_object_add(
                        d, "status",
                        json_object_new_string(sc == 200 ? "pushed" : "failed"));
                    json_object_object_add(d, "via",
                                           json_object_new_string("short"));
                    json_object_put(sp);
                } else {
                    json_object_object_add(d, "status",
                                           json_object_new_string("skipped"));
                    json_object_put(sp);
                }
            }
            if (body2) json_object_put(body2);
            json_object_put(cur);
            dvs::send_json(res, dvs::envelope_ok(d, "config-push"));
            return true;
        }
        return false;
    }
    if (req.method == "GET" && req.path == "/api/v1/nodes") {
        json_object* arr = json_object_new_array();
        for (auto& i : ctx.store->list_ids("node")) {
            json_object* d = ctx.store->get_doc("node", i);
            if (d) json_object_array_add(arr, d);
        }
        json_object* d = json_object_new_object();
        json_object_object_add(d, "items", arr);
        dvs::send_json(res, dvs::envelope_ok(d, ""));
        return true;
    }
    return false;
}
