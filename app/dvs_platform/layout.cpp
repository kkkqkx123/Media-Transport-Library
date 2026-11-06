#include "layout.h"

#include <algorithm>

bool layout_cell_grid(int wall_w, int wall_h, int cols, int rows,
                      int* cell_w, int* cell_h) {
    if (wall_w <= 0 || wall_h <= 0 || cols <= 0 || rows <= 0) return false;
    if (cell_w) *cell_w = wall_w / cols;
    if (cell_h) *cell_h = wall_h / rows;
    return true;
}

// Window/canvas intersection and source-crop math moved into
// main.cpp fragment_for_node() with the unified flat wire shape (see
// DVS_WINDOWS_TRANSLATION_GAPS_DESIGN.md §4); the old nested-shape helper
// was removed rather than kept as a parallel track.

uint64_t layout_apply_at_ns(uint64_t now_ns, double fps) {
    uint64_t per_frame_ns = (fps > 0.0) ? (uint64_t)(1000000000.0 / fps) : 20000000ULL;
    uint64_t guard = 2 * per_frame_ns;
    if (guard < 50000000ULL) guard = 50000000ULL;
    return now_ns + guard;
}
