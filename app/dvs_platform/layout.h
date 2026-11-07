#pragma once
#include <json-c/json.h>
#include <cstdint>
#include <string>

// Pixel layout math. The platform owns all rounding: node never rounds.
// Convention: left/top edges round down, right/bottom edges round up.
struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
};

// Derive per-cell grid size from wall pixels and column/row counts.
bool layout_cell_grid(int wall_w, int wall_h, int cols, int rows,
                      int* cell_w, int* cell_h);

// Target commit time: now plus a guard band covering two frame periods at the
// given fps with a 50ms floor. All times are TAI nanoseconds.
uint64_t layout_apply_at_ns(uint64_t now_ns, double fps);

// Business-rule helpers shared by the HTTP layer and unit tests. Pure JSON
// logic, no store access: callers do the uniqueness/reference scans.
// Validators return "" on success else "CODE:reason" (loud fail).
std::string docs_trim(const std::string& s);
std::string docs_lower(const std::string& s);
std::string validate_group_shape(json_object* doc);
std::string validate_wall_shape(json_object* doc);
std::string validate_scene_shape(json_object* doc, int wall_w, int wall_h);
std::string validate_binding_shape(json_object* doc);
std::string validate_whitelist_shape(json_object* doc);
std::string validate_playback_shape(json_object* doc);
// Canonical hash of the windows array for idempotency and receipt matching.
std::string layout_windows_hash(json_object* windows);
// Read-only dirty compare: {dirty, diff[], savedHash, liveHash}.
// Caller owns the returned object.
json_object* layout_dirty_compare(json_object* saved_windows, int saved_volume,
                                  json_object* live_windows, int live_volume);
// Soft-delete marker on wall docs (deleted flag or deletedAt timestamp).
bool wall_is_deleted(json_object* wall);
// Sub-image descriptor rect: intersection of the wall canvas with the node
// canvas. Returns false when there is no overlap or inputs are degenerate.
bool base_subrect(int wall_w, int wall_h, const Rect& node, Rect* out);
// Clock gate: max tolerable PTP offset in microseconds (env override).
int ptp_max_offset_us();
// Node qualification for fanout: online, role output, clock synced within
// threshold. Missing clock summary means unknown and unqualified.
bool node_clock_ok(json_object* node, std::string* reason);
