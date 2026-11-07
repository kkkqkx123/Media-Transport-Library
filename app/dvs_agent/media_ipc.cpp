#include "media_ipc.h"
#include "dvs_node_bundle.h"

#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

long long MediaIpcClient::now_ms() {
    struct timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

void MediaIpcClient::note_ok_locked() {
    fail_streak_ = 0;
    ok_ms_ = now_ms();
    last_code_ = "OK";
}

void MediaIpcClient::note_err_locked(const char* code) {
    fail_streak_++;
    err_ms_ = now_ms();
    last_code_ = code ? code : "UNKNOWN";
}

static bool is_event_type(const std::string& type) {
    return type.rfind("media.", 0) == 0 && type != "media.status";
}

static json_object* parse_line(const std::string& line) {
    if (line.empty()) return nullptr;
    return json_tokener_parse(line.c_str());
}

static std::string error_envelope(const char* code, const char* msg) {
    json_object* o = json_object_new_object();
    json_object_object_add(o, "ok", json_object_new_boolean(0));
    json_object_object_add(o, "status", json_object_new_int(503));
    json_object_object_add(o, "code", json_object_new_string(code));
    json_object_object_add(o, "msg", json_object_new_string(msg));
    json_object_object_add(o, "data", json_object_new_object());
    const char* s = json_object_to_json_string(o);
    std::string out(s);
    json_object_put(o);
    return out;
}

MediaIpcClient::MediaIpcClient(const std::string& sock_path)
    : sock_path_(sock_path) {}

MediaIpcClient::~MediaIpcClient() {
    std::lock_guard<std::mutex> lk(mtx_);
    close_locked();
}

void MediaIpcClient::close_locked() {
    if (fd_ >= 0) {
        ::shutdown(fd_, SHUT_RDWR);
        ::close(fd_);
        fd_ = -1;
    }
}

bool MediaIpcClient::ensure_conn_locked() {
    if (fd_ >= 0) return true;
    if (sock_path_.empty()) return false;

    fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) return false;

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path_.c_str(), sizeof(addr.sun_path) - 1);
    if (connect(fd_, (sockaddr*)&addr, sizeof(addr)) < 0) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }
    pending_line_.clear();
    return true;
}

static void cache_event_locked(std::string* events, size_t kRing, size_t& head,
                               size_t& count, const std::string& full) {
    size_t tail = (head + count) % kRing;
    if (count == kRing) {
        events[head].clear();
        head = (head + 1) % kRing;
        count--;
        tail = (head + count) % kRing;
    }
    events[tail] = full;
    count++;
}

void MediaIpcClient::drain_socket_locked() {
    if (fd_ < 0) return;
    for (;;) {
        size_t pos = pending_line_.find('\n');
        while (pos != std::string::npos) {
            std::string full = pending_line_.substr(0, pos);
            pending_line_.erase(0, pos + 1);
            json_object* o = parse_line(full);
            if (o) {
                json_object* tv = nullptr;
                const char* t = nullptr;
                if (json_object_object_get_ex(o, "type", &tv) && tv)
                    t = json_object_get_string(tv);
                if (t && is_event_type(t))
                    cache_event_locked(events_, kEventRing, ev_head_, ev_count_, full);
                /* stray non-event with no waiter is discarded */
                json_object_put(o);
            }
            pos = pending_line_.find('\n');
        }
        struct pollfd pfd = {.fd = fd_, .events = POLLIN};
        int pr = poll(&pfd, 1, 0);
        if (pr < 0) {
            if (errno == EINTR) continue;
            close_locked();
            return;
        }
        if (pr == 0) return;
        if (pfd.revents & (POLLERR | POLLNVAL)) {
            close_locked();
            return;
        }
        if (!(pfd.revents & POLLIN)) {
            if (pfd.revents & POLLHUP) {
                char tmp[512];
                ssize_t n = read(fd_, tmp, sizeof(tmp));
                if (n > 0) {
                    pending_line_.append(tmp, (size_t)n);
                    continue;
                }
                close_locked();
            }
            return;
        }
        char buf[8192];
        ssize_t n = read(fd_, buf, sizeof(buf));
        if (n <= 0) {
            close_locked();
            return;
        }
        pending_line_.append(buf, (size_t)n);
    }
}

std::string MediaIpcClient::request(const std::string& type, json_object* payload,
                                    int timeout_ms) {
    if (sock_path_.empty()) {
        std::lock_guard<std::mutex> lk(mtx_);
        note_err_locked("MEDIA_ENGINE_UNCONFIGURED");
        return error_envelope("MEDIA_ENGINE_UNCONFIGURED",
                              "media IPC socket not configured");
    }

    std::lock_guard<std::mutex> lk(mtx_);

    /* build request line */
    std::string expect;
    json_object* req = json_object_new_object();
    json_object_object_add(req, "type", json_object_new_string(type.c_str()));
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "req-%ld", ++seq_);
        expect = buf;
        json_object_object_add(req, "requestId", json_object_new_string(buf));
    }
    if (payload)
        json_object_object_add(req, "payload", json_object_get(payload));
    const char* line = json_object_to_json_string(req);
    std::string msg = std::string(line) + "\n";
    json_object_put(req);

    for (int attempt = 0; attempt < 2; attempt++) {
        if (!ensure_conn_locked()) {
            note_err_locked("MEDIA_ENGINE_UNREACHABLE");
            return error_envelope("MEDIA_ENGINE_UNREACHABLE",
                                  "cannot connect to media engine IPC socket");
        }

        size_t total = 0;
        while (total < msg.size()) {
            ssize_t n = write(fd_, msg.data() + total, msg.size() - total);
            if (n <= 0) { close_locked(); break; }
            total += (size_t)n;
        }
        if (total != msg.size()) continue; /* reconnect and retry once */

        /* read until we get the full response line for this request.
         * Event lines are cached; the response echoes our requestId. */
        std::string response;
        for (;;) {
            size_t pos = pending_line_.find('\n');
            if (pos != std::string::npos) {
                std::string full = pending_line_.substr(0, pos);
                pending_line_.erase(0, pos + 1);
                json_object* o = parse_line(full);
                if (!o) continue;
                json_object* tv = nullptr;
                const char* t = nullptr;
                if (json_object_object_get_ex(o, "type", &tv) && tv)
                    t = json_object_get_string(tv);
                if (t && is_event_type(t)) {
                    cache_event_locked(events_, kEventRing, ev_head_, ev_count_, full);
                    json_object_put(o);
                    continue;
                }
                const char* rid = nullptr;
                json_object* ridv = nullptr;
                if (json_object_object_get_ex(o, "requestId", &ridv) && ridv)
                    rid = json_object_get_string(ridv);
                if (!rid || expect != rid) {
                    /* stray response for another waiter: discard */
                    json_object_put(o);
                    continue;
                }
                json_object_put(o);
                response = full;
                break;
            }

            struct pollfd pfd = {.fd = fd_, .events = POLLIN};
            int pr = poll(&pfd, 1, timeout_ms);
            if (pr < 0) {
                if (errno == EINTR) continue;
                close_locked();
                break;
            }
            if (pr == 0) {
                close_locked();
                note_err_locked("MEDIA_ENGINE_TIMEOUT");
                return error_envelope("MEDIA_ENGINE_TIMEOUT",
                                      "media engine did not respond in time");
            }
            char buf[8192];
            ssize_t n = read(fd_, buf, sizeof(buf));
            if (n <= 0) { close_locked(); break; }
            pending_line_.append(buf, (size_t)n);
        }
        if (!response.empty()) {
            /* A received response proves the link is alive, even when the
             * engine refuses the verb itself; only transport failures feed
             * the streak. The code still records the last verdict. */
            json_object* ro = parse_line(response);
            if (ro) {
                json_object* cv = nullptr;
                const char* code = "OK";
                if (json_object_object_get_ex(ro, "code", &cv) && cv)
                    code = json_object_get_string(cv);
                fail_streak_ = 0;
                ok_ms_ = now_ms();
                last_code_ = code ? code : "OK";
                json_object_put(ro);
            } else {
                note_ok_locked();
            }
            return response;
        }

        /* connection dropped mid-request: retry once */
        if (attempt == 1) {
            note_err_locked("MEDIA_ENGINE_UNREACHABLE");
            return error_envelope("MEDIA_ENGINE_UNREACHABLE",
                                  "media engine closed the connection");
        }
    }
    note_err_locked("MEDIA_ENGINE_UNREACHABLE");
    return error_envelope("MEDIA_ENGINE_UNREACHABLE", "media engine unavailable");
}

std::string MediaIpcClient::query_state() {
    return request("query_state", nullptr);
}

std::string MediaIpcClient::hello() {
    return request("hello", nullptr);
}

bool MediaIpcClient::parse_hello(const std::string& hello, bool& answered,
                                 bool& match, std::string& eng_bundle, int& eng_ipc) {
    answered = false;
    match = false;
    eng_bundle.clear();
    eng_ipc = -1;
    json_object* ho = json_tokener_parse(hello.c_str());
    if (!ho) return false;
    json_object* okv = nullptr;
    json_object* data = nullptr;
    bool ok = json_object_object_get_ex(ho, "ok", &okv) && okv &&
              json_object_get_boolean(okv);
    if (ok && json_object_object_get_ex(ho, "data", &data) && data) {
        answered = true;
        json_object* iv = nullptr;
        json_object* bv = nullptr;
        json_object_object_get_ex(data, "ipcVersion", &iv);
        json_object_object_get_ex(data, "bundleVersion", &bv);
        if (iv) eng_ipc = json_object_get_int(iv);
        if (bv) eng_bundle = json_object_get_string(bv);
        match = iv && json_object_get_int(iv) == DVS_MEDIA_IPC_VERSION && bv &&
                std::string(json_object_get_string(bv)) == DVS_NODE_BUNDLE_VERSION;
    }
    json_object_put(ho);
    return true;
}

json_object* MediaIpcClient::drain_events() {
    std::lock_guard<std::mutex> lk(mtx_);
    drain_socket_locked();
    json_object* arr = json_object_new_array();
    for (size_t i = 0; i < ev_count_; i++) {
        const std::string& line = events_[(ev_head_ + i) % kEventRing];
        json_object* ev = parse_line(line);
        if (ev) json_object_array_add(arr, ev);
    }
    ev_head_ = 0;
    ev_count_ = 0;
    return arr;
}

size_t MediaIpcClient::pending_events() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return ev_count_;
}

bool MediaIpcClient::connected() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return fd_ >= 0;
}

int MediaIpcClient::fail_streak() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return fail_streak_;
}

long long MediaIpcClient::last_ok_ms() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return ok_ms_;
}

long long MediaIpcClient::last_err_ms() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return err_ms_;
}

std::string MediaIpcClient::last_code() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return last_code_;
}

bool MediaIpcClient::scan_result_locked(const std::string& opid,
                                        std::string& status_out,
                                        std::string& lanes_json_out) {
    for (size_t k = 0; k < ev_count_; k++) {
        size_t idx = (ev_head_ + k) % kEventRing;
        json_object* o = parse_line(events_[idx]);
        if (!o) continue;
        json_object* tv = nullptr;
        const char* t = nullptr;
        if (json_object_object_get_ex(o, "type", &tv) && tv)
            t = json_object_get_string(tv);
        bool hit = false;
        if (t && strcmp(t, "media.prepare.result") == 0) {
            json_object* dd = nullptr;
            if (json_object_object_get_ex(o, "data", &dd) && dd) {
                json_object* ov = nullptr;
                if (json_object_object_get_ex(dd, "operationId", &ov) && ov &&
                    opid == json_object_get_string(ov))
                    hit = true;
            }
        }
        if (hit) {
            json_object* dd = nullptr;
            json_object_object_get_ex(o, "data", &dd);
            status_out = "blocked";
            lanes_json_out = "[]";
            if (dd) {
                json_object* sv = nullptr;
                if (json_object_object_get_ex(dd, "status", &sv) && sv &&
                    strcmp(json_object_get_string(sv), "ready") == 0)
                    status_out = "ready";
                json_object* lj = nullptr;
                if (json_object_object_get_ex(dd, "lanes", &lj) && lj) {
                    const char* ls = json_object_to_json_string(lj);
                    if (ls) lanes_json_out = ls;
                }
            }
            json_object_put(o);
            /* consume the entry and compact the ring */
            for (size_t m = k; m + 1 < ev_count_; m++)
                events_[(ev_head_ + m) % kEventRing] =
                    events_[(ev_head_ + m + 1) % kEventRing];
            events_[(ev_head_ + ev_count_ - 1) % kEventRing].clear();
            ev_count_--;
            return true;
        }
        json_object_put(o);
    }
    return false;
}

bool MediaIpcClient::wait_prepare_result(const std::string& opid, int timeout_ms,
                                         std::string& status_out,
                                         std::string& lanes_json_out) {
    if (opid.empty() || timeout_ms <= 0) return false;
    long long deadline = now_ms() + timeout_ms;
    /* Fast path: the result may already sit in the ring. */
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (scan_result_locked(opid, status_out, lanes_json_out)) return true;
    }
    for (;;) {
        long long now = now_ms();
        if (now >= deadline) break;
        int fd = -1;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            fd = fd_;
        }
        if (fd < 0) {
            struct timespec ts = {.tv_sec = 0, .tv_nsec = 50 * 1000000L};
            nanosleep(&ts, nullptr);
            continue;
        }
        long long rest = deadline - now_ms();
        if (rest <= 0) break;
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int pr = poll(&pfd, 1, rest > 200 ? 200 : (int)rest);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;
        std::lock_guard<std::mutex> lk(mtx_);
        /* The socket may have been recycled while unlocked; rescan instead
         * of reading a stale descriptor. */
        if (fd_ != fd) {
            if (scan_result_locked(opid, status_out, lanes_json_out)) return true;
            continue;
        }
        drain_socket_locked();
        if (scan_result_locked(opid, status_out, lanes_json_out)) return true;
    }
    std::lock_guard<std::mutex> lk(mtx_);
    return scan_result_locked(opid, status_out, lanes_json_out);
}

/* Translate a platform sub-layout (flat windows with windowId/sig/dst/src/z/
 * muted/partial plus full-frame dims, an endpoint table, audio owner and
 * scene volume) into the engine "streams" shape. Returns false on a
 * whole-order rejection (signal not in the endpoint table or over the lane
 * cap); on success *prep_out holds the prepare payload with lanes assigned
 * by z ascending, ties in array order. Cross-node windows ride the engine
 * source crop; defensive clipping drops windows fully outside the canvas. */
bool translate_platform_layout(json_object* body, json_object** prep_out,
                               std::string& err_out) {
    *prep_out = nullptr;
    if (!body || !json_object_is_type(body, json_type_object)) {
        err_out = "INVALID_ARGUMENT:empty body";
        return false;
    }
    json_object* wins = nullptr;
    json_object_object_get_ex(body, "windows", &wins);
    int wn = (wins && json_object_is_type(wins, json_type_array))
                 ? (int)json_object_array_length(wins) : 0;
    json_object* geo = nullptr;
    json_object_object_get_ex(body, "geometry", &geo);
    int canvas_w = 0, canvas_h = 0;
    if (geo) {
        json_object* v = nullptr;
        if (json_object_object_get_ex(geo, "w", &v) && v) canvas_w = json_object_get_int(v);
        if (json_object_object_get_ex(geo, "h", &v) && v) canvas_h = json_object_get_int(v);
    }
    /* Endpoint lookup: sig -> (address, port). A signal without a table
     * entry leaves the agent no way to name its stream — reject the order. */
    std::map<std::string, std::pair<std::string, int>> endpoints;
    json_object* eps = nullptr;
    if (json_object_object_get_ex(body, "endpoints", &eps) &&
        json_object_is_type(eps, json_type_array)) {
        int en = (int)json_object_array_length(eps);
        for (int i = 0; i < en; i++) {
            json_object* e = json_object_array_get_idx(eps, i);
            if (!e || !json_object_is_type(e, json_type_object)) continue;
            json_object* v = nullptr;
            std::string sig, addr;
            int port = 0;
            if (json_object_object_get_ex(e, "sig", &v) && v)
                sig = json_object_get_string(v);
            if (json_object_object_get_ex(e, "address", &v) && v)
                addr = json_object_get_string(v);
            if (json_object_object_get_ex(e, "port", &v) && v)
                port = json_object_get_int(v);
            if (!sig.empty() && !addr.empty() && port > 0)
                endpoints[sig] = {addr, port};
        }
    }
    if (wn > 8) {  // MP4_PIP_MAX_LANES (display_layout.h)
        err_out = "INVALID_ARGUMENT:too many windows for engine lanes";
        return false;
    }
    /* Stable z-ascending order: same layer order falls back to array order,
     * so channel assignment is deterministic. */
    std::vector<int> order((size_t)wn);
    for (int i = 0; i < wn; i++) order[(size_t)i] = i;
    auto z_of = [&](int i) -> int {
        json_object* w = json_object_array_get_idx(wins, i);
        json_object* v = nullptr;
        return (w && json_object_object_get_ex(w, "z", &v) && v)
                   ? json_object_get_int(v) : 0;
    };
    std::stable_sort(order.begin(), order.end(),
                     [&](int a, int b) { return z_of(a) < z_of(b); });

    json_object* streams = json_object_new_array();
    for (int idx : order) {
        json_object* w = json_object_array_get_idx(wins, idx);
        if (!w || !json_object_is_type(w, json_type_object)) continue;
        json_object* v = nullptr;
        std::string sig, win_id;
        if (json_object_object_get_ex(w, "sig", &v) && v) sig = json_object_get_string(v);
        if (json_object_object_get_ex(w, "windowId", &v) && v) win_id = json_object_get_string(v);
        auto it = endpoints.find(sig);
        if (it == endpoints.end()) {
            json_object_put(streams);
            err_out = "INVALID_ARGUMENT:signal '" + sig + "' not in endpoint table";
            return false;
        }
        int dx = 0, dy = 0, dw = 0, dh = 0;
        json_object* dst = nullptr;
        if (json_object_object_get_ex(w, "dst", &dst) && dst) {
            if (json_object_object_get_ex(dst, "x", &v) && v) dx = json_object_get_int(v);
            if (json_object_object_get_ex(dst, "y", &v) && v) dy = json_object_get_int(v);
            if (json_object_object_get_ex(dst, "w", &v) && v) dw = json_object_get_int(v);
            if (json_object_object_get_ex(dst, "h", &v) && v) dh = json_object_get_int(v);
        }
        if (dw <= 0 || dh <= 0) continue;
        int sx = 0, sy = 0, sw = dw, sh = dh;
        json_object* src = nullptr;
        if (json_object_object_get_ex(w, "src", &src) && src) {
            if (json_object_object_get_ex(src, "x", &v) && v) sx = json_object_get_int(v);
            if (json_object_object_get_ex(src, "y", &v) && v) sy = json_object_get_int(v);
            if (json_object_object_get_ex(src, "w", &v) && v) sw = json_object_get_int(v);
            if (json_object_object_get_ex(src, "h", &v) && v) sh = json_object_get_int(v);
        }
        int full_w = sw, full_h = sh;
        if (json_object_object_get_ex(w, "fullW", &v) && v &&
            json_object_get_int(v) > 0)
            full_w = json_object_get_int(v);
        if (json_object_object_get_ex(w, "fullH", &v) && v &&
            json_object_get_int(v) > 0)
            full_h = json_object_get_int(v);
        int cx1 = std::max(0, dx), cy1 = std::max(0, dy);
        int cx2 = std::min(canvas_w, dx + dw), cy2 = std::min(canvas_h, dy + dh);
        if (cx2 <= cx1 || cy2 <= cy1) {
            fprintf(stderr, "dvs_agent: window '%s' fully outside canvas, dropped\n",
                    win_id.c_str());
            continue;
        }
        sx += (cx1 - dx);
        sy += (cy1 - dy);
        sw = cx2 - cx1;
        sh = cy2 - cy1;
        int lane = (int)json_object_array_length(streams);
        json_object* s = json_object_new_object();
        json_object_object_add(s, "lane", json_object_new_int(lane));
        json_object_object_add(s, "main", json_object_new_boolean(lane == 0));
        json_object_object_add(s, "sip", json_object_new_string(it->second.first.c_str()));
        json_object_object_add(s, "udpPort", json_object_new_int(it->second.second));
        json_object_object_add(s, "width", json_object_new_int(full_w));
        json_object_object_add(s, "height", json_object_new_int(full_h));
        json_object_object_add(s, "x", json_object_new_int(cx1));
        json_object_object_add(s, "y", json_object_new_int(cy1));
        json_object_object_add(s, "w", json_object_new_int(cx2 - cx1));
        json_object_object_add(s, "h", json_object_new_int(cy2 - cy1));
        json_object_object_add(s, "srcX", json_object_new_int(sx));
        json_object_object_add(s, "srcY", json_object_new_int(sy));
        json_object_object_add(s, "srcW", json_object_new_int(sw));
        json_object_object_add(s, "srcH", json_object_new_int(sh));
        json_object_array_add(streams, s);
    }
    json_object* prep = json_object_new_object();
    json_object* opv = nullptr;
    if (json_object_object_get_ex(body, "operationId", &opv) && opv)
        json_object_object_add(prep, "operationId", json_object_get(opv));
    json_object* rv = nullptr;
    if (json_object_object_get_ex(body, "revision", &rv) && rv)
        json_object_object_add(prep, "revision", json_object_get(rv));
    json_object* av = nullptr;
    if (json_object_object_get_ex(body, "applyAtNs", &av) && av)
        json_object_object_add(prep, "applyAtNs", json_object_get(av));
    /* Empty array = explicit clear (keep backdrop); it must still be staged
     * so the commit applies an empty layout instead of keeping the picture. */
    json_object_object_add(prep, "streams", streams);
    json_object* aov = nullptr;
    if (json_object_object_get_ex(body, "audioOwner", &aov) && aov)
        json_object_object_add(prep, "audioOwner", json_object_get(aov));
    json_object* vol = nullptr;
    if (json_object_object_get_ex(body, "volume", &vol) && vol)
        json_object_object_add(prep, "audioVolume", json_object_get(vol));
    *prep_out = prep;
    return true;
}
