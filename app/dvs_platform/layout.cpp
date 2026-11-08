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
bool valid_ip4_addr(const std::string& s) {
    if (s.empty() || s.size() > 15) return false;
    int parts = 0;
    const char* p = s.c_str();
    while (true) {
        if (*p < '0' || *p > '9') return false;
        int v = 0, digits = 0;
        while (*p >= '0' && *p <= '9') {
            v = v * 10 + (*p - '0');
            digits++;
            p++;
            if (digits > 3) return false;
        }
        if (v > 255) return false;
        parts++;
        if (*p == '\0') break;
        if (*p != '.') return false;
        p++;
        if (*p == '\0') return false;
    }
    return parts == 4;
}
}  // namespace

json_object* fragment_for_node(json_object* windows, const Rect& node) {
    json_object* out = json_object_new_array();
    if (!windows || !json_object_is_type(windows, json_type_array)) return out;
    int n = json_object_array_length(windows);
    for (int i = 0; i < n; i++) {
        json_object* w = json_object_array_get_idx(windows, i);
        if (!w || !json_object_is_type(w, json_type_object)) continue;
        int gx = rect_int(w, "x"), gy = rect_int(w, "y");
        int gw = rect_int(w, "w"), gh = rect_int(w, "h");
        if (gw <= 0 || gh <= 0) continue;
        int ix1 = std::max(gx, node.x), iy1 = std::max(gy, node.y);
        int ix2 = std::min(gx + gw, node.x + node.w);
        int iy2 = std::min(gy + gh, node.y + node.h);
        if (ix2 <= ix1 || iy2 <= iy1) continue;
        json_object* nw = json_object_new_object();
        json_object_object_add(nw, "windowId",
                               json_object_new_string(jstr(w, "id").c_str()));
        json_object_object_add(nw, "sig",
                               json_object_new_string(jstr(w, "sig").c_str()));
        json_object* nd = json_object_new_object();
        json_object_object_add(nd, "x", json_object_new_int(ix1 - node.x));
        json_object_object_add(nd, "y", json_object_new_int(iy1 - node.y));
        json_object_object_add(nd, "w", json_object_new_int(ix2 - ix1));
        json_object_object_add(nd, "h", json_object_new_int(iy2 - iy1));
        json_object* ns = json_object_new_object();
        json_object_object_add(ns, "x", json_object_new_int(ix1 - gx));
        json_object_object_add(ns, "y", json_object_new_int(iy1 - gy));
        json_object_object_add(ns, "w", json_object_new_int(ix2 - ix1));
        json_object_object_add(ns, "h", json_object_new_int(iy2 - iy1));
        json_object_object_add(nw, "dst", nd);
        json_object_object_add(nw, "src", ns);
        json_object_object_add(nw, "fullW", json_object_new_int(gw));
        json_object_object_add(nw, "fullH", json_object_new_int(gh));
        bool partial = (ix1 != gx || iy1 != gy || (ix2 - ix1) != gw ||
                        (iy2 - iy1) != gh);
        json_object_object_add(nw, "partial",
                               json_object_new_boolean(partial ? 1 : 0));
        json_object* zv = nullptr;
        if (json_object_object_get_ex(w, "z", &zv) && zv)
            json_object_object_add(nw, "z", json_object_get(zv));
        json_object* mv = nullptr;
        if (json_object_object_get_ex(w, "muted", &mv) && mv &&
            json_object_is_type(mv, json_type_boolean))
            json_object_object_add(nw, "muted", json_object_get(mv));
        json_object_array_add(out, nw);
    }
    return out;
}

int max_windows_per_node() { return 4; }

std::string validate_per_node_budget(
    json_object* windows,
    const std::vector<std::pair<std::string, Rect>>& members) {
    for (const auto& kv : members) {
        json_object* frag = fragment_for_node(windows, kv.second);
        int n = frag ? (int)json_object_array_length(frag) : 0;
        if (frag) json_object_put(frag);
        if (n > max_windows_per_node())
            return "INVALID_ARGUMENT:node " + kv.first +
                   " exceeds per-node window budget";
    }
    return "";
}

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
    if (!wins) return "";
    if (!json_object_is_type(wins, json_type_array))
        return "INVALID_ARGUMENT:scene windows must be array";
    for (size_t i = 0; i < json_object_array_length(wins); i++) {
        json_object* w = json_object_array_get_idx(wins, i);
        if (!w || !json_object_is_type(w, json_type_object))
            return "INVALID_ARGUMENT:scene window must be object";
        std::string id = jstr(w, "id");
        std::string sig = jstr(w, "sig");
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
    if (json_object_object_get_ex(doc, "outputPort", &v) && v) {
        if (json_object_is_type(v, json_type_string)) return jstr(doc, "outputPort");
        if (json_object_is_type(v, json_type_int)) {
            int p = json_object_get_int(v);
            if (p > 0) return std::to_string(p);
            return "";
        }
    }
    return "";
}

std::string binding_sig_of(json_object* doc) {
    if (!doc) return "";
    return jstr(doc, "sig");
}

std::string validate_binding_shape(json_object* doc) {
    if (!doc || !json_object_is_type(doc, json_type_object))
        return "INVALID_ARGUMENT:binding must be object";
    std::string wall = jstr(doc, "wallId");
    std::string node = jstr(doc, "nodeId");
    if (wall.empty()) return "INVALID_ARGUMENT:binding missing wallId";
    if (node.empty()) return "INVALID_ARGUMENT:binding missing nodeId";
    // Optional output dimension: string port name or numeric port 1..65535
    // under the single outputPort key.
    std::string out_s;
    bool has_out_s = false;
    int out_p = 0;
    bool has_out_p = false;
    json_object* v = nullptr;
    if (json_object_object_get_ex(doc, "outputPort", &v) && v) {
        if (json_object_is_type(v, json_type_string)) {
            out_s = json_object_get_string(v);
            has_out_s = true;
        } else if (json_object_is_type(v, json_type_int)) {
            out_p = json_object_get_int(v);
            has_out_p = true;
        } else {
            return "INVALID_ARGUMENT:binding outputPort must be string or int";
        }
    }
    if (has_out_s && (out_s.empty() || out_s.size() > 64))
        return "INVALID_ARGUMENT:binding outputPort must be 1..64 chars";
    if (has_out_p && (out_p < 1 || out_p > 65535))
        return "INVALID_ARGUMENT:binding outputPort must be 1..65535";
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
    if (sig.empty()) return "INVALID_ARGUMENT:whitelist missing sig";
    std::string addr = jstr(doc, "address");
    if (addr.empty()) return "INVALID_ARGUMENT:whitelist missing address";
    // Optional SSM source filter: when present the receivers join the group
    // with a source check instead of any-source multicast.
    json_object* sipv = nullptr;
    if (json_object_object_get_ex(doc, "sourceIp", &sipv) && sipv) {
        if (!json_object_is_type(sipv, json_type_string) ||
            !valid_ip4_addr(json_object_get_string(sipv)))
            return "INVALID_ARGUMENT:whitelist sourceIp must be IPv4";
    }
    int port = 0;
    json_object* v = nullptr;
    if (json_object_object_get_ex(doc, "port", &v) && v) port = json_object_get_int(v);
    if (port != 0 && (port < 1 || port > 65535))
        return "INVALID_ARGUMENT:whitelist port must be 1..65535";
    // Optional per-signal audio (ST 2110-30) port. Absent or zero means the
    // signal carries no subscribable audio; the agent then omits audioUdpPort
    // and the engine honestly reports the owner as not applied (mute).
    json_object* apv = nullptr;
    if (json_object_object_get_ex(doc, "audioPort", &apv) && apv) {
        if (!json_object_is_type(apv, json_type_int))
            return "INVALID_ARGUMENT:whitelist audioPort must be int";
        int aport = json_object_get_int(apv);
        if (aport < 1 || aport > 65535)
            return "INVALID_ARGUMENT:whitelist audioPort must be 1..65535";
    }
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

std::string validate_image_adjust(json_object* doc) {
    if (!doc || !json_object_is_type(doc, json_type_object))
        return "INVALID_ARGUMENT:imageAdjust must be object";
    static const char* kKeys[] = {"brightness", "contrast", "saturation",
                                  "hue"};
    bool any = false;
    for (auto k : kKeys) {
        json_object* kv = nullptr;
        if (json_object_object_get_ex(doc, k, &kv) && kv) {
            if (!json_object_is_type(kv, json_type_int))
                return std::string("INVALID_ARGUMENT:imageAdjust.") + k +
                       " must be int";
            int n = json_object_get_int(kv);
            if (n < -100 || n > 100)
                return std::string("INVALID_ARGUMENT:imageAdjust.") + k +
                       " must be -100..100";
            any = true;
        }
    }
    if (!any) return "INVALID_ARGUMENT:imageAdjust needs at least one key";
    return "";
}

std::string validate_base_image(json_object* doc) {
    if (!doc || !json_object_is_type(doc, json_type_object))
        return "INVALID_ARGUMENT:baseImage must be object";
    json_object* v = nullptr;
    if (!json_object_object_get_ex(doc, "revision", &v) || !v ||
        !json_object_is_type(v, json_type_int) || json_object_get_int(v) < 0)
        return "INVALID_ARGUMENT:baseImage revision must be int >= 0";
    std::string url = jstr(doc, "url");
    if (url.empty() || url.size() > 1024)
        return "INVALID_ARGUMENT:baseImage url required";
    for (const char* k : {"width", "height"}) {
        json_object* vv = nullptr;
        if (json_object_object_get_ex(doc, k, &vv) && vv) {
            if (!json_object_is_type(vv, json_type_int) ||
                json_object_get_int(vv) <= 0)
                return std::string("INVALID_ARGUMENT:baseImage ") + k +
                       " must be positive";
        }
    }
    json_object* sub = nullptr;
    if (json_object_object_get_ex(doc, "sub", &sub) && sub) {
        if (!json_object_is_type(sub, json_type_object))
            return "INVALID_ARGUMENT:baseImage sub must be object";
        for (const char* k : {"x", "y", "w", "h"}) {
            json_object* kv = nullptr;
            if (!json_object_object_get_ex(sub, k, &kv) || !kv ||
                !json_object_is_type(kv, json_type_int))
                return "INVALID_ARGUMENT:baseImage sub must be {x,y,w,h}";
        }
        if (rect_int(sub, "w") <= 0 || rect_int(sub, "h") <= 0)
            return "INVALID_ARGUMENT:baseImage sub must be {x,y,w,h}";
    }
    return "";
}

int node_decode_budget(json_object* node) {
    if (!node || !json_object_is_type(node, json_type_object)) return -1;
    json_object* cap = nullptr;
    if (!json_object_object_get_ex(node, "capability", &cap) || !cap ||
        !json_object_is_type(cap, json_type_object))
        return -1;
    json_object* v = nullptr;
    if (!json_object_object_get_ex(cap, "maxDecodeSessions", &v) || !v ||
        !json_object_is_type(v, json_type_int))
        return -1;
    int n = json_object_get_int(v);
    return n > 0 ? n : -1;
}

int count_node_signals(json_object* windows, const Rect& node) {
    json_object* frag = fragment_for_node(windows, node);
    std::set<std::string> sigs;
    if (frag && json_object_is_type(frag, json_type_array)) {
        for (size_t i = 0; i < json_object_array_length(frag); i++) {
            json_object* w = json_object_array_get_idx(frag, i);
            if (!w) continue;
            std::string sig = jstr(w, "sig");
            if (!sig.empty()) sigs.insert(sig);
        }
    }
    if (frag) json_object_put(frag);
    return (int)sigs.size();
}

json_object* snap_windows_to_cells(
    json_object* windows,
    const std::vector<std::pair<std::string, Rect>>& cells) {
    json_object* out = json_object_new_array();
    if (!windows || !json_object_is_type(windows, json_type_array) || cells.empty()) {
        if (windows && json_object_is_type(windows, json_type_array)) {
            for (size_t i = 0; i < json_object_array_length(windows); i++)
                json_object_array_add(out,
                    json_object_get(json_object_array_get_idx(windows, i)));
        }
        return out;
    }
    for (size_t i = 0; i < json_object_array_length(windows); i++) {
        json_object* w = json_object_array_get_idx(windows, i);
        json_object* c = json_object_new_object();
        if (!w || !json_object_is_type(w, json_type_object)) {
            json_object_array_add(out, c);
            continue;
        }
        int x = rect_int(w, "x"), y = rect_int(w, "y");
        int ww = rect_int(w, "w"), hh = rect_int(w, "h");
        if (ww > 0 && hh > 0) {
            int cx = x + ww / 2, cy = y + hh / 2;
            for (const auto& kv : cells) {
                const Rect& r = kv.second;
                if (cx >= r.x && cx < r.x + r.w && cy >= r.y && cy < r.y + r.h) {
                    x = r.x;
                    y = r.y;
                    ww = r.w;
                    hh = r.h;
                    break;
                }
            }
        }
        json_object_object_foreach(w, key, val) {
            std::string k(key);
            if (k == "x")
                json_object_object_add(c, "x", json_object_new_int(x));
            else if (k == "y")
                json_object_object_add(c, "y", json_object_new_int(y));
            else if (k == "w")
                json_object_object_add(c, "w", json_object_new_int(ww));
            else if (k == "h")
                json_object_object_add(c, "h", json_object_new_int(hh));
            else
                json_object_object_add(c, key, json_object_get(val));
        }
        json_object_array_add(out, c);
    }
    return out;
}

int wall_min_window_side(json_object* wall, int wall_w, int wall_h) {
    std::vector<std::pair<std::string, Rect>> cells;
    wall_cells(wall, wall_w, wall_h, &cells);
    int side = 0;
    for (const auto& kv : cells) {
        int m = std::min(kv.second.w, kv.second.h);
        if (m > 0 && (side == 0 || m < side)) side = m;
    }
    if (side <= 0) return 64;
    int floor = side / 4;
    return floor < 64 ? 64 : floor;
}

void wall_cells(json_object* wall, int wall_w, int wall_h,
                std::vector<std::pair<std::string, Rect>>* out) {
    if (!out) return;
    out->clear();
    if (!wall || !json_object_is_type(wall, json_type_object)) return;
    json_object* cg = nullptr;
    if (json_object_object_get_ex(wall, "cell_grid", &cg) && cg &&
        json_object_is_type(cg, json_type_array) &&
        json_object_array_length(cg) > 0) {
        for (size_t i = 0; i < json_object_array_length(cg); i++) {
            json_object* e = json_object_array_get_idx(cg, i);
            if (!e || !json_object_is_type(e, json_type_object)) continue;
            int x = rect_int(e, "x"), y = rect_int(e, "y");
            int w = rect_int(e, "w"), h = rect_int(e, "h");
            if (w <= 0 || h <= 0) continue;
            std::string id = jstr(e, "id");
            if (id.empty()) {
                char b[32];
                snprintf(b, sizeof(b), "%d:%d", rect_int(e, "r"),
                         rect_int(e, "c"));
                id = b;
            }
            out->emplace_back(id, Rect{x, y, w, h});
        }
        return;
    }
    int rows = jint(wall, "rows", 0), cols = jint(wall, "cols", 0);
    if (rows <= 0 || cols <= 0 || wall_w <= 0 || wall_h <= 0) return;
    int cw = wall_w / cols, ch = wall_h / rows;
    if (cw <= 0 || ch <= 0) return;
    for (int r = 0; r < rows; r++) {
        for (int c = 0; c < cols; c++) {
            char b[32];
            snprintf(b, sizeof(b), "%d:%d", r, c);
            int w = (c == cols - 1) ? (wall_w - cw * c) : cw;
            int h = (r == rows - 1) ? (wall_h - ch * r) : ch;
            out->emplace_back(b, Rect{c * cw, r * ch, w, h});
        }
    }
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
    std::string scene = jstr(doc, "sceneId");
    std::string node = jstr(doc, "targetNodeId");
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

std::string validate_volume(int volume) {
    if (volume < 0 || volume > 100)
        return "INVALID_ARGUMENT:volume must be 0..100";
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
    json_object_object_get_ex(node, "ptp", &ptp);
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

bool barrier_lanes_ready(json_object* lanes) {
    if (!lanes || !json_object_is_type(lanes, json_type_array)) return true;
    for (size_t i = 0; i < json_object_array_length(lanes); i++) {
        json_object* e = json_object_array_get_idx(lanes, i);
        json_object* rv = nullptr;
        if (e && json_object_object_get_ex(e, "ready", &rv) && rv) {
            if (!json_object_get_boolean(rv)) return false;
        }
    }
    return true;
}

bool barrier_lanes_frame_consistent(json_object* lanes, int64_t* seq_out) {
    if (seq_out) *seq_out = 0;
    if (!lanes || !json_object_is_type(lanes, json_type_array)) return true;
    bool have = false;
    int64_t first = 0;
    for (size_t i = 0; i < json_object_array_length(lanes); i++) {
        json_object* e = json_object_array_get_idx(lanes, i);
        if (!e || !json_object_is_type(e, json_type_object)) continue;
        json_object* v = nullptr;
        int64_t seq = 0;
        bool present = false;
        if (json_object_object_get_ex(e, "frameSeq", &v) && v &&
            json_object_is_type(v, json_type_int)) {
            seq = json_object_get_int64(v);
            present = true;
        } else if (json_object_object_get_ex(e, "frame_seq", &v) && v &&
                   json_object_is_type(v, json_type_int)) {
            seq = json_object_get_int64(v);
            present = true;
        } else if (json_object_object_get_ex(e, "frame", &v) && v &&
                   json_object_is_type(v, json_type_int)) {
            seq = json_object_get_int64(v);
            present = true;
        }
        if (!present) continue;
        if (!have) {
            have = true;
            first = seq;
            if (seq_out) *seq_out = seq;
        } else if (seq != first) {
            return false;
        }
    }
    return true;
}

int fanout_parallelism() {
    const char* v = std::getenv("DVS_FANOUT_PARALLEL");
    if (!v || !*v) return 8;
    int n = atoi(v);
    if (n < 1) return 1;
    if (n > 16) return 16;
    return n;
}

int max_wall_members() {
    const char* v = std::getenv("DVS_MAX_WALL_MEMBERS");
    if (!v || !*v) return 128;
    int n = atoi(v);
    return n > 0 ? n : 128;
}

void wall_px_of(json_object* wall, int* w, int* h) {
    int ww = 0, hh = 0;
    if (wall) {
        ww = rect_int(wall, "wall_px_w");
        hh = rect_int(wall, "wall_px_h");
        if (ww <= 0) ww = rect_int(wall, "w");
        if (hh <= 0) hh = rect_int(wall, "h");
        json_object* rp = nullptr;
        json_object* cp = nullptr;
        if (ww <= 0 && json_object_object_get_ex(wall, "colPixels", &cp) && cp &&
            json_object_is_type(cp, json_type_array)) {
            for (size_t i = 0; i < (size_t)json_object_array_length(cp); i++)
                ww += json_object_get_int(json_object_array_get_idx(cp, (int)i));
        }
        if (hh <= 0 && json_object_object_get_ex(wall, "rowPixels", &rp) && rp &&
            json_object_is_type(rp, json_type_array)) {
            for (size_t i = 0; i < (size_t)json_object_array_length(rp); i++)
                hh += json_object_get_int(json_object_array_get_idx(rp, (int)i));
        }
    }
    if (w) *w = ww;
    if (h) *h = hh;
}

double wall_fps_of(json_object* wall) {
    json_object* v = nullptr;
    if (wall && json_object_object_get_ex(wall, "fps", &v) && v) {
        double f = json_object_get_double(v);
        if (f > 0.0) return f;
    }
    return 60.0;
}

namespace {
std::string doc_str(json_object* o, const char* key) {
    json_object* v = nullptr;
    if (o && json_object_object_get_ex(o, key, &v) && v)
        return json_object_get_string(v);
    return "";
}
}  // namespace

std::string binding_wall_of(json_object* doc) { return doc_str(doc, "wallId"); }
std::string binding_node_of(json_object* doc) { return doc_str(doc, "nodeId"); }
std::string binding_cell_of(json_object* doc) { return doc_str(doc, "cellId"); }

void whitelist_addr_port(json_object* doc, std::string* addr, int* port) {
    int p = 0;
    json_object* v = nullptr;
    if (json_object_object_get_ex(doc, "port", &v) && v) p = json_object_get_int(v);
    if (addr) *addr = doc_str(doc, "address");
    if (port) *port = p;
}

std::string playback_status_of(json_object* doc) { return doc_str(doc, "status"); }

bool playback_transition_ok(const std::string& from, const std::string& to) {
    if (from.empty()) return to.empty() || to == "pending_check";
    if (from == to) return true;
    if (from == "pending_check" &&
        (to == "pending_execution" || to == "failed" || to == "cancelled"))
        return true;
    if (from == "pending_execution" &&
        (to == "executing" || to == "failed" || to == "cancelled" ||
         to == "expired"))
        return true;
    if (from == "executing" &&
        (to == "playing" || to == "failed" || to == "cancelled" ||
         to == "expired"))
        return true;
    if (from == "playing" &&
        (to == "succeeded" || to == "failed" || to == "cancelled" ||
         to == "expired"))
        return true;
    return false;
}

bool playback_active_status(const std::string& st) {
    return st == "executing" || st == "playing";
}
