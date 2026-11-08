#include "layout_fanout.h"

#include <thread>
#include <vector>

#include "channels.h"
#include "dvs_contract.h"
#include "http_client.h"
#include "http_json.h"
#include "layout.h"
#include "platform_context.h"
#include "platform_store.h"

namespace ct = dvs::contract;

LayoutDelivery deliver_layout_prepare(
    PlatformContext& ctx, const std::string& node_id, const std::string& opid,
    int revision, int64_t apply_at_ns, json_object* wins, int volume,
    const std::string& token, const std::string& wall_id,
    const std::string& scene_id, const std::string& layout_hash,
    int64_t epoch_ns, double fps, int64_t frame_index, int wall_w, int wall_h,
    const std::string& base_json, const std::string& adjust_json) {
    LayoutDelivery d;
    json_object* nd = ctx.store->get_doc("node", node_id);
    if (!nd) {
        d.skipped = true;
        d.skip_reason = "no node doc";
        return d;
    }
    std::string agent_base = body_str(nd, "agentBase");
    json_object* geo = nullptr;
    json_object_object_get_ex(nd, "geometry", &geo);
    bool geo_ok = geo && rect_int(geo, "w") > 0 && rect_int(geo, "h") > 0;
    if (!geo_ok) {
        d.skipped = true;
        d.skip_reason = "no geometry";
        json_object_put(nd);
        return d;
    }
    Rect node{rect_int(geo, "x"), rect_int(geo, "y"), rect_int(geo, "w"),
              rect_int(geo, "h")};
    json_object* frag = fragment_for_node(wins, node);
    std::set<std::string> sigs;
    int fn = json_object_array_length(frag);
    for (int i = 0; i < fn; i++)
        sigs.insert(body_str(json_object_array_get_idx(frag, i), "sig"));
    json_object* endpoints = json_object_new_array();
    if (!sigs.empty()) {
        for (auto& wid : ctx.store->list_ids("whitelist")) {
            json_object* wl = ctx.store->get_doc("whitelist", wid);
            if (!wl) continue;
            std::string wsig = body_str(wl, "sig");
            if (!sigs.count(wsig)) {
                json_object_put(wl);
                continue;
            }
            json_object* ep = json_object_new_object();
            json_object_object_add(ep, "sig",
                                   json_object_new_string(wsig.c_str()));
            json_object_object_add(
                ep, "address",
                json_object_new_string(body_str(wl, "address").c_str()));
            json_object* pv = nullptr;
            int port = 0;
            if (json_object_object_get_ex(wl, "port", &pv))
                port = json_object_get_int(pv);
            if (port <= 0) port = 50010;
            json_object_object_add(ep, "port", json_object_new_int(port));
            json_object* fv = nullptr;
            int fps = 0;
            if (json_object_object_get_ex(wl, "fps", &fv) && fv)
                fps = json_object_get_int(fv);
            if (fps <= 0) fps = 60;
            json_object_object_add(ep, "fps", json_object_new_int(fps));
            json_object* apv = nullptr;
            int aport = 0;
            if (json_object_object_get_ex(wl, "audioPort", &apv) && apv)
                aport = json_object_get_int(apv);
            if (aport > 0)
                json_object_object_add(ep, "audioPort",
                                       json_object_new_int(aport));
            std::string mode = body_str(wl, "mode");
            json_object_object_add(
                ep, "mode",
                json_object_new_string(mode.empty() ? "multicast" : mode.c_str()));
            std::string ssrc = body_str(wl, "sourceIp");
            if (!ssrc.empty())
                json_object_object_add(ep, "sourceIp",
                                       json_object_new_string(ssrc.c_str()));
            json_object_array_add(endpoints, ep);
            json_object_put(wl);
        }
    }
    std::string audio_owner;
    if (wins) {
        int wn = json_object_array_length(wins);
        for (int i = wn - 1; i >= 0; i--) {
            json_object* w = json_object_array_get_idx(wins, i);
            json_object* mv = nullptr;
            bool muted = false;
            if (json_object_object_get_ex(w, "muted", &mv))
                muted = json_object_get_boolean(mv);
            std::string wid = body_str(w, "id");
            if (!wid.empty() && !muted) {
                audio_owner = wid;
                break;
            }
        }
    }
    json_object* payload = json_object_new_object();
    json_object_object_add(payload, "operationId",
                           json_object_new_string(opid.c_str()));
    json_object_object_add(payload, "revision", json_object_new_int(revision));
    json_object_object_add(payload, "applyAtNs",
                           json_object_new_int64(apply_at_ns));
    if (epoch_ns > 0) {
        json_object_object_add(payload, "epochTaiNs",
                               json_object_new_int64(epoch_ns));
        if (fps > 0.0)
            json_object_object_add(payload, "fps",
                                   json_object_new_double(fps));
        if (frame_index >= 0)
            json_object_object_add(payload, "frameIndex",
                                   json_object_new_int64(frame_index));
    }
    if (!base_json.empty()) {
        json_object* bi = json_tokener_parse(base_json.c_str());
        if (bi && json_object_is_type(bi, json_type_object)) {
            Rect sub;
            if (base_subrect(wall_w, wall_h, node, &sub)) {
                json_object* subo = json_object_new_object();
                json_object_object_add(subo, "x", json_object_new_int(sub.x));
                json_object_object_add(subo, "y", json_object_new_int(sub.y));
                json_object_object_add(subo, "w", json_object_new_int(sub.w));
                json_object_object_add(subo, "h", json_object_new_int(sub.h));
                json_object_object_add(bi, "sub", subo);
            }
            json_object_object_add(payload, "baseImage", bi);
        } else if (bi) {
            json_object_put(bi);
        }
    }
    if (!adjust_json.empty()) {
        json_object* ia = json_tokener_parse(adjust_json.c_str());
        if (ia && json_object_is_type(ia, json_type_object))
            json_object_object_add(payload, "imageAdjust", ia);
        else if (ia)
            json_object_put(ia);
    }
    json_object_object_add(payload, "geometry", json_object_get(geo));
    json_object_object_add(payload, "windows", frag);
    json_object_object_add(payload, "endpoints", endpoints);
    if (!audio_owner.empty())
        json_object_object_add(payload, "audioOwner",
                               json_object_new_string(audio_owner.c_str()));
    if (volume >= 0)
        json_object_object_add(payload, "volume", json_object_new_int(volume));
    if (!wall_id.empty())
        json_object_object_add(payload, "wallId",
                               json_object_new_string(wall_id.c_str()));
    if (!scene_id.empty())
        json_object_object_add(payload, "sceneId",
                               json_object_new_string(scene_id.c_str()));
    if (!layout_hash.empty())
        json_object_object_add(payload, "layoutHash",
                               json_object_new_string(layout_hash.c_str()));
    bool via_ws = false;
    if (ct::validate_layout_prepare(payload).empty())
        via_ws = ws_push(ctx.channels, node_id, ct::kLayoutPrepare,
                         json_object_get(payload));
    if (via_ws) {
        d.sent = true;
        d.via_ws = true;
    } else if (!agent_base.empty()) {
        const char* pb = json_object_to_json_string(payload);
        std::string resp;
        d.http_status = HttpClient().post_json(
            agent_base + "/api/v1/node/media/layout",
            std::string(pb ? pb : "{}"), resp, 5000, 2, token);
        d.sent = true;
    } else {
        d.skipped = true;
        d.skip_reason = "no channel";
    }
    json_object_put(payload);
    json_object_put(nd);
    return d;
}

void fanout_layout_parallel(PlatformContext& ctx,
                            const std::set<std::string>& members,
                            const std::string& opid, int rev, int64_t apply_at,
                            const std::string& windows_json, int volume,
                            const std::string& token,
                            const std::string& wall_id,
                            const std::string& scene_id,
                            const std::string& hash, int64_t epoch_ns,
                            double fps, int64_t frame_index, int wall_w,
                            int wall_h, const std::string& base_json,
                            const std::string& adjust_json, bool is_rollback) {
    if (members.empty()) return;
    std::vector<std::string> ids(members.begin(), members.end());
    int shards = fanout_parallelism();
    if (shards < 1) shards = 1;
    if (shards > (int)ids.size()) shards = (int)ids.size();
    std::vector<std::thread> workers;
    for (int s = 0; s < shards; s++) {
        workers.emplace_back([s, shards, ids, opid, rev, apply_at, windows_json,
                              volume, token, wall_id, scene_id, hash, epoch_ns,
                              fps, frame_index, wall_w, wall_h, base_json,
                              adjust_json, is_rollback, &ctx]() {
            json_object* wins = json_tokener_parse(windows_json.c_str());
            for (size_t i = (size_t)s; i < ids.size(); i += (size_t)shards) {
                const std::string& mid = ids[i];
                LayoutDelivery d = deliver_layout_prepare(
                    ctx, mid, opid, rev, apply_at, wins, volume, token, wall_id,
                    scene_id, hash, epoch_ns, fps, frame_index, wall_w, wall_h,
                    base_json, adjust_json);
                if (is_rollback) {
                    if (d.skipped && d.skip_reason == "no geometry") continue;
                    if (d.via_ws) {
                        ctx.store->audit("{\"rollback\":\"" + json_escape(opid) +
                                         "\",\"node\":\"" + json_escape(mid) +
                                         "\",\"via\":\"ws\",\"executed\":1}");
                    } else if (d.sent) {
                        ctx.store->audit(
                            "{\"rollback\":\"" + json_escape(opid) +
                            "\",\"node\":\"" + json_escape(mid) +
                            "\",\"executed\":" +
                            std::to_string(d.http_status == 200 ? 1 : 0) + "}");
                    } else {
                        ctx.store->audit(
                            "{\"rollback\":\"" + json_escape(opid) +
                            "\",\"node\":\"" + json_escape(mid) +
                            "\",\"executed\":0,\"reason\":\"" +
                            json_escape(d.skip_reason) + "\"}");
                    }
                } else {
                    if (d.via_ws) {
                        ctx.store->audit("{\"fanout\":\"" + json_escape(mid) +
                                         "\",\"via\":\"ws\"}");
                        ctx.channels.set_transport(mid, "ws");
                    } else if (d.sent) {
                        ctx.store->audit("{\"fanout\":\"" + json_escape(mid) +
                                         "\",\"status\":" +
                                         std::to_string(d.http_status) + "}");
                        ctx.channels.set_transport(mid, "short");
                    } else {
                        ctx.store->audit("{\"fanout-skip\":\"" +
                                         json_escape(mid) + "\",\"reason\":\"" +
                                         json_escape(d.skip_reason) + "\"}");
                    }
                }
            }
            if (wins) json_object_put(wins);
        });
    }
    for (auto& t : workers) t.join();
}

void execute_rollback(PlatformContext& ctx, const std::string& rollback_op_id,
                      int to_revision, const std::set<std::string>& members) {
    const Operation* snap = nullptr;
    {
        std::lock_guard<std::mutex> lk(ctx.ops_mu);
        snap = ctx.ops.find_committed(to_revision);
    }
    if (!snap) {
        ctx.store->audit("{\"rollback\":\"" + json_escape(rollback_op_id) +
                         "\",\"executed\":0,\"reason\":\"no snapshot\"}");
        return;
    }
    std::string opid_c = rollback_op_id;
    int rev_c = to_revision;
    std::string windows_json = snap->windows_json;
    int vol_c = snap->volume;
    std::string wall_c = snap->wall_id;
    std::string scene_c = snap->scene_id;
    std::string hash_c = snap->layout_hash;
    int wall_w_c = snap->wall_w;
    int wall_h_c = snap->wall_h;
    int64_t epoch_c = (int64_t)snap->epoch_ns;
    double fps_c = snap->fps;
    std::string token_c = ctx.api_token;
    {
        std::lock_guard<std::mutex> lk(ctx.ops_mu);
        uint64_t deadline =
            now_ns() + (uint64_t)ct::barrier_timeout_ms() * 1000000ULL;
        Operation& rb = ctx.ops.begin_with_deadline(opid_c, rev_c, members, 0,
                                                    deadline);
        rb.windows_json = windows_json;
        rb.volume = vol_c;
        rb.wall_id = wall_c;
        rb.scene_id = scene_c;
        rb.layout_hash = hash_c;
        rb.wall_w = wall_w_c;
        rb.wall_h = wall_h_c;
        rb.epoch_ns = snap->epoch_ns;
        rb.fps = snap->fps;
        rb.frame_index = -1;
        rb.created_ns = now_ns();
        ctx.ops.persist(opid_c);
    }
    std::thread([opid_c, rev_c, windows_json, members, token_c, vol_c, wall_c,
                 scene_c, hash_c, epoch_c, fps_c, wall_w_c, wall_h_c,
                 &ctx]() {
        fanout_layout_parallel(ctx, members, opid_c, rev_c, 0, windows_json,
                               vol_c, token_c, wall_c, scene_c, hash_c, epoch_c,
                               fps_c, -1, wall_w_c, wall_h_c, "", "", true);
    })
        .detach();
}
