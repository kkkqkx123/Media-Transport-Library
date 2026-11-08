#include "http_json.h"

#include <cctype>
#include <cstdio>
#include <ctime>

#include "dvs_contract.h"

namespace ct = dvs::contract;

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

std::string body_str(json_object* o, const char* key) {
    json_object* v = nullptr;
    if (o && json_object_object_get_ex(o, key, &v) && v)
        return json_object_get_string(v);
    return "";
}

int rect_int(json_object* o, const char* k) {
    json_object* v = nullptr;
    if (o && json_object_object_get_ex(o, k, &v) && v)
        return json_object_get_int(v);
    return 0;
}

std::string now_iso() {
    char b[32];
    time_t t = time(nullptr);
    strftime(b, sizeof(b), "%FT%TZ", gmtime(&t));
    return b;
}

int64_t now_sec() { return (int64_t)time(nullptr); }

uint64_t now_ns() {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

std::string idempotency_key(const HttpRequest& req) {
    for (auto& h : req.headers) {
        std::string k = h.first;
        for (auto& c : k) c = (char)tolower((unsigned char)c);
        if (k == "idempotency-key" && !h.second.empty()) return h.second;
    }
    return "";
}

json_object* ok_envelope(json_object* data, const std::string& msg) {
    return ct::make_ok(data, msg, "p");
}

json_object* fail_envelope(const char* code, int status, const std::string& msg) {
    return ct::make_fail(code, status, msg, "p");
}

void send_json(HttpResponse& res, json_object* o) {
    const char* s = json_object_to_json_string(o);
    res.body = s ? s : "{}";
    res.content_type = "application/json; charset=utf-8";
    json_object_put(o);
}

bool http_bearer_ok(const HttpRequest& req, const std::string& token) {
    return ct::bearer_ok(ct::extract_bearer(req.headers), token);
}

bool require_bearer(const HttpRequest& req, HttpResponse& res,
                    const std::string& token) {
    if (http_bearer_ok(req, token)) return true;
    send_json(res, fail_envelope("UNAUTHORIZED", 401, "missing or bad token"));
    return false;
}
