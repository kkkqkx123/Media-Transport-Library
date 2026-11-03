#include "layout.h"

#include <algorithm>

bool layout_cell_grid(int wall_w, int wall_h, int cols, int rows,
                      int* cell_w, int* cell_h) {
    if (wall_w <= 0 || wall_h <= 0 || cols <= 0 || rows <= 0) return false;
    if (cell_w) *cell_w = wall_w / cols;
    if (cell_h) *cell_h = wall_h / rows;
    return true;
}

bool layout_window_fragment(const Rect& win, const Rect& node,
                            int src_w, int src_h, Rect* dst, Rect* src) {
    int ix0 = std::max(win.x, node.x);
    int iy0 = std::max(win.y, node.y);
    int ix1 = std::min(win.x + win.w, node.x + node.w);
    int iy1 = std::min(win.y + win.h, node.y + node.h);
    if (ix1 <= ix0 || iy1 <= iy0) return false;
    if (dst) {
        dst->x = ix0 - node.x;
        dst->y = iy0 - node.y;
        dst->w = ix1 - ix0;
        dst->h = iy1 - iy0;
    }
    if (src && win.w > 0 && win.h > 0 && src_w > 0 && src_h > 0) {
        // Map the visible slice back into source pixels, rounding outward.
        double sx = (double)src_w / (double)win.w;
        double sy = (double)src_h / (double)win.h;
        src->x = (int)((ix0 - win.x) * sx); // floor via truncation (non-negative)
        src->y = (int)((iy0 - win.y) * sy);
        int x1 = (int)((ix1 - win.x) * sx + 0.999999);
        int y1 = (int)((iy1 - win.y) * sy + 0.999999);
        src->w = x1 - src->x;
        src->h = y1 - src->y;
        if (src->x < 0) src->x = 0;
        if (src->y < 0) src->y = 0;
        if (src->x + src->w > src_w) src->w = src_w - src->x;
        if (src->y + src->h > src_h) src->h = src_h - src->y;
    }
    return true;
}

uint64_t layout_apply_at_ns(uint64_t now_ns, double fps) {
    uint64_t per_frame_ns = (fps > 0.0) ? (uint64_t)(1000000000.0 / fps) : 20000000ULL;
    uint64_t guard = 2 * per_frame_ns;
    if (guard < 50000000ULL) guard = 50000000ULL;
    return now_ns + guard;
}
