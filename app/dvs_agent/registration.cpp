#include "registration.h"

#include "node_store.h"
#include "media_ipc.h"
#include "http_client.h"

#include <json-c/json.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>

static std::string str_of(json_object* o, const char* key, const char* def = "") {
    json_object* v = nullptr;
    if (o && json_object_object_get_ex(o, key, &v) && v) return json_object_get_string(v);
    return def;
}

static int int_of(json_object* o, const char* key, int def = 0) {
    json_object* v = nullptr;
    if (o && json_object_object_get_ex(o, key, &v) && v) return json_object_get_int(v);
    return def;
}

/* Read a single integer from a sysfs/proc file. Returns def on any failure. */
static long read_long_file(const char* path, long def) {
    std::ifstream f(path);
    if (!f) return def;
    long v = def;
    f >> v;
    return v;
}

Registration::Registration(NodeStore* store, MediaIpcClient* media,
                           const std::string& platform_url, int interval_ms)
    : store_(store), media_(media), platform_url_(platform_url),
      interval_ms_(interval_ms), register_state_("unregistered"),
      start_time_(time(nullptr)) {}

Registration::~Registration() { stop(); }

void Registration::start() {
    if (running_) return;
    running_ = true;
    thread_ = std::thread(&Registration::loop, this);
}

void Registration::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void Registration::loop() {
    if (platform_url_.empty()) {
        /* Local autonomous mode: stay online, no egress. */
        register_state_ = "unregistered";
        online_ = true;
        while (running_) {
            std::this_thread::sleep_for(std::chrono::seconds(3));
            /* touch lastHeartbeat locally so the admin page shows liveness */
            json_object* patch = json_object_new_object();
            json_object_object_add(patch, "lastHeartbeat", json_object_new_string("now"));
            store_->update_node(patch);
            json_object_put(patch);
        }
        return;
    }

    register_state_ = "registering";
    int backoff = 2;
    while (running_) {
        if (register_state_ != "registered") {
            if (register_once()) {
                register_state_ = "registered";
                backoff = 2;
            } else {
                std::this_thread::sleep_for(std::chrono::seconds(backoff));
                if (backoff < 30) backoff *= 2;
                continue;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms_));
        if (!heartbeat_once()) {
            fail_streak_++;
            if (fail_streak_ >= 3) {
                online_ = false;
                json_object* patch = json_object_new_object();
                json_object_object_add(patch, "online", json_object_new_boolean(0));
                store_->update_node(patch);
                json_object_put(patch);
            }
        } else {
            if (!online_) {
                online_ = true;
                json_object* patch = json_object_new_object();
                json_object_object_add(patch, "online", json_object_new_boolean(1));
                store_->update_node(patch);
                json_object_put(patch);
            }
            fail_streak_ = 0;
        }
    }
}

bool Registration::register_once() {
    HttpClient cli;
    json_object* payload = build_register();
    const char* body = json_object_to_json_string(payload);
    std::string resp;
    int status = cli.post_json(platform_url_ + "/api/v1/nodes/register", body, resp, 5000);
    json_object_put(payload);
    if (status < 0 || status >= 400) return false;

    json_object* ack = json_tokener_parse(resp.c_str());
    if (!ack) return false;
    json_object* data = nullptr;
    if (json_object_object_get_ex(ack, "data", &data) && data) {
        json_object* nid = nullptr;
        if (json_object_object_get_ex(data, "nodeId", &nid) && nid)
            store_->set_node_id(json_object_get_string(nid));
        json_object* iv = nullptr;
        if (json_object_object_get_ex(data, "heartbeatIntervalMs", &iv) && iv) {
            int ivv = json_object_get_int(iv);
            if (ivv > 0) interval_ms_ = ivv;
        }
    }
    json_object_put(ack);

    json_object* patch = json_object_new_object();
    json_object_object_add(patch, "registerState", json_object_new_string("registered"));
    json_object_object_add(patch, "online", json_object_new_boolean(1));
    store_->update_node(patch);
    json_object_put(patch);
    return true;
}

bool Registration::heartbeat_once() {
    HttpClient cli;
    json_object* payload = build_heartbeat();
    const char* body = json_object_to_json_string(payload);
    std::string resp;
    int status = cli.post_json(platform_url_ + "/api/v1/nodes/heartbeat", body, resp, interval_ms_ + 1000);
    json_object_put(payload);
    if (status < 0 || status >= 400) return false;

    /* Reflect appliedRevision from ack if present; keep it simple and just
     * update lastHeartbeat locally. */
    json_object* patch = json_object_new_object();
    json_object_object_add(patch, "lastHeartbeat", json_object_new_string("now"));
    store_->update_node(patch);
    json_object_put(patch);
    return true;
}

json_object* Registration::build_register() {
    json_object* node = store_->node();
    json_object* o = json_object_new_object();
    json_object_object_add(o, "sn", json_object_new_string(str_of(node, "sn", "RK3588-LOCAL").c_str()));
    json_object_object_add(o, "mac", json_object_new_string(str_of(node, "mac").c_str()));
    json_object_object_add(o, "model", json_object_new_string(str_of(node, "model", "DVS-NODE-R1").c_str()));
    json_object_object_add(o, "firmware", json_object_new_string(str_of(node, "fw", "V1.0.0").c_str()));
    json_object_object_add(o, "role", json_object_new_string(str_of(node, "role", "output").c_str()));

    json_object* cap = nullptr;
    json_object_object_get_ex(node, "capabilities", &cap);
    json_object_object_add(o, "capability", cap ? json_object_get(cap) : json_object_new_object());

    json_object* net = nullptr;
    json_object_object_get_ex(node, "network", &net);
    json_object* net_out = net ? json_object_get(net) : json_object_new_object();
    if (net == nullptr) {
        json_object_object_add(net_out, "ip", json_object_new_string(str_of(node, "ip", "127.0.0.1").c_str()));
    }
    json_object_object_add(o, "network", net_out);

    json_object* ptp = json_object_new_object();
    json_object_object_add(ptp, "state", json_object_new_string("unknown"));
    json_object_object_add(ptp, "offsetUs", json_object_new_int(0));
    json_object_object_add(o, "ptp", ptp);
    return o;
}

json_object* Registration::build_heartbeat() {
    json_object* node = store_->node();
    json_object* o = json_object_new_object();
    json_object_object_add(o, "nodeId", json_object_new_string(str_of(node, "id", "local-node").c_str()));
    json_object_object_add(o, "uptimeSec", json_object_new_int((int)(time(nullptr) - start_time_)));
    json_object_object_add(o, "role", json_object_new_string(str_of(node, "role", "output").c_str()));
    json_object_object_add(o, "firmware", json_object_new_string(str_of(node, "fw", "V1.0.0").c_str()));

    /* appliedRevision: max applied across the six scopes */
    int applied = 0;
    json_object* cr = nullptr;
    if (json_object_object_get_ex(node, "configRevision", &cr) && cr) {
        for (int i = 0; i < 6; i++) {
            json_object* s = nullptr;
            if (json_object_object_get_ex(cr, NodeStore::SCOPES[i], &s) && s) {
                json_object* a = nullptr;
                if (json_object_object_get_ex(s, "applied", &a) && a)
                    applied = std::max(applied, json_object_get_int(a));
            }
        }
    }
    json_object_object_add(o, "appliedRevision", json_object_new_int(applied));

    /* media stats + ptp from engine query_state (best effort) */
    json_object* media = json_object_new_object();
    json_object* ptp = json_object_new_object();
    std::string qs = media_->query_state();
    json_object* st = json_tokener_parse(qs.c_str());
    if (st) {
        json_object* d = nullptr;
        if (json_object_object_get_ex(st, "data", &d) && d) {
            json_object_object_add(media, "rxSessions", json_object_new_int(int_of(d, "rxSessions")));
            json_object_object_add(media, "decoding", json_object_new_int(int_of(d, "decoding")));
            json_object_object_add(media, "layers", json_object_new_int(int_of(d, "layers")));
            json_object* p = nullptr;
            if (json_object_object_get_ex(d, "ptp", &p) && p) {
                json_object_object_add(ptp, "state", json_object_new_string(str_of(p, "state", "unknown").c_str()));
                json_object_object_add(ptp, "offsetUs", json_object_new_int(int_of(p, "offsetUs")));
            }
        }
        json_object_put(st);
    } else {
        json_object_object_add(media, "rxSessions", json_object_new_int(0));
        json_object_object_add(media, "decoding", json_object_new_int(0));
        json_object_object_add(media, "layers", json_object_new_int(0));
        json_object_object_add(ptp, "state", json_object_new_string("unknown"));
        json_object_object_add(ptp, "offsetUs", json_object_new_int(0));
    }
    json_object_object_add(o, "media", media);
    json_object_object_add(o, "ptp", ptp);

    /* resources (best effort) */
    json_object* res = json_object_new_object();
    long mem_kb = read_long_file("/proc/meminfo", 0);
    /* MemAvailable is the 3rd field on a line; a crude read suffices. */
    json_object_object_add(res, "cpu", json_object_new_int(0));
    json_object_object_add(res, "memMb", json_object_new_int((int)(mem_kb / 1024)));
    long temp = read_long_file("/sys/class/thermal/thermal_zone0/temp", 0);
    json_object_object_add(res, "tempC", json_object_new_int((int)(temp / 1000)));
    json_object_object_add(o, "resources", res);

    return o;
}
