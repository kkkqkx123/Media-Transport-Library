#pragma once
#include <json-c/json.h>
#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

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
// Optional image adjustment targets carried on wall settings and layout
// payloads: {brightness,contrast,saturation,hue}, each -100..100 when
// present, at least one key required. Returns "" on success else
// "CODE:reason" (loud fail).
std::string validate_image_adjust(json_object* doc);
// Optional wall base image descriptor: {revision>=0, url non-empty,
// width/height positive when present, sub rect optional}. Returns "" on
// success else "CODE:reason".
std::string validate_base_image(json_object* doc);
// Optional binding dimensions: output port name (string) or numeric port.
// Empty means "no port dimension" — such bindings never collide on ports.
std::string binding_output_of(json_object* doc);
// Optional binding signal reference (sig/signalId). Empty when absent.
std::string binding_sig_of(json_object* doc);
// Wall canvas pixels: wall_px_w/h preferred, w/h fallback, then the
// colPixels/rowPixels sums. Pure JSON read, no store access.
void wall_px_of(json_object* wall, int* w, int* h);
// Wall output frame rate, default 60 when absent or non-positive.
double wall_fps_of(json_object* wall);
// Binding reference fields. Empty when absent.
std::string binding_wall_of(json_object* doc);
std::string binding_node_of(json_object* doc);
std::string binding_cell_of(json_object* doc);
// Whitelist address/port pair. Port defaults to 0 (caller maps to default).
void whitelist_addr_port(json_object* doc, std::string* addr, int* port);
// Playback task status value. Empty when absent.
std::string playback_status_of(json_object* doc);
// Playback status transition gate. Returns true when from -> to is legal.
bool playback_transition_ok(const std::string& from, const std::string& to);
// Whether the status counts as an active (node-occupying) playback.
bool playback_active_status(const std::string& st);
// First free (address, port) in the platform multicast pool, skipping the
// used pairs. want_port > 0 pins the port, 0 selects the default port.
// Returns false when the pool is exhausted or the port is out of range.
bool multicast_pool_next(const std::set<std::pair<std::string, int>>& used,
                         int want_port, std::string* addr_out, int* port_out);
std::string validate_playback_shape(json_object* doc);
// Prepare/rollback scene volume: 0..100 inclusive. Returns "" on success
// else "CODE:reason" (loud fail, whole order rejected).
std::string validate_volume(int volume);
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
// Intersect the global flat window set (wall pixels: id/sig/x/y/w/h/z/muted)
// against one node's canvas rect and produce the node-local flat window list:
// windowId/sig/dst/src/fullW/fullH/partial/z/muted, where dst is the
// node-local target rect and src the source crop relative to the window
// origin (window rect maps 1:1 onto the signal frame). Caller owns the
// returned array. Pure pixel math; node never rounds (platform owns rounding).
json_object* fragment_for_node(json_object* windows, const Rect& node);
// Reported per-node decode budget from a node doc capability object.
// Returns the maxDecodeSessions value when positive, -1 when unknown
// (caller must not gate on unknown budgets).
int node_decode_budget(json_object* node);
// Distinct signal count inside one member canvas (platform fragment math).
int count_node_signals(json_object* windows, const Rect& node);
// Fixed-mode snap: clamp every window into the single cell containing its
// center (dominant cell), in wall pixels. Windows with non-positive size
// are left untouched for the caller-side validator to reject.
// Caller owns the returned array (same length/order as the input).
json_object* snap_windows_to_cells(json_object* windows,
                                    const std::vector<std::pair<std::string, Rect>>& cells);
// Minimum window side for a wall: the largest of the 64px floor and one
// quarter of the smallest cell side (1/16 of a cell by area). Falls back to
// 64 when the wall carries no usable cell table.
int wall_min_window_side(json_object* wall, int wall_w, int wall_h);
// Wall cell table for fixed-mode snap: {cellId, rect} in wall pixels.
// Prefers the stored cell_grid array (entries {id?,r,c,x,y,w,h}), else falls
// back to a uniform rows x cols split of the wall canvas. Empty when the
// wall carries no usable geometry.
void wall_cells(json_object* wall, int wall_w, int wall_h,
                std::vector<std::pair<std::string, Rect>>* out);
// Per-node window budget: at most this many windows may land on one member
// canvas (mirrors MP4_PIP_V1_NV12_LANES without pulling the rk3588 dep).
// The wall-global window count is intentionally unbounded: a 16-window wall
// spread over 16 nodes needs one plane per node, well within budget.
int max_windows_per_node();
// Per-node budget check over the wall-global window list: every listed member
// canvas must intersect at most max_windows_per_node() windows (same fragment
// math the fanout uses). Caller passes geometry-known members only; unknown
// geometry stays missing-report at fanout. Returns "" on success else
// "CODE:reason" naming the first over-budget node.
std::string validate_per_node_budget(
    json_object* windows,
    const std::vector<std::pair<std::string, Rect>>& members);
// Clock gate: max tolerable PTP offset in microseconds (env override).
int ptp_max_offset_us();
// Node qualification for fanout: online, role output, clock synced within
// threshold. Missing clock summary means unknown and unqualified.
bool node_clock_ok(json_object* node, std::string* reason);
// Barrier ack lane readiness: every present lane must be ready. A missing
// ready flag counts as ready; a missing or empty array counts as ready.
bool barrier_lanes_ready(json_object* lanes);
// Barrier ack frame self-consistency: lanes carrying a frame sequence must
// agree within one ack. No sequence fields means unknown and passes.
bool barrier_lanes_frame_consistent(json_object* lanes, int64_t* seq_out);
// Fanout sharding for large walls (env override, clamped to a sane range).
int fanout_parallelism();
// Largest accepted wall membership (env override, at least one).
int max_wall_members();
