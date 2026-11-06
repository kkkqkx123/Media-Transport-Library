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
