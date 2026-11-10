// docs_routes: generic document CRUD mounted at /api/v1/<kind>[/<id>].
// Pure document admission (shape, uniqueness, references, soft delete);
// barrier, gateway and upgrade paths stay in main.cpp.
#include "docs_routes.h"

#include "dvs_contract.h"
#include "dvs_http_json.h"
#include "layout.h"

static std::mutex g_idem_mu;
static std::map<std::string, std::string> g_idem_cache;

// Generic docs CRUD mounted at /api/v1/<kind>[/<id>].
// Stage 1: per-kind shape validation, uniqueness and reference checks.
// Walls use soft delete; lists filter deleted docs.
bool route_docs(DocsCtx& ctx, const std::string& kind, const HttpRequest& req,
                HttpResponse& res) {
    std::string base = "/api/v1/" + kind;
    if (req.path != base && req.path.rfind(base + "/", 0) != 0) return false;
    std::string id = (req.path.size() > base.size()) ? req.path.substr(base.size() + 1) : "";
    if (req.method == "GET" && id.empty()) {
        json_object* arr = json_object_new_array();
        for (auto& i : ctx.store->list_ids(kind)) {
            json_object* d = ctx.store->get_doc(kind, i);
            if (!d) continue;
            if (kind == "walls" && wall_is_deleted(d)) { json_object_put(d); continue; }
            json_object_array_add(arr, d);
        }
        json_object* d = json_object_new_object();
        json_object_object_add(d, "items", arr);
        dvs::send_json(res, dvs::envelope_ok(d, ""));
        return true;
    }
    if (req.method == "GET") {
        json_object* d = ctx.store->get_doc(kind, id);
        if (!d) { dvs::send_json(res, dvs::envelope_fail("NOT_FOUND", 404, "not found")); return true; }
        if (kind == "walls" && wall_is_deleted(d)) {
            json_object_put(d);
            dvs::send_json(res, dvs::envelope_fail("GONE", 410, "wall deleted"));
            return true;
        }
        dvs::send_json(res, dvs::envelope_ok(d, ""));
        return true;
    }
    if (req.method == "POST" || req.method == "PUT") {
        // Serialize admission + id generation + store so concurrent creates
        // cannot collide on names, addresses or random ids.
        std::unique_lock<std::mutex> alloc_lk;
        if (ctx.alloc_mu) alloc_lk = std::unique_lock<std::mutex>(*ctx.alloc_mu);
        std::string ikey = dvs::idempotency_key(req);
        std::string cache_key;
        if (!ikey.empty()) {
            std::lock_guard<std::mutex> lk(g_idem_mu);
            auto it = g_idem_cache.find(req.method + " " + req.path + "#" + ikey);
            if (it != g_idem_cache.end()) {
                res.body = it->second;
                res.content_type = "application/json; charset=utf-8";
                return true;
            }
            cache_key = req.method + " " + req.path + "#" + ikey;
        }
        json_object* b = json_tokener_parse(req.body.c_str());
        if (!b) b = json_object_new_object();
        if (id.empty()) {
            id = dvs::json_get_str(b, "id");
            if (id.empty()) {
                id = platform_new_doc_id(kind);
                json_object_object_add(b, "id", json_object_new_string(id.c_str()));
            }
        }
        // Per-kind admission: shape first, then references. Any violation
        // rejects the whole write; nothing is stored on failure.
        std::string verr;
        if (kind == "groups") {
            verr = validate_group_shape(b);
            if (verr.empty()) {
                std::string want = docs_lower(docs_trim(dvs::json_get_str(b, "name")));
                for (auto& gid : ctx.store->list_ids("groups")) {
                    if (gid == id) continue;
                    json_object* g = ctx.store->get_doc("groups", gid);
                    if (!g) continue;
                    bool dup = (docs_lower(docs_trim(dvs::json_get_str(g, "name"))) == want);
                    json_object_put(g);
                    if (dup) { verr = "CONFLICT:group name already exists"; break; }
                }
            }
        } else if (kind == "walls") {
            verr = validate_wall_shape(b);
            json_object* old = verr.empty() ? ctx.store->get_doc("walls", id) : nullptr;
            if (verr.empty() && old && wall_is_deleted(old)) {
                // Deleted walls are immutable except via restore.
                verr = "GONE:wall deleted, restore first";
            }
            if (verr.empty() && old) {
                // Geometry freeze: cell grid and wall pixels never change.
                int ow = 0, oh = 0, nw = 0, nh = 0;
                wall_px_of(old, &ow, &oh);
                wall_px_of(b, &nw, &nh);
                if (ow > 0 && nw > 0 && (ow != nw || oh != nh))
                    verr = "INVALID_ARGUMENT:wall pixels frozen, rebuild the wall";
            }
            if (verr.empty() && old) {
                json_object* ocg = nullptr;
                json_object* ncg = nullptr;
                bool has_o = json_object_object_get_ex(old, "cell_grid", &ocg) && ocg;
                bool has_n = json_object_object_get_ex(b, "cell_grid", &ncg) && ncg;
                if (has_o && has_n) {
                    const char* os = json_object_to_json_string(ocg);
                    const char* ns = json_object_to_json_string(ncg);
                    std::string a = os ? os : "", cc = ns ? ns : "";
                    if (a != cc) verr = "INVALID_ARGUMENT:wall cell_grid frozen";
                }
            }
            if (old) json_object_put(old);
            if (verr.empty()) {
                // Wall name unique within its group (canonical groupId only).
                std::string g = dvs::json_get_str(b, "groupId");
                std::string want = docs_lower(docs_trim(dvs::json_get_str(b, "name")));
                for (auto& wid : ctx.store->list_ids("walls")) {
                    if (wid == id) continue;
                    json_object* w = ctx.store->get_doc("walls", wid);
                    if (!w) continue;
                    if (wall_is_deleted(w)) { json_object_put(w); continue; }
                    std::string wg = dvs::json_get_str(w, "groupId");
                    bool dup = (!g.empty() && wg == g &&
                                docs_lower(docs_trim(dvs::json_get_str(w, "name"))) == want);
                    json_object_put(w);
                    if (dup) { verr = "CONFLICT:wall name already exists in group"; break; }
                }
            }
            if (verr.empty()) {
                // Epoch freeze: the frame time base never moves once set.
                // Updates that drop it inherit the stored one; updates that
                // move it are rejected so frame numbers stay stable.
                json_object* oldw = ctx.store->get_doc("walls", id);
                int64_t old_epoch = 0;
                if (oldw) {
                    json_object* oev = nullptr;
                    if (json_object_object_get_ex(oldw, "epochTaiNs", &oev) && oev &&
                        json_object_is_type(oev, json_type_int))
                        old_epoch = json_object_get_int64(oev);
                    json_object_put(oldw);
                }
                json_object* ev = nullptr;
                bool has_epoch =
                    json_object_object_get_ex(b, "epochTaiNs", &ev) && ev &&
                    json_object_is_type(ev, json_type_int);
                int64_t want_epoch = has_epoch ? json_object_get_int64(ev) : 0;
                if (has_epoch && want_epoch <= 0)
                    verr = "INVALID_ARGUMENT:wall epochTaiNs must be positive";
                else if (has_epoch && old_epoch > 0 && want_epoch != old_epoch)
                    verr = "INVALID_ARGUMENT:wall epochTaiNs frozen";
                else if (!has_epoch || want_epoch <= 0) {
                    int64_t use = old_epoch > 0 ? old_epoch : (int64_t)dvs::wall_ns_now();
                    json_object_object_del(b, "epochTaiNs");
                    json_object_object_add(b, "epochTaiNs",
                                           json_object_new_int64(use));
                }
            }
            if (verr.empty()) {
                json_object* biv = nullptr;
                if (json_object_object_get_ex(b, "baseImage", &biv) && biv)
                    verr = validate_base_image(biv);
            }
            if (verr.empty()) {
                json_object* iav = nullptr;
                if (json_object_object_get_ex(b, "imageAdjust", &iav) && iav)
                    verr = validate_image_adjust(iav);
            }
        } else if (kind == "scenes") {
            int ww = 0, hh = 0;
            std::string wall_id = dvs::json_get_str(b, "wallId");
            if (!wall_id.empty()) {
                json_object* w = ctx.store->get_doc("walls", wall_id);
                if (!w || wall_is_deleted(w)) {
                    verr = "NOT_FOUND:wall unknown or deleted";
                } else {
                    wall_px_of(w, &ww, &hh);
                }
                if (w) json_object_put(w);
            }
            if (verr.empty()) verr = validate_scene_shape(b, ww, hh);
            if (verr.empty() && !wall_id.empty()) {
                json_object* wdoc = ctx.store->get_doc("walls", wall_id);
                if (wdoc && !wall_is_deleted(wdoc)) {
                    int sw = 0, sh = 0;
                    wall_px_of(wdoc, &sw, &sh);
                    int min_side = wall_min_window_side(wdoc, sw, sh);
                    json_object* wins = nullptr;
                    if (json_object_object_get_ex(b, "windows", &wins) && wins &&
                        json_object_is_type(wins, json_type_array)) {
                        for (size_t i = 0; i < json_object_array_length(wins); i++) {
                            json_object* w = json_object_array_get_idx(wins, (int)i);
                            int win_w = dvs::json_rect_int(w, "w");
                            int win_h = dvs::json_rect_int(w, "h");
                            if (win_w < min_side || win_h < min_side) {
                                verr = "INVALID_ARGUMENT:window below wall minimum size";
                                break;
                            }
                        }
                    }
                    json_object_put(wdoc);
                } else if (wdoc) {
                    json_object_put(wdoc);
                }
            }
            if (verr.empty()) {
                json_object* mv = nullptr;
                bool fixed = json_object_object_get_ex(b, "layoutMode", &mv) && mv &&
                             json_object_is_type(mv, json_type_string) &&
                             std::string(json_object_get_string(mv)) == "fixed";
                if (fixed && !wall_id.empty()) {
                    // Fixed mode: the platform snaps windows to cell bounds;
                    // the snapped geometry is what gets stored and fanned out.
                    json_object* wdoc = ctx.store->get_doc("walls", wall_id);
                    if (wdoc && !wall_is_deleted(wdoc)) {
                        int sw = 0, sh = 0;
                        wall_px_of(wdoc, &sw, &sh);
                        std::vector<std::pair<std::string, Rect>> cells;
                        wall_cells(wdoc, sw, sh, &cells);
                        json_object* wins = nullptr;
                        if (!cells.empty() &&
                            json_object_object_get_ex(b, "windows", &wins) && wins) {
                            json_object* snapped = snap_windows_to_cells(wins, cells);
                            json_object_object_del(b, "windows");
                            json_object_object_add(b, "windows", snapped);
                        }
                    }
                    if (wdoc) json_object_put(wdoc);
                }
            }
        } else if (kind == "bindings") {
            verr = validate_binding_shape(b);
            std::string w = binding_wall_of(b);
            std::string n = binding_node_of(b);
            std::string c = binding_cell_of(b);
            if (verr.empty()) {
                json_object* wall = ctx.store->get_doc("walls", w);
                if (!wall || wall_is_deleted(wall)) verr = "NOT_FOUND:wall unknown or deleted";
                if (wall) json_object_put(wall);
            }
            if (verr.empty()) {
                for (auto& bid : ctx.store->list_ids("bindings")) {
                    if (bid == id) continue;
                    json_object* o = ctx.store->get_doc("bindings", bid);
                    if (!o) continue;
                    bool node_dup = (binding_node_of(o) == n);
                    bool cell_dup = (binding_wall_of(o) == w && !c.empty() &&
                                     binding_cell_of(o) == c);
                    json_object_put(o);
                    if (node_dup) { verr = "CONFLICT:node already bound to a wall"; break; }
                    if (cell_dup) { verr = "CONFLICT:cell already bound"; break; }
                }
            }
            if (verr.empty()) {
                // Port idle: the same node output feeds at most one active
                // binding. Pending-offline bindings hold no port.
                std::string outp = binding_output_of(b);
                if (!outp.empty()) {
                    for (auto& bid : ctx.store->list_ids("bindings")) {
                        if (bid == id) continue;
                        json_object* o = ctx.store->get_doc("bindings", bid);
                        if (!o) continue;
                        std::string st = dvs::json_get_str(o, "status");
                        bool busy = (binding_node_of(o) == n &&
                                     binding_output_of(o) == outp &&
                                     st != "pending" && st != "pending_offline");
                        json_object_put(o);
                        if (busy) {
                            verr = "CONFLICT:node output port already bound";
                            break;
                        }
                    }
                }
            }
            if (verr.empty()) {
                // Binding constraint: a referenced signal must be whitelisted.
                std::string sig = binding_sig_of(b);
                if (!sig.empty()) {
                    bool found = false;
                    for (auto& wid : ctx.store->list_ids("whitelist")) {
                        json_object* o = ctx.store->get_doc("whitelist", wid);
                        if (!o) continue;
                        bool hit = (dvs::json_get_str(o, "sig") == sig);
                        json_object_put(o);
                        if (hit) { found = true; break; }
                    }
                    if (!found)
                        verr = "NOT_FOUND:binding signal not in whitelist";
                }
            }
            if (verr.empty()) {
                // Online nodes activate immediately; offline stay pending.
                json_object* nd = ctx.store->get_doc("node", n);
                bool online = false;
                if (nd) {
                    json_object* v = nullptr;
                    if (json_object_object_get_ex(nd, "online", &v) && v)
                        online = json_object_get_boolean(v) != 0;
                }
                if (nd) json_object_put(nd);
                json_object_object_del(b, "status");
                json_object_object_add(b, "status", json_object_new_string(
                    online ? "active" : "pending_offline"));
            }
        } else if (kind == "whitelist") {
            verr = validate_whitelist_shape(b);
            if (verr.empty()) {
                std::string addr;
                int port = 0;
                whitelist_addr_port(b, &addr, &port);
                if (port <= 0) {
                    port = dvs::contract::kDefaultMediaPort;
                    json_object_object_del(b, "port");
                    json_object_object_add(b, "port", json_object_new_int(port));
                }
                for (auto& wid : ctx.store->list_ids("whitelist")) {
                    if (wid == id) continue;
                    json_object* o = ctx.store->get_doc("whitelist", wid);
                    if (!o) continue;
                    std::string oa;
                    int op = 0;
                    whitelist_addr_port(o, &oa, &op);
                    if (op <= 0) op = dvs::contract::kDefaultMediaPort;
                    bool dup = (!addr.empty() && oa == addr && op == port);
                    json_object_put(o);
                    if (dup) { verr = "CONFLICT:multicast address+port already used"; break; }
                }
            }
            if (verr.empty()) {
                if (!dvs::json_get_str(b, "status").empty() && dvs::json_get_str(b, "status") == "conflict")
                    verr = "INVALID_ARGUMENT:whitelist status is server-computed";
                else {
                    json_object_object_del(b, "status");
                    json_object_object_add(b, "status",
                                           json_object_new_string("active"));
                }
            }
        } else if (kind == "playback-tasks") {
            // Strict contract: canonical wallId/sceneId/targetNodeId only.
            // Frontend dialects (wall/scene/node/targetNodeIds) are rejected
            // by the shape validator instead of being silently normalized.
            if (verr.empty()) verr = validate_playback_shape(b);
            std::string want_st = playback_status_of(b);
            if (want_st.empty()) want_st = "pending_check";
            json_object* old = verr.empty() ? ctx.store->get_doc("playback-tasks", id) : nullptr;
            std::string from = old ? playback_status_of(old) : "";
            if (verr.empty() && !playback_transition_ok(from, want_st))
                verr = "CONFLICT:illegal playback transition";
            if (verr.empty() && playback_active_status(want_st)) {
                std::string node = dvs::json_get_str(b, "targetNodeId");
                for (auto& tid : ctx.store->list_ids("playback-tasks")) {
                    if (tid == id) continue;
                    json_object* o = ctx.store->get_doc("playback-tasks", tid);
                    if (!o) continue;
                    std::string on = dvs::json_get_str(o, "targetNodeId");
                    bool busy = (on == node && playback_active_status(playback_status_of(o)));
                    json_object_put(o);
                    if (busy) { verr = "CONFLICT:node already has an active playback"; break; }
                }
            }
            if (old) json_object_put(old);
            if (verr.empty() && playback_status_of(b).empty()) {
                json_object_object_add(b, "status",
                                       json_object_new_string("pending_check"));
            }
        }
        if (!verr.empty()) {
            std::string code = "INVALID_ARGUMENT";
            int status = 400;
            if (verr.rfind("CONFLICT:", 0) == 0) { code = "CONFLICT"; status = 409; }
            else if (verr.rfind("NOT_FOUND:", 0) == 0) { code = "NOT_FOUND"; status = 404; }
            else if (verr.rfind("GONE:", 0) == 0) { code = "GONE"; status = 410; }
            ctx.store->audit("{\"rejected-" + dvs::json_escape(kind) + "\":\"" +
                           dvs::json_escape(id) + "\"}");
            json_object_put(b);
            dvs::send_json(res, dvs::envelope_fail(code.c_str(), status, verr));
            return true;
        }
        bool is_new_wall = false;
        if (kind == "walls") {
            json_object* exist = ctx.store->get_doc("walls", id);
            is_new_wall = (exist == nullptr);
            if (exist) json_object_put(exist);
        }
        ctx.store->put_doc(kind, id, b);
        ctx.store->audit("{\"kind\":\"" + dvs::json_escape(kind) + "\",\"id\":\"" + dvs::json_escape(id) + "\"}");
        if (is_new_wall) {
            // New walls start with an empty default scene so switching and
            // clear-screen have a revision-1 baseline to roll back to.
            std::string sid = id + "-default";
            json_object* sexist = ctx.store->get_doc("scenes", sid);
            if (!sexist) {
                json_object* sc = json_object_new_object();
                json_object_object_add(sc, "id", json_object_new_string(sid.c_str()));
                json_object_object_add(sc, "wallId", json_object_new_string(id.c_str()));
                json_object_object_add(sc, "name", json_object_new_string("default"));
                json_object_object_add(sc, "snapshotVersion", json_object_new_int(1));
                json_object_object_add(sc, "volume", json_object_new_int(default_volume()));
                json_object_object_add(sc, "layoutMode", json_object_new_string("free"));
                json_object_object_add(sc, "windows", json_object_new_array());
                ctx.store->put_doc("scenes", sid, sc);
                json_object_put(sc);
                ctx.store->audit("{\"default-scene\":\"" + dvs::json_escape(sid) + "\"}");
            } else {
                json_object_put(sexist);
            }
            json_object* w0 = ctx.store->get_doc("walls", id);
            if (w0) {
                if (dvs::json_get_str(w0, "currentSceneId").empty()) {
                    json_object_object_add(w0, "currentSceneId",
                                           json_object_new_string(sid.c_str()));
                    ctx.store->put_doc("walls", id, w0);
                }
                json_object_put(w0);
            }
        }
        json_object* d = ctx.store->get_doc(kind, id);
        json_object_put(b);
        dvs::send_json(res, dvs::envelope_ok(d ? d : json_object_new_object(), ""));
        if (!cache_key.empty()) {
            std::lock_guard<std::mutex> lk(g_idem_mu);
            if (g_idem_cache.size() < 1024) g_idem_cache[cache_key] = res.body;
        }
        return true;
    }
    if (req.method == "DELETE") {
        if (kind == "groups") {
            int walls = 0;
            for (auto& wid : ctx.store->list_ids("walls")) {
                json_object* w = ctx.store->get_doc("walls", wid);
                if (!w) continue;
                bool hit = false;
                if (!wall_is_deleted(w)) {
                    std::string g = dvs::json_get_str(w, "groupId");
                    hit = (g == id);
                }
                json_object_put(w);
                if (hit) walls++;
            }
            if (walls > 0) {
                json_object* d = json_object_new_object();
                json_object_object_add(d, "wallCount", json_object_new_int(walls));
                json_object_put(d);
                dvs::send_json(res, dvs::envelope_fail("RESOURCE_IN_USE", 409,
                    "group still has " + std::to_string(walls) + " wall(s)"));
                return true;
            }
            ctx.store->del_doc(kind, id);
            dvs::send_json(res, dvs::envelope_ok(json_object_new_object(), ""));
            return true;
        }
        if (kind == "walls") {
            json_object* w = ctx.store->get_doc("walls", id);
            if (!w) {
                dvs::send_json(res, dvs::envelope_fail("NOT_FOUND", 404, "not found"));
                return true;
            }
            if (wall_is_deleted(w)) {
                json_object_put(w);
                dvs::send_json(res, dvs::envelope_fail("GONE", 410, "wall already deleted"));
                return true;
            }
            json_object_put(w);
            {
                std::lock_guard<std::mutex> lk(*ctx.ops_mu);
                if (ctx.ops->wall_has_preparing(id, "")) {
                    dvs::send_json(res, dvs::envelope_fail("RESOURCE_IN_USE", 409,
                        "wall has an operation in flight"));
                    return true;
                }
            }
            for (auto& tid : ctx.store->list_ids("playback-tasks")) {
                json_object* t = ctx.store->get_doc("playback-tasks", tid);
                if (!t) continue;
                bool busy = (dvs::json_get_str(t, "wallId") == id &&
                             playback_active_status(playback_status_of(t)));
                json_object_put(t);
                if (busy) {
                    dvs::send_json(res, dvs::envelope_fail("RESOURCE_IN_USE", 409,
                        "wall has an active playback task"));
                    return true;
                }
            }
            for (auto& bid : ctx.store->list_ids("bindings")) {
                json_object* b = ctx.store->get_doc("bindings", bid);
                if (!b) continue;
                bool hit = (binding_wall_of(b) == id);
                json_object_put(b);
                if (hit) {
                    dvs::send_json(res, dvs::envelope_fail("RESOURCE_IN_USE", 409,
                        "wall still has bindings; unbind first"));
                    return true;
                }
            }
            // Soft delete: keep the doc 7 days for restore, hide from lists.
            json_object* cur = ctx.store->get_doc("walls", id);
            if (cur) {
                json_object_object_del(cur, "deleted");
                json_object_object_add(cur, "deleted", json_object_new_boolean(1));
                json_object_object_del(cur, "deletedAt");
                json_object_object_add(cur, "deletedAt",
                    json_object_new_string(dvs::iso_now().c_str()));
                json_object_object_del(cur, "deletedAtEpoch");
                json_object_object_add(cur, "deletedAtEpoch",
                    json_object_new_int64((int64_t)dvs::wall_sec_now()));
                ctx.store->put_doc("walls", id, cur);
                json_object_put(cur);
            }
            ctx.store->audit("{\"wall-soft-delete\":\"" + dvs::json_escape(id) + "\"}");
            dvs::send_json(res, dvs::envelope_ok(json_object_new_object(), "soft deleted"));
            return true;
        }
        if (kind == "scenes") {
            json_object* sc = ctx.store->get_doc("scenes", id);
            std::string wall_id;
            if (sc) {
                wall_id = dvs::json_get_str(sc, "wallId");
                json_object_put(sc);
            }
            if (!wall_id.empty()) {
                json_object* w = ctx.store->get_doc("walls", wall_id);
                if (w) {
                    bool eff = (dvs::json_get_str(w, "currentSceneId") == id);
                    json_object_put(w);
                    if (eff) {
                        dvs::send_json(res, dvs::envelope_fail("RESOURCE_IN_USE", 409,
                            "scene is the effective scene"));
                        return true;
                    }
                }
            }
            ctx.store->del_doc(kind, id);
            dvs::send_json(res, dvs::envelope_ok(json_object_new_object(), ""));
            return true;
        }
        if (kind == "whitelist") {
            json_object* wl = ctx.store->get_doc("whitelist", id);
            std::string sig;
            if (wl) {
                sig = dvs::json_get_str(wl, "sig");
                json_object_put(wl);
            }
            if (sig.empty()) sig = id;
            for (auto& sid : ctx.store->list_ids("scenes")) {
                json_object* sc = ctx.store->get_doc("scenes", sid);
                if (!sc) continue;
                bool hit = false;
                json_object* wins = nullptr;
                if (json_object_object_get_ex(sc, "windows", &wins) && wins &&
                    json_object_is_type(wins, json_type_array)) {
                    for (size_t i = 0; i < json_object_array_length(wins); i++) {
                        json_object* e = json_object_array_get_idx(wins, i);
                        if (dvs::json_get_str(e, "sig") == sig) { hit = true; break; }
                    }
                }
                json_object_put(sc);
                if (hit) {
                    dvs::send_json(res, dvs::envelope_fail("RESOURCE_IN_USE", 409,
                        "signal still referenced by a scene"));
                    return true;
                }
            }
            ctx.store->del_doc(kind, id);
            dvs::send_json(res, dvs::envelope_ok(json_object_new_object(), ""));
            return true;
        }
        if (kind == "playback-tasks") {
            json_object* t = ctx.store->get_doc("playback-tasks", id);
            if (!t) {
                dvs::send_json(res, dvs::envelope_fail("NOT_FOUND", 404, "not found"));
                return true;
            }
            std::string st = playback_status_of(t);
            json_object_put(t);
            if (st == "pending_check" || st == "pending_execution" ||
                st == "executing" || st == "playing") {
                dvs::send_json(res, dvs::envelope_fail("RESOURCE_IN_USE", 409,
                    "playback task still active"));
                return true;
            }
            ctx.store->del_doc(kind, id);
            dvs::send_json(res, dvs::envelope_ok(json_object_new_object(), ""));
            return true;
        }
        ctx.store->del_doc(kind, id);
        dvs::send_json(res, dvs::envelope_ok(json_object_new_object(), ""));
        return true;
    }
    return false;
}
