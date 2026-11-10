#include "dvs_http_json.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>

#include "dvs_contract.h"

namespace dvs {

std::string json_get_str(json_object* o, const char* key) {
    json_object* v = nullptr;
    if (o && json_object_object_get_ex(o, key, &v) && v)
        return json_object_get_string(v);
    return "";
}

std::string json_escape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char b[8];
                snprintf(b, sizeof(b), "\\u%04x", c);
                out += b;
            } else {
                out += (char)c;
            }
        }
    }
    return out;
}

int json_rect_int(json_object* o, const char* k) {
    json_object* v = nullptr;
    if (o && json_object_object_get_ex(o, k, &v) && v)
        return json_object_get_int(v);
    return 0;
}

std::string iso_now() {
    char buf[32];
    time_t t = time(nullptr);
    struct tm tmv{};
    gmtime_r(&t, &tmv);
    strftime(buf, sizeof(buf), "%FT%TZ", &tmv);
    return buf;
}

int64_t wall_sec_now() { return (int64_t)time(nullptr); }

uint64_t wall_ns_now() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

long long steady_ms_now() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string env_or(const char* k, const char* d) {
    const char* v = std::getenv(k);
    return v ? std::string(v) : std::string(d ? d : "");
}

json_object* envelope_ok(json_object* data, const std::string& msg,
                         const char* origin) {
    return contract::make_ok(data, msg, origin ? origin : "p");
}

json_object* envelope_fail(const char* code, int status,
                           const std::string& msg, const char* origin) {
    return contract::make_fail(code, status, msg, origin ? origin : "p");
}

void send_json(HttpResponse& res, json_object* o) {
    const char* s = json_object_to_json_string(o);
    res.body = s ? s : "{}";
    res.content_type = "application/json; charset=utf-8";
    json_object_put(o);
}

bool http_bearer_ok(const HttpRequest& req, const std::string& token) {
    auto it = req.headers.find("Authorization");
    std::string provided = (it == req.headers.end())
                               ? ""
                               : contract::strip_bearer(it->second);
    if (provided.empty())
        provided = contract::extract_bearer(
            const_cast<std::map<std::string, std::string>&>(req.headers));
    return contract::bearer_ok(provided, token);
}

bool require_bearer(const HttpRequest& req, HttpResponse& res,
                    const std::string& token, const char* origin) {
    if (http_bearer_ok(req, token)) return true;
    send_json(res, envelope_fail("UNAUTHORIZED", 401, "missing or bad token",
                                 origin ? origin : "p"));
    return false;
}

std::string idempotency_key(const HttpRequest& req) {
    for (auto& h : req.headers) {
        std::string k = h.first;
        for (auto& c : k) c = (char)tolower((unsigned char)c);
        if (k == "idempotency-key" && !h.second.empty()) return h.second;
    }
    return "";
}

std::string url_join(const std::string& base, const std::string& path) {
    std::string b = base;
    while (b.size() > 1 && b.back() == '/') b.pop_back();
    if (path.empty()) return b;
    if (path.front() == '/') return b + path;
    return b + "/" + path;
}

}  // namespace dvs
