#include "layout.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>

#include "sha256.h"

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

namespace {
std::string jstr(json_object* o, const char* k) {
    json_object* v = nullptr;
    if (o && json_object_object_get_ex(o, k, &v) && v &&
        json_object_is_type(v, json_type_string))
        return json_object_get_string(v);
    return "";
}
int jint(json_object* o, const char* k, int dflt = 0) {
    json_object* v = nullptr;
    if (o && json_object_object_get_ex(o, k, &v) && v &&
        (json_object_is_type(v, json_type_int)))
        return json_object_get_int(v);
    return dflt;
}
bool jbool(json_object* o, const char* k, bool dflt = false) {
    json_object* v = nullptr;
    if (o && json_object_object_get_ex(o, k, &v) && v &&
        json_object_is_type(v, json_type_boolean))
        return json_object_get_boolean(v) != 0;
    return dflt;
}
int rect_int(json_object* o, const char* k) {
    json_object* v = nullptr;
    if (o && json_object_object_get_ex(o, k, &v) && v)
        return json_object_get_int(v);
    return 0;
}
}  // namespace

std::string docs_trim(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && isspace((unsigned char)s[a])) a++;
    size_t b = s.size();
    while (b > a && isspace((unsigned char)s[b - 1])) b--;
    return s.substr(a, b - a);
}
std::string docs_lower(const std::string& s) {
    std::string o = s;
    for (auto& c : o) c = (char)tolower((unsigned char)c);
    return o;
}

std::string validate_group_shape(json_object* doc) {
    if (!doc || !json_object_is_type(doc, json_type_object))
        return "INVALID_ARGUMENT:group must be object";
    if (docs_trim(jstr(doc, "name")).empty())
        return "INVALID_ARGUMENT:group name required";
    return "";
}

std::string validate_wall_shape(json_object* doc) {
    if (!doc || !json_object_is_type(doc, json_type_object))
        return "INVALID_ARGUMENT:wall must be object";
    if (docs_trim(jstr(doc, "name")).empty())
        return "INVALID_ARGUMENT:wall name required";
    json_object* v = nullptr;
    if (json_object_object_get_ex(doc, "rows", &v) && v) {
        int r = json_object_get_int(v);
        if (r < 1 || r > 6) return "INVALID_ARGUMENT:wall rows must be 1..6";
    }
    if (json_object_object_get_ex(doc, "cols", &v) && v) {
        int c = json_object_get_int(v);
        if (c < 1 || c > 15) return "INVALID_ARGUMENT:wall cols must be 1..15";
    }
    if (json_object_object_get_ex(doc, "mode", &v) && v &&
        json_object_is_type(v, json_type_string)) {
        std::string m = jstr(doc, "mode");
        if (m != "LCD" && m != "LED")
            return "INVALID_ARGUMENT:wall mode must be LCD|LED";
    }
    if (json_object_object_get_ex(doc, "type", &v) && v &&
        json_object_is_type(v, json_type_string)) {
        std::string t = jstr(doc, "type");
        if (!t.empty() && t != "standard" && t != "nonstandard" &&
            t != "STD" && t != "STD+") {
            // Tolerant: frontend prototype uses display labels; only reject
            // obviously malformed values by length, never by dialect.
            if (t.size() > 32) return "INVALID_ARGUMENT:wall type too long";
        }
    }
    // LED per-row/column pixels share the 64..7680 floor.
    int rows = jint(doc, "rows", 0), cols = jint(doc, "cols", 0);
    json_object* rp = nullptr;
    if (json_object_object_get_ex(doc, "rowPixels", &rp) && rp) {
        if (!json_object_is_type(rp, json_type_array))
            return "INVALID_ARGUMENT:wall rowPixels must be array";
        if (rows > 0 && (int)json_object_array_length(rp) != rows)
            return "INVALID_ARGUMENT:wall rowPixels length must match rows";
        for (size_t i = 0; i < json_object_array_length(rp); i++) {
            int px = json_object_get_int(json_object_array_get_idx(rp, i));
            if (px < 64 || px > 7680)
                return "INVALID_ARGUMENT:wall rowPixels must be 64..7680";
        }
    }
    json_object* cp = nullptr;
    if (json_object_object_get_ex(doc, "colPixels", &cp) && cp) {
        if (!json_object_is_type(cp, json_type_array))
            return "INVALID_ARGUMENT:wall colPixels must be array";
        if (cols > 0 && (int)json_object_array_length(cp) != cols)
            return "INVALID_ARGUMENT:wall colPixels length must match cols";
        for (size_t i = 0; i < json_object_array_length(cp); i++) {
            int px = json_object_get_int(json_object_array_get_idx(cp, i));
            if (px < 64 || px > 7680)
                return "INVALID_ARGUMENT:wall colPixels must be 64..7680";
        }
    }
    json_object* cg = nullptr;
    if (json_object_object_get_ex(doc, "cell_grid", &cg) && cg) {
        if (!cg) return "INVALID_ARGUMENT:wall cell_grid must be array";
        if (!json_object_is_type(cg, json_type_array) &&
            !json_object_is_type(cg, json_type_object))
            return "INVALID_ARGUMENT:wall cell_grid must be array";
    }
    return "";
}

std::string validate_scene_shape(json_object* doc, int wall_w, int wall_h) {
    if (!doc || !json_object_is_type(doc, json_type_object))
        return "INVALID_ARGUMENT:scene must be object";
    json_object* v = nullptr;
    if (json_object_object_get_ex(doc, "snapshotVersion", &v) && v) {
        if (!json_object_is_type(v, json_type_int) || json_object_get_int(v) != 1)
            return "INVALID_ARGUMENT:scene snapshotVersion must be 1";
    }
    if (json_object_object_get_ex(doc, "volume", &v) && v) {
        if (!json_object_is_type(v, json_type_int))
            return "INVALID_ARGUMENT:scene volume must be int";
        int vol = json_object_get_int(v);
        if (vol < 0 || vol > 100)
            return "INVALID_ARGUMENT:scene volume must be 0..100";
    }
    if (json_object_object_get_ex(doc, "layoutMode", &v) && v &&
        json_object_is_type(v, json_type_string)) {
        std::string m = json_object_get_string(v);
        if (m != "free" && m != "fixed")
            return "INVALID_ARGUMENT:scene layoutMode must be free|fixed";
    }
    json_object* wins = nullptr;
    if (json_object_object_get_ex(doc, "windows", &v) && v) wins = v;
    if (json_object_object_get_ex(doc, "layout", &v) && v && !wins) wins = v;
    if (!wins) return "";
    if (!json_object_is_type(wins, json_type_array))
        return "INVALID_ARGUMENT:scene windows must be array";
    if ((int)json_object_array_length(wins) > 4)
        return "INVALID_ARGUMENT:too many windows for node channel cap";
    for (size_t i = 0; i < json_object_array_length(wins); i++) {
        json_object* w = json_object_array_get_idx(wins, i);
        if (!w || !json_object_is_type(w, json_type_object))
            return "INVALID_ARGUMENT:scene window must be object";
        std::string id = jstr(w, "id");
        if (id.empty()) id = jstr(w, "windowId");
        std::string sig = jstr(w, "sig");
        if (sig.empty()) sig = jstr(w, "signalId");
        if (id.empty()) return "INVALID_ARGUMENT:scene window missing id";
        if (sig.empty()) return "INVALID_ARGUMENT:scene window missing signal";
        int x = rect_int(w, "x"), y = rect_int(w, "y");
        int ww = rect_int(w, "w"), hh = rect_int(w, "h");
        if (ww < 64 || hh < 64)
            return "INVALID_ARGUMENT:window below minimum size";
        if (wall_w > 0 && wall_h > 0 &&
            (x < 0 || y < 0 || x + ww > wall_w || y + hh > wall_h))
            return "INVALID_ARGUMENT:window outside wall canvas";
    }
    return "";
}

std::string binding_output_of(json_object* doc) {
    if (!doc) return "";
    json_object* v = nullptr;
    if (json_object_object_get_ex(doc, "output", &v) && v) {
        if (json_object_is_type(v, json_type_string)) return jstr(doc, "output");
        if (json_object_is_type(v, json_type_int)) {
            int p = json_object_get_int(v);
            if (p > 0) return std::to_string(p);
            return "";
        }
    }
    if (json_object_object_get_ex(doc, "outputPort", &v) && v) {
        if (json_object_is_type(v, json_type_string)) return jstr(doc, "outputPort");
        if (json_object_is_type(v, json_type_int)) {
            int p = json_object_get_int(v);
            if (p > 0) return std::to_string(p);
            return "";
        }
    }
    if (json_object_object_get_ex(doc, "port", &v) && v &&
        json_object_is_type(v, json_type_int)) {
        int p = json_object_get_int(v);
        if (p > 0) return std::to_string(p);
    }
    return "";
}

std::string binding_sig_of(json_object* doc) {
    if (!doc) return "";
    std::string s = jstr(doc, "sig");
    if (s.empty()) s = jstr(doc, "signalId");
    return s;
}

std::string validate_binding_shape(json_object* doc) {
    if (!doc || !json_object_is_type(doc, json_type_object))
        return "INVALID_ARGUMENT:binding must be object";
    std::string wall = jstr(doc, "wallId");
    if (wall.empty()) wall = jstr(doc, "wall");
    std::string node = jstr(doc, "nodeId");
    if (node.empty()) node = jstr(doc, "node");
    if (node.empty()) node = jstr(doc, "node_id");
    if (wall.empty()) return "INVALID_ARGUMENT:binding missing wallId";
    if (node.empty()) return "INVALID_ARGUMENT:binding missing nodeId";
    // Optional output dimension: string port name or numeric port 1..65535.
    // A binding carrying both spellings must agree; disagreement rejects.
    std::string out_s;
    bool has_out_s = false;
    int out_p = 0;
    bool has_out_p = false;
    json_object* v = nullptr;
    if (json_object_object_get_ex(doc, "output", &v) && v) {
        if (json_object_is_type(v, json_type_string)) {
            out_s = json_object_get_string(v);
            has_out_s = true;
        } else if (json_object_is_type(v, json_type_int)) {
            out_p = json_object_get_int(v);
            has_out_p = true;
        } else {
            return "INVALID_ARGUMENT:binding output must be string or int";
        }
    }
    if (json_object_object_get_ex(doc, "outputPort", &v) && v) {
        if (json_object_is_type(v, json_type_string)) {
            std::string s = json_object_get_string(v);
            if (has_out_s && s != out_s)
                return "INVALID_ARGUMENT:binding output/outputPort disagree";
            out_s = s;
            has_out_s = true;
        } else if (json_object_is_type(v, json_type_int)) {
            int p = json_object_get_int(v);
            if (has_out_p && p != out_p)
                return "INVALID_ARGUMENT:binding output/outputPort disagree";
            out_p = p;
            has_out_p = true;
        } else {
            return "INVALID_ARGUMENT:binding outputPort must be string or int";
        }
    }
    if (json_object_object_get_ex(doc, "port", &v) && v) {
        if (!json_object_is_type(v, json_type_int))
            return "INVALID_ARGUMENT:binding port must be int";
        int p = json_object_get_int(v);
        if (has_out_p && p != out_p)
            return "INVALID_ARGUMENT:binding port/outputPort disagree";
        out_p = p;
        has_out_p = true;
    }
    // String port name and numeric port are two spellings of the same
    // dimension: carrying both rejects so the idle check stays unambiguous.
    if (has_out_s && has_out_p)
        return "INVALID_ARGUMENT:binding output/port disagree";
    if (has_out_s && (out_s.empty() || out_s.size() > 64))
        return "INVALID_ARGUMENT:binding output must be 1..64 chars";
    if (has_out_p && (out_p < 1 || out_p > 65535))
        return "INVALID_ARGUMENT:binding port must be 1..65535";
    // Optional signal reference: validated for existence by the caller.
    std::string sig = binding_sig_of(doc);
    if (!sig.empty() && sig.size() > 128)
        return "INVALID_ARGUMENT:binding sig too long";
    return "";
}

std::string validate_whitelist_shape(json_object* doc) {
    if (!doc || !json_object_is_type(doc, json_type_object))
        return "INVALID_ARGUMENT:whitelist must be object";
    std::string sig = jstr(doc, "sig");
    if (sig.empty()) sig = jstr(doc, "id");
    if (sig.empty()) return "INVALID_ARGUMENT:whitelist missing sig";
    std::string addr = jstr(doc, "address");
    if (addr.empty()) addr = jstr(doc, "mcast_ip");
    if (addr.empty()) return "INVALID_ARGUMENT:whitelist missing address";
    int port = 0;
    json_object* v = nullptr;
    if (json_object_object_get_ex(doc, "port", &v) && v) port = json_object_get_int(v);
    if (json_object_object_get_ex(doc, "mcast_port", &v) && v)
        port = json_object_get_int(v);
    if (port != 0 && (port < 1 || port > 65535))
        return "INVALID_ARGUMENT:whitelist port must be 1..65535";
    if (json_object_object_get_ex(doc, "codec", &v) && v &&
        json_object_is_type(v, json_type_string)) {
        std::string c = docs_lower(json_object_get_string(v));
        if (c != "h264" && c != "hevc" && c != "jpegxs" && c != "raw")
            return "INVALID_ARGUMENT:whitelist codec must be h264|hevc|jpegxs|raw";
    }
    for (const char* k : {"width", "height"}) {
        json_object* vv = nullptr;
        if (json_object_object_get_ex(doc, k, &vv) && vv) {
            if (!json_object_is_type(vv, json_type_int) || json_object_get_int(vv) <= 0)
                return std::string("INVALID_ARGUMENT:whitelist ") + k + " must be positive";
        }
    }
    // Encoding ceiling: optional fields are range-checked when present so an
    // impossible encode fails at save time instead of at first TX start.
    json_object* fpsv = nullptr;
    if (json_object_object_get_ex(doc, "fps", &fpsv) && fpsv) {
        double f = 0;
        if (json_object_is_type(fpsv, json_type_int))
            f = (double)json_object_get_int(fpsv);
        else if (json_object_is_type(fpsv, json_type_double))
            f = json_object_get_double(fpsv);
        else
            return "INVALID_ARGUMENT:whitelist fps must be numeric";
        if (f < 1.0 || f > 120.0)
            return "INVALID_ARGUMENT:whitelist fps must be 1..120";
    }
    for (const char* k : {"bitrateKbps", "bitrate"}) {
        json_object* bv = nullptr;
        if (json_object_object_get_ex(doc, k, &bv) && bv) {
            if (!json_object_is_type(bv, json_type_int) ||
                json_object_get_int(bv) <= 0 || json_object_get_int(bv) > 100000)
                return std::string("INVALID_ARGUMENT:whitelist ") + k +
                       " must be 1..100000";
        }
    }
    for (const char* k : {"keyFrameInterval", "gop"}) {
        json_object* gv = nullptr;
        if (json_object_object_get_ex(doc, k, &gv) && gv) {
            if (!json_object_is_type(gv, json_type_int) ||
                json_object_get_int(gv) <= 0 || json_object_get_int(gv) > 600)
                return std::string("INVALID_ARGUMENT:whitelist ") + k +
                       " must be 1..600";
        }
    }
    return "";
}

bool multicast_pool_next(const std::set<std::pair<std::string, int>>& used,
                         int want_port, std::string* addr_out, int* port_out) {
    int port = (want_port > 0) ? want_port : 50010;
    if (port < 1 || port > 65535) return false;
    // Platform pool: 239.100.0.1/24 at the selected port. Addresses are
    // scanned in ascending order so allocation is deterministic.
    for (int lo = 1; lo <= 254; lo++) {
        char a[32];
        snprintf(a, sizeof(a), "239.100.0.%d", lo);
        if (used.count({a, port}) == 0) {
            if (addr_out) *addr_out = a;
            if (port_out) *port_out = port;
            return true;
        }
    }
    return false;
}

std::string validate_playback_shape(json_object* doc) {
    if (!doc || !json_object_is_type(doc, json_type_object))
        return "INVALID_ARGUMENT:playback-task must be object";
    std::string wall = jstr(doc, "wallId");
    if (wall.empty()) wall = jstr(doc, "wall");
    std::string scene = jstr(doc, "sceneId");
    if (scene.empty()) scene = jstr(doc, "scene");
    std::string node = jstr(doc, "targetNodeId");
    if (node.empty()) node = jstr(doc, "nodeId");
    if (node.empty()) node = jstr(doc, "node");
    if (wall.empty()) return "INVALID_ARGUMENT:playback-task missing wallId";
    if (scene.empty()) return "INVALID_ARGUMENT:playback-task missing sceneId";
    if (node.empty()) return "INVALID_ARGUMENT:playback-task missing targetNodeId";
    std::string st = jstr(doc, "status");
    if (!st.empty()) {
        static const char* kStates[] = {"pending_check", "pending_execution",
                                        "executing", "playing", "succeeded",
                                        "failed", "cancelled", "expired"};
        bool ok = false;
        for (auto s : kStates)
            if (st == s) ok = true;
        if (!ok) return "INVALID_ARGUMENT:playback-task bad status";
    }
    return "";
}

std::string layout_windows_hash(json_object* windows) {
    const char* s = "[]";
    std::string owned;
    if (windows && json_object_is_type(windows, json_type_array)) {
        const char* js = json_object_to_json_string(windows);
        if (js) s = js;
    } else if (windows) {
        const char* js = json_object_to_json_string(windows);
        owned = js ? js : "{}";
        s = owned.c_str();
    }
    uint8_t out[32];
    dvs::sha256((const uint8_t*)s, __builtin_strlen(s), out);
    return dvs::to_hex(out, 32);
}

json_object* layout_dirty_compare(json_object* saved_windows, int saved_volume,
                                  json_object* live_windows, int live_volume) {
    json_object* out = json_object_new_object();
    json_object* diff = json_object_new_array();
    bool dirty = false;
    if (saved_volume != live_volume) {
        dirty = true;
        json_object_array_add(diff, json_object_new_string("volume"));
    }
    int sn = (saved_windows && json_object_is_type(saved_windows, json_type_array))
                 ? (int)json_object_array_length(saved_windows)
                 : 0;
    int ln = (live_windows && json_object_is_type(live_windows, json_type_array))
                 ? (int)json_object_array_length(live_windows)
                 : 0;
    if (sn != ln) {
        dirty = true;
        json_object_array_add(diff, json_object_new_string("windows.count"));
    }
    int n = std::min(sn, ln);
    for (int i = 0; i < n; i++) {
        json_object* a = json_object_array_get_idx(saved_windows, i);
        json_object* b = json_object_array_get_idx(live_windows, i);
        const char* sa = a ? json_object_to_json_string(a) : "";
        const char* sb = b ? json_object_to_json_string(b) : "";
        std::string x = sa ? sa : "", y = sb ? sb : "";
        if (x != y) {
            dirty = true;
            json_object_array_add(diff, json_object_new_string(
                                          ("windows[" + std::to_string(i) + "]").c_str()));
        }
    }
    if (sn != ln) {
        // Length mismatch already recorded; per-index walk above covers the
        // shared prefix so no further detail is needed.
    }
    json_object_object_add(out, "dirty", json_object_new_boolean(dirty ? 1 : 0));
    json_object_object_add(out, "diff", diff);
    json_object_object_add(
        out, "savedHash",
        json_object_new_string(layout_windows_hash(saved_windows).c_str()));
    json_object_object_add(
        out, "liveHash",
        json_object_new_string(layout_windows_hash(live_windows).c_str()));
    return out;
}

bool wall_is_deleted(json_object* wall) {
    if (!wall) return false;
    if (jbool(wall, "deleted", false)) return true;
    json_object* v = nullptr;
    if (json_object_object_get_ex(wall, "deletedAt", &v) && v) {
        if (json_object_is_type(v, json_type_string))
            return jstr(wall, "deletedAt").size() > 0;
        if (json_object_is_type(v, json_type_int))
            return json_object_get_int64(v) != 0;
    }
    return false;
}

bool base_subrect(int wall_w, int wall_h, const Rect& node, Rect* out) {
    if (wall_w <= 0 || wall_h <= 0) return false;
    if (node.w <= 0 || node.h <= 0) return false;
    int ix1 = std::max(0, node.x), iy1 = std::max(0, node.y);
    int ix2 = std::min(wall_w, node.x + node.w);
    int iy2 = std::min(wall_h, node.y + node.h);
    if (ix2 <= ix1 || iy2 <= iy1) return false;
    if (out) {
        out->x = ix1;
        out->y = iy1;
        out->w = ix2 - ix1;
        out->h = iy2 - iy1;
    }
    return true;
}

int ptp_max_offset_us() {
    const char* v = std::getenv("DVS_PTP_MAX_OFFSET_US");
    if (!v || !*v) return 2000;
    int n = atoi(v);
    return n > 0 ? n : 2000;
}

bool node_clock_ok(json_object* node, std::string* reason) {
    auto fail = [&](const std::string& r) {
        if (reason) *reason = r;
        return false;
    };
    if (!node) return fail("node unknown");
    json_object* v = nullptr;
    bool online = false;
    if (json_object_object_get_ex(node, "online", &v) && v)
        online = json_object_get_boolean(v) != 0;
    if (!online) return fail("node offline");
    std::string role = jstr(node, "role");
    if (!role.empty() && role != "output") return fail("role is not output");
    json_object* ptp = nullptr;
    if (json_object_object_get_ex(node, "ptp", &ptp) && !ptp)
        json_object_object_get_ex(node, "clock", &ptp);
    if (!json_object_object_get_ex(node, "ptp", &ptp) || !ptp)
        json_object_object_get_ex(node, "clock", &ptp);
    // Heartbeat path stores the digest under metrics.ptp for additive
    // compatibility; accept either location.
    if (!ptp) {
        json_object* m = nullptr;
        if (json_object_object_get_ex(node, "metrics", &m) && m)
            json_object_object_get_ex(m, "ptp", &ptp);
    }
    if (!ptp || !json_object_is_type(ptp, json_type_object))
        return fail("clock unknown");
    std::string st = jstr(ptp, "state");
    if (st != "synced") return fail("clock unsynced");
    json_object* off = nullptr;
    if (json_object_object_get_ex(ptp, "offsetUs", &off) && off) {
        long o = labs(json_object_get_int64(off));
        if (o > ptp_max_offset_us()) return fail("clock offset exceeded");
    }
    return true;
}
