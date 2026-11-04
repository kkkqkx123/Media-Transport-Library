#include "media_ipc.h"

#include <errno.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cstring>

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
    if (sock_path_.empty())
        return error_envelope("MEDIA_ENGINE_UNCONFIGURED",
                              "media IPC socket not configured");

    std::lock_guard<std::mutex> lk(mtx_);

    /* build request line */
    std::string expect;
    json_object* req = json_object_new_object();
    json_object_object_add(req, "type", json_object_new_string(type.c_str()));
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "req-%ld", ++seq_);
        expect = buf;
        json_object_object_add(req, "messageId", json_object_new_string(buf));
    }
    if (payload)
        json_object_object_add(req, "payload", json_object_get(payload));
    const char* line = json_object_to_json_string(req);
    std::string msg = std::string(line) + "\n";
    json_object_put(req);

    for (int attempt = 0; attempt < 2; attempt++) {
        if (!ensure_conn_locked())
            return error_envelope("MEDIA_ENGINE_UNREACHABLE",
                                  "cannot connect to media engine IPC socket");

        size_t total = 0;
        while (total < msg.size()) {
            ssize_t n = write(fd_, msg.data() + total, msg.size() - total);
            if (n <= 0) { close_locked(); break; }
            total += (size_t)n;
        }
        if (total != msg.size()) continue; /* reconnect and retry once */

        /* read until we get the full response line for this request.
         * Event lines are cached; the response echoes our messageId. */
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
                return error_envelope("MEDIA_ENGINE_TIMEOUT",
                                      "media engine did not respond in time");
            }
            char buf[8192];
            ssize_t n = read(fd_, buf, sizeof(buf));
            if (n <= 0) { close_locked(); break; }
            pending_line_.append(buf, (size_t)n);
        }
        if (!response.empty()) return response;

        /* connection dropped mid-request: retry once */
        if (attempt == 1)
            return error_envelope("MEDIA_ENGINE_UNREACHABLE",
                                  "media engine closed the connection");
    }
    return error_envelope("MEDIA_ENGINE_UNREACHABLE", "media engine unavailable");
}

std::string MediaIpcClient::query_state() {
    return request("query_state", nullptr);
}

std::string MediaIpcClient::hello() {
    return request("hello", nullptr);
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
