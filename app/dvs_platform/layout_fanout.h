#pragma once
#include <json-c/json.h>

#include <set>
#include <string>

struct PlatformContext;

// Outcome of one per-node layout delivery attempt.
struct LayoutDelivery {
    bool sent = false;  // payload handed to a transport (WS or HTTP)
    bool via_ws = false;
    int http_status = 0;  // meaningful when sent && !via_ws
    bool skipped = false;
    std::string skip_reason;  // "no node doc" / "no geometry" / "no channel"
};

// Deliver one per-node layout.prepare payload: WS first, short HTTP to the
// agent base as fallback. Shared by the prepare fanout and the rollback
// reissue, which previously duplicated this loop inline.
LayoutDelivery deliver_layout_prepare(
    PlatformContext& ctx, const std::string& node_id, const std::string& opid,
    int revision, int64_t apply_at_ns, json_object* wins, int volume,
    const std::string& token, const std::string& wall_id = "",
    const std::string& scene_id = "", const std::string& layout_hash = "",
    int64_t epoch_ns = 0, double fps = 60.0, int64_t frame_index = -1,
    int wall_w = 0, int wall_h = 0, const std::string& base_json = "",
    const std::string& adjust_json = "");

// Sharded fanout shared by prepare and rollback: each worker parses its own
// snapshot copy and delivers a strided subset so large walls stay within the
// barrier timeout.
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
                            const std::string& adjust_json, bool is_rollback);

// Re-issue the settled snapshot for to_revision to every member with the
// rollback op id and applyAtNs=0 (immediate apply). No snapshot means audit
// only, never faked as executed. Must not be called with ops_mu held.
void execute_rollback(PlatformContext& ctx, const std::string& rollback_op_id,
                      int to_revision, const std::set<std::string>& members);
