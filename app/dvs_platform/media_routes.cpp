#include "media_routes.h"

#include <cstdio>
#include <cstring>
#include <set>

#include "dvs_http_json.h"
#include "layout.h"
#include "platform_context.h"
#include "platform_store.h"

bool route_media(PlatformContext& ctx, const HttpRequest& req,
                 HttpResponse& res) {
    if (req.method == "POST" && req.path == "/api/v1/scenes/compare") {
        json_object* b = json_tokener_parse(req.body.c_str());
        std::string sid = dvs::json_get_str(b, "sceneId");
        if (sid.empty()) sid = dvs::json_get_str(b, "id");
        json_object* live_wins = nullptr;
        if (b) json_object_object_get_ex(b, "windows", &live_wins);
        int live_vol = 80;
        json_object* vv = nullptr;
        if (b && json_object_object_get_ex(b, "volume", &vv) && vv)
            live_vol = json_object_get_int(vv);
        json_object* sc =
            sid.empty() ? nullptr : ctx.store->get_doc("scenes", sid);
        if (!sc) {
            if (b) json_object_put(b);
            dvs::send_json(res, dvs::envelope_fail("NOT_FOUND", 404, "scene unknown"));
            return true;
        }
        json_object* saved_wins = nullptr;
        json_object_object_get_ex(sc, "windows", &saved_wins);
        int saved_vol = 80;
        json_object* sv = nullptr;
        if (json_object_object_get_ex(sc, "volume", &sv) && sv)
            saved_vol = json_object_get_int(sv);
        json_object* d =
            layout_dirty_compare(saved_wins, saved_vol, live_wins, live_vol);
        json_object_put(sc);
        if (b) json_object_put(b);
        dvs::send_json(res, dvs::envelope_ok(d, "compared"));
        return true;
    }
    if (req.method == "POST" && req.path.rfind("/api/v1/walls/", 0) == 0 &&
        req.path.size() > strlen("/api/v1/walls/") + 8 &&
        req.path.rfind("/restore") == req.path.size() - 8) {
        std::string wid = req.path.substr(strlen("/api/v1/walls/"));
        wid = wid.substr(0, wid.size() - 8);
        json_object* w = ctx.store->get_doc("walls", wid);
        if (!w) {
            dvs::send_json(res, dvs::envelope_fail("NOT_FOUND", 404, "wall unknown"));
            return true;
        }
        if (!wall_is_deleted(w)) {
            json_object_put(w);
            dvs::send_json(res, dvs::envelope_fail("INVALID_ARGUMENT", 400,
                                         "wall not deleted"));
            return true;
        }
        json_object_object_del(w, "deleted");
        json_object_object_del(w, "deletedAt");
        ctx.store->put_doc("walls", wid, w);
        json_object_put(w);
        ctx.store->audit("{\"wall-restore\":\"" + dvs::json_escape(wid) + "\"}");
        json_object* d = ctx.store->get_doc("walls", wid);
        dvs::send_json(res,
                  dvs::envelope_ok(d ? d : json_object_new_object(), "restored"));
        return true;
    }
    if (req.method == "GET" && req.path.rfind("/api/v1/walls/", 0) == 0 &&
        req.path.size() >= 11 &&
        req.path.rfind("/base-image") == req.path.size() - 11) {
        std::string mid = req.path.substr(strlen("/api/v1/walls/"));
        size_t sl = mid.find('/');
        std::string wid = (sl == std::string::npos) ? mid : mid.substr(0, sl);
        std::string node_id;
        auto qi = req.query.find("node");
        if (qi != req.query.end()) node_id = qi->second;
        json_object* w = ctx.store->get_doc("walls", wid);
        if (!w || wall_is_deleted(w)) {
            if (w) json_object_put(w);
            dvs::send_json(res, dvs::envelope_fail("NOT_FOUND", 404,
                                         "wall unknown or deleted"));
            return true;
        }
        int ww = 0, hh = 0;
        wall_px_of(w, &ww, &hh);
        std::string bver = dvs::json_get_str(w, "baseImageRevision");
        if (bver.empty()) bver = dvs::json_get_str(w, "backdropRevision");
        json_object_put(w);
        if (node_id.empty()) {
            dvs::send_json(res,
                      dvs::envelope_fail("INVALID_ARGUMENT", 400, "need ?node="));
            return true;
        }
        json_object* nd = ctx.store->get_doc("node", node_id);
        if (!nd) {
            dvs::send_json(res, dvs::envelope_fail("NOT_FOUND", 404, "node unknown"));
            return true;
        }
        json_object* geo = nullptr;
        json_object_object_get_ex(nd, "geometry", &geo);
        Rect node{dvs::json_rect_int(geo, "x"), dvs::json_rect_int(geo, "y"), dvs::json_rect_int(geo, "w"),
                  dvs::json_rect_int(geo, "h")};
        json_object_put(nd);
        Rect sub;
        if (!base_subrect(ww, hh, node, &sub)) {
            dvs::send_json(res, dvs::envelope_fail("NOT_FOUND", 404,
                                         "node outside wall canvas"));
            return true;
        }
        json_object* d = json_object_new_object();
        json_object_object_add(d, "wallId",
                               json_object_new_string(wid.c_str()));
        json_object_object_add(d, "nodeId",
                               json_object_new_string(node_id.c_str()));
        json_object_object_add(d, "baseImageRevision",
                               json_object_new_string(bver.c_str()));
        json_object_object_add(d, "revision",
                               json_object_new_string(bver.c_str()));
        std::string cache_key = wid + ":" + bver + ":" + node_id;
        json_object_object_add(d, "cacheKey",
                               json_object_new_string(cache_key.c_str()));
        json_object_object_add(d, "wallW", json_object_new_int(ww));
        json_object_object_add(d, "wallH", json_object_new_int(hh));
        json_object* r = json_object_new_object();
        json_object_object_add(r, "x", json_object_new_int(sub.x));
        json_object_object_add(r, "y", json_object_new_int(sub.y));
        json_object_object_add(r, "w", json_object_new_int(sub.w));
        json_object_object_add(r, "h", json_object_new_int(sub.h));
        json_object_object_add(d, "subRect", r);
        dvs::send_json(res, dvs::envelope_ok(d, "base-image"));
        return true;
    }
    if (req.method == "POST" && req.path.rfind("/api/v1/playback-tasks/", 0) == 0) {
        std::string rest = req.path.substr(strlen("/api/v1/playback-tasks/"));
        size_t sl = rest.find('/');
        std::string tid =
            (sl == std::string::npos) ? rest : rest.substr(0, sl);
        std::string verb =
            (sl == std::string::npos) ? "" : rest.substr(sl + 1);
        if (!tid.empty() &&
            (verb == "start" || verb == "cancel" || verb == "finish")) {
            json_object* t = ctx.store->get_doc("playback-tasks", tid);
            if (!t) {
                dvs::send_json(res, dvs::envelope_fail("NOT_FOUND", 404, "task unknown"));
                return true;
            }
            std::string from = playback_status_of(t);
            std::string want;
            if (verb == "start")
                want = "executing";
            else if (verb == "cancel")
                want = "cancelled";
            else
                want = "succeeded";
            if (verb == "start" && from == "pending_execution") {
                std::string wall_id = dvs::json_get_str(t, "wallId");
                std::string node = dvs::json_get_str(t, "targetNodeId");
                json_object* w =
                    wall_id.empty() ? nullptr
                                    : ctx.store->get_doc("walls", wall_id);
                json_object* nd =
                    node.empty() ? nullptr : ctx.store->get_doc("node", node);
                bool ok = (w && !wall_is_deleted(w) && nd);
                if (w) json_object_put(w);
                if (nd) json_object_put(nd);
                if (!ok) {
                    json_object_put(t);
                    dvs::send_json(res, dvs::envelope_fail("INVALID_ARGUMENT", 400,
                                                 "wall/node unavailable for "
                                                 "start"));
                    return true;
                }
            }
            if (!playback_transition_ok(from, want)) {
                json_object_put(t);
                dvs::send_json(res, dvs::envelope_fail("CONFLICT", 409,
                                             "illegal playback transition"));
                return true;
            }
            json_object_object_del(t, "status");
            json_object_object_add(t, "status",
                                   json_object_new_string(want.c_str()));
            ctx.store->put_doc("playback-tasks", tid, t);
            json_object_put(t);
            ctx.store->audit("{\"playback-" + verb + "\":\"" +
                             dvs::json_escape(tid) + "\"}");
            json_object* d = ctx.store->get_doc("playback-tasks", tid);
            dvs::send_json(res,
                      dvs::envelope_ok(d ? d : json_object_new_object(), verb));
            return true;
        }
    }
    if (req.method == "POST" && req.path == "/api/v1/whitelist/allocate") {
        json_object* b = json_tokener_parse(req.body.c_str());
        if (!b) b = json_object_new_object();
        std::string sig = dvs::json_get_str(b, "sig");
        int want_port = 0;
        json_object* pv = nullptr;
        if (json_object_object_get_ex(b, "port", &pv) && pv)
            want_port = json_object_get_int(pv);
        std::string want_addr = dvs::json_get_str(b, "address");
        if (!sig.empty()) {
            json_object* hit = nullptr;
            for (auto& wid : ctx.store->list_ids("whitelist")) {
                json_object* o = ctx.store->get_doc("whitelist", wid);
                if (!o) continue;
                if (dvs::json_get_str(o, "sig") == sig) {
                    hit = o;
                    break;
                }
                json_object_put(o);
            }
            if (hit) {
                json_object_put(b);
                dvs::send_json(res, dvs::envelope_ok(hit, "already allocated"));
                return true;
            }
        }
        std::set<std::pair<std::string, int>> used;
        for (auto& wid : ctx.store->list_ids("whitelist")) {
            json_object* o = ctx.store->get_doc("whitelist", wid);
            if (!o) continue;
            std::string a;
            int p = 0;
            whitelist_addr_port(o, &a, &p);
            if (p <= 0) p = 50010;
            if (!a.empty()) used.insert({a, p});
            json_object_put(o);
        }
        std::string addr;
        int port = 0;
        if (!want_addr.empty()) {
            if (want_port <= 0) want_port = 50010;
            if (want_port < 1 || want_port > 65535) {
                json_object_put(b);
                dvs::send_json(res, dvs::envelope_fail("INVALID_ARGUMENT", 400,
                                             "port must be 1..65535"));
                return true;
            }
            if (used.count({want_addr, want_port}) != 0) {
                json_object_put(b);
                dvs::send_json(res, dvs::envelope_fail("CONFLICT", 409,
                                             "multicast address+port already "
                                             "used"));
                return true;
            }
            addr = want_addr;
            port = want_port;
        } else {
            if (!multicast_pool_next(used, want_port, &addr, &port)) {
                json_object_put(b);
                dvs::send_json(res, dvs::envelope_fail("RESOURCE_EXHAUSTED", 507,
                                             "multicast pool exhausted"));
                return true;
            }
        }
        std::string id = sig;
        if (id.empty()) {
            char nb[48];
            snprintf(nb, sizeof(nb), "wl-%ld", (long)dvs::wall_sec_now());
            id = nb;
            sig = id;
        }
        json_object* doc = json_object_new_object();
        json_object_object_add(doc, "id", json_object_new_string(id.c_str()));
        json_object_object_add(doc, "sig",
                               json_object_new_string(sig.c_str()));
        json_object_object_add(doc, "address",
                               json_object_new_string(addr.c_str()));
        json_object_object_add(doc, "port", json_object_new_int(port));
        for (const char* k :
             {"codec", "width", "height", "fps", "audioPort", "bitrateKbps",
              "bitrate", "keyFrameInterval", "gop", "mode"}) {
            json_object* kv = nullptr;
            if (json_object_object_get_ex(b, k, &kv) && kv)
                json_object_object_add(doc, k, json_object_get(kv));
        }
        std::string verr = validate_whitelist_shape(doc);
        if (!verr.empty()) {
            json_object_put(doc);
            json_object_put(b);
            dvs::send_json(res,
                      dvs::envelope_fail("INVALID_ARGUMENT", 400, verr.c_str()));
            return true;
        }
        json_object_object_add(doc, "status",
                               json_object_new_string("active"));
        json_object_put(b);
        ctx.store->put_doc("whitelist", id, doc);
        ctx.store->audit("{\"whitelist-allocate\":\"" + dvs::json_escape(id) + "\"}");
        json_object* d = ctx.store->get_doc("whitelist", id);
        json_object_put(doc);
        dvs::send_json(res,
                  dvs::envelope_ok(d ? d : json_object_new_object(), "allocated"));
        return true;
    }
    if (req.method == "POST" && req.path.rfind("/api/v1/walls/", 0) == 0 &&
        req.path.size() >= 11 &&
        req.path.rfind("/base-image") == req.path.size() - 11) {
        std::string wid = req.path.substr(strlen("/api/v1/walls/"));
        wid = wid.substr(0, wid.size() - 11);
        json_object* w = ctx.store->get_doc("walls", wid);
        if (!w || wall_is_deleted(w)) {
            if (w) json_object_put(w);
            dvs::send_json(res, dvs::envelope_fail("NOT_FOUND", 404,
                                         "wall unknown or deleted"));
            return true;
        }
        json_object_put(w);
        json_object* b = json_tokener_parse(req.body.c_str());
        if (!b) b = json_object_new_object();
        std::string rev = dvs::json_get_str(b, "revision");
        if (rev.empty()) rev = dvs::json_get_str(b, "baseImageRevision");
        int bw = 0, bh = 0;
        json_object* v = nullptr;
        if (json_object_object_get_ex(b, "width", &v) && v)
            bw = json_object_get_int(v);
        if (json_object_object_get_ex(b, "height", &v) && v)
            bh = json_object_get_int(v);
        std::string url = dvs::json_get_str(b, "url");
        bool bad = rev.empty() || rev.size() > 64 ||
                   (bw != 0 && (bw < 64 || bw > 7680)) ||
                   (bh != 0 && (bh < 64 || bh > 4320)) || url.size() > 512;
        if (bad) {
            json_object_put(b);
            dvs::send_json(res,
                      dvs::envelope_fail("INVALID_ARGUMENT", 400,
                                    "need revision[1..64] + optional "
                                    "width/height/url in range"));
            return true;
        }
        json_object* doc = json_object_new_object();
        json_object_object_add(doc, "id",
                               json_object_new_string(wid.c_str()));
        json_object_object_add(doc, "wallId",
                               json_object_new_string(wid.c_str()));
        json_object_object_add(doc, "revision",
                               json_object_new_string(rev.c_str()));
        if (bw > 0)
            json_object_object_add(doc, "width", json_object_new_int(bw));
        if (bh > 0)
            json_object_object_add(doc, "height", json_object_new_int(bh));
        if (!url.empty())
            json_object_object_add(doc, "url",
                                   json_object_new_string(url.c_str()));
        json_object_object_add(doc, "updatedAt",
                               json_object_new_string(dvs::iso_now().c_str()));
        json_object_put(b);
        ctx.store->put_doc("base-images", wid, doc);
        json_object_put(doc);
        json_object* cur = ctx.store->get_doc("walls", wid);
        if (cur) {
            json_object_object_del(cur, "baseImageRevision");
            json_object_object_add(cur, "baseImageRevision",
                                   json_object_new_string(rev.c_str()));
            ctx.store->put_doc("walls", wid, cur);
            json_object_put(cur);
        }
        ctx.store->audit("{\"base-image\":\"" + dvs::json_escape(wid) + "\"}");
        json_object* d = ctx.store->get_doc("base-images", wid);
        dvs::send_json(res,
                  dvs::envelope_ok(d ? d : json_object_new_object(), "stored"));
        return true;
    }
    if (req.method == "GET" && req.path.rfind("/api/v1/walls/", 0) == 0 &&
        req.path.size() >= 19 &&
        req.path.rfind("/base-image-version") == req.path.size() - 19) {
        std::string wid = req.path.substr(strlen("/api/v1/walls/"));
        wid = wid.substr(0, wid.size() - 19);
        json_object* w = ctx.store->get_doc("walls", wid);
        if (!w || wall_is_deleted(w)) {
            if (w) json_object_put(w);
            dvs::send_json(res, dvs::envelope_fail("NOT_FOUND", 404,
                                         "wall unknown or deleted"));
            return true;
        }
        json_object_put(w);
        json_object* d = ctx.store->get_doc("base-images", wid);
        if (!d) {
            dvs::send_json(res, dvs::envelope_fail("NOT_FOUND", 404, "no base image"));
            return true;
        }
        std::string rev = dvs::json_get_str(d, "revision");
        json_object_object_add(d, "wallId",
                               json_object_new_string(wid.c_str()));
        json_object_object_add(d, "cacheKey",
                               json_object_new_string((wid + ":" + rev).c_str()));
        dvs::send_json(res, dvs::envelope_ok(d, "base-image-version"));
        return true;
    }
    return false;
}
