#include <cassert>
#include <cstdio>
#include <set>

#include "layout.h"
#include "ops.h"
#include "platform_store.h"

int main() {
    // layout: cell grid
    {
        int cw = 0, ch = 0;
        assert(layout_cell_grid(3840, 2160, 2, 2, &cw, &ch));
        assert(cw == 1920 && ch == 1080);
        assert(!layout_cell_grid(0, 2160, 2, 2, &cw, &ch));
    }
    // layout: window fragment split across two nodes
    {
        Rect win{1000, 500, 2000, 1000};
        Rect left{0, 0, 1920, 1080};
        Rect dst, src;
        assert(layout_window_fragment(win, left, 1920, 1080, &dst, &src));
        assert(dst.x == 1000 && dst.y == 500 && dst.w == 920 && dst.h == 580);
        Rect right{1920, 0, 1920, 1080};
        assert(layout_window_fragment(win, right, 1920, 1080, &dst, &src));
        assert(dst.x == 0 && dst.w == 1080);
        Rect far{4000, 0, 1920, 1080};
        assert(!layout_window_fragment(win, far, 1920, 1080, &dst, &src));
    }
    // layout: apply_at guard band
    {
        uint64_t at = layout_apply_at_ns(1000000000ULL, 60.0);
        assert(at >= 1000000000ULL + 50000000ULL);
    }
    // ops: barrier + rollback
    {
        OpCoordinator c;
        c.begin("op-1", 7, {"n1", "n2"}, 12345);
        assert(c.on_reply("op-1", "n1", true, "ready") == "preparing");
        assert(!c.missing("op-1").empty());
        assert(c.on_reply("op-1", "n2", true, "ready") == "committed");
        assert(c.missing("op-1").empty());
    }
    {
        OpCoordinator c;
        c.begin("op-2", 8, {"n1", "n2"}, 0);
        assert(c.on_reply("op-2", "n1", false, "blocked") == "blocked");
        int rev = 0;
        std::string rb = c.rollback_op("op-2", &rev);
        assert(!rb.empty() && rev == 7);
        assert(c.find("op-2")->state == "rolledback");
    }
    // store: docs + tombstone
    {
        PlatformStore s(":memory:");
        json_object* d = json_object_new_object();
        json_object_object_add(d, "id", json_object_new_string("w1"));
        assert(s.put_doc("walls", "w1", d));
        json_object_put(d);
        json_object* g = s.get_doc("walls", "w1");
        assert(g);
        json_object_put(g);
        assert(s.list_ids("walls").size() == 1);
        assert(!s.tombstone_check("n9", 1000));
        s.tombstone_add("n9", 1000);
        assert(s.tombstone_check("n9", 1000));
        s.tombstone_gc(1000 + 31LL * 24 * 3600);
        assert(!s.tombstone_check("n9", 1000 + 31LL * 24 * 3600));
    }
    printf("dvs_platform_basic OK\n");
    return 0;
}
