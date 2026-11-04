#include <json-c/json.h>
#include <sqlite3.h>

#include "node_store.h"
#include "media_ipc.h"
#include "dvs_node_bundle.h"
#include "http_server.h"
#include "maintenance.h"
#include "registration.h"
#include "http_client.h"

#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <string>
#include <thread>
#include <chrono>
#include <fstream>
#include <sstream>
#include <map>
#include <mutex>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/stat.h>

static NodeStore* g_store = nullptr;
static MediaIpcClient* g_media = nullptr;
static Maintenance* g_maint = nullptr;
static Registration* g_reg = nullptr;
static std::string g_web_root;
// Shared control-plane bearer token. Empty means no check; when set,
// platform-to-agent calls must carry it in an Authorization header.
static std::string g_api_token =
    (std::getenv("DVS_API_TOKEN") ? std::getenv("DVS_API_TOKEN") : "");

/* Per-source login rate limiter: 5 attempts / 60s window, then locked out. */
struct LoginLimiter {
    std::mutex mu;
    std::map<std::string, std::pair<int, time_t>> attempts;  // key -> (count, windowStart)
    bool allow(const std::string& key) {
        std::lock_guard<std::mutex> lk(mu);
        time_t now = time(nullptr);
        auto& e = attempts[key];
        if (now - e.second > 60) { e.first = 0; e.second = now; }
        if (e.first >= 5) return false;
        e.first++;
        return true;
    }
    void clear(const std::string& key) {
        std::lock_guard<std::mutex> lk(mu);
        attempts.erase(key);
    }
};
static LoginLimiter g_limiter;

/* Issued browser sessions with expiry. Random hex tokens replace the fixed
 * prototype value; logout invalidates. All access under the internal mutex. */
struct SessionStore {
    std::mutex mu;
    std::map<std::string, time_t> exp_;
    std::string issue() {
        uint8_t rnd[16] = {0};
        int fd = open("/dev/urandom", O_RDONLY);
        if (fd >= 0) {
            size_t got = 0;
            while (got < sizeof(rnd)) {
                ssize_t n = read(fd, rnd + got, sizeof(rnd) - got);
                if (n <= 0) break;
                got += (size_t)n;
            }
            close(fd);
        }
        char hex[33];
        for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", rnd[i]);
        std::string tok(hex, 32);
        std::lock_guard<std::mutex> lk(mu);
        exp_[tok] = time(nullptr) + 3600;
        return tok;
    }
    bool valid(const std::string& tok) {
        if (tok.empty()) return false;
        std::lock_guard<std::mutex> lk(mu);
        auto it = exp_.find(tok);
        if (it == exp_.end()) return false;
        if (time(nullptr) > it->second) { exp_.erase(it); return false; }
        return true;
    }
    void invalidate(const std::string& tok) {
        std::lock_guard<std::mutex> lk(mu);
        exp_.erase(tok);
    }
};
static SessionStore g_sessions;

static std::string session_from_cookie(const std::map<std::string, std::string>& headers) {
    auto it = headers.find("Cookie");
    if (it == headers.end()) return "";
    const std::string& c = it->second;
    size_t p = c.find("session=");
    if (p == std::string::npos) return "";
    size_t s = p + 8;
    size_t e = c.find(';', s);
    std::string v = c.substr(s, e == std::string::npos ? std::string::npos : e - s);
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) v.erase(0, 1);
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) v.pop_back();
    return v;
}

static bool valid_ip4(const std::string& s) {
    if (s.empty() || s.size() > 15) return false;
    int parts = 0;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); i++) {
        if (i == s.size() || s[i] == '.') {
            if (i == start || i - start > 3) return false;
            for (size_t j = start; j < i; j++) if (s[j] < '0' || s[j] > '9') return false;
            int v = atoi(s.substr(start, i - start).c_str());
            if (v < 0 || v > 255) return false;
            parts++;
            start = i + 1;
        }
    }
    return parts == 4;
}

static std::string trim_str(const std::string& s) {
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t' || s[a] == '\r' || s[a] == '\n')) a++;
    size_t b = s.size();
    while (b > a && (s[b-1] == ' ' || s[b-1] == '\t' || s[b-1] == '\r' || s[b-1] == '\n')) b--;
    return s.substr(a, b - a);
}

/* Single whitelist item: "ip" or "ip:port". */
static bool parse_endpoint_item(const std::string& raw, std::string& ip_out,
                                int& port_out, bool& has_port_out) {
    std::string t = trim_str(raw);
    if (t.empty()) return false;
    size_t colon = t.find(':');
    if (colon == std::string::npos) {
        if (!valid_ip4(t)) return false;
        ip_out = t; port_out = 0; has_port_out = false;
        return true;
    }
    if (t.find(':', colon + 1) != std::string::npos) return false;
    std::string ip = trim_str(t.substr(0, colon));
    std::string ps = trim_str(t.substr(colon + 1));
    if (!valid_ip4(ip) || ps.empty() || ps.size() > 5) return false;
    for (char c : ps) if (c < '0' || c > '9') return false;
    int p = atoi(ps.c_str());
    if (p < 1 || p > 65535) return false;
    ip_out = ip; port_out = p; has_port_out = true;
    return true;
}

/* Strict per-item whitelist validation. Accepts comma string, array of
 * string/object, or {whitelist|sources: ...} nesting. */
static bool validate_whitelist_value(json_object* wv, std::string& bad_item) {
    if (!wv) return true;
    if (json_object_is_type(wv, json_type_string)) {
        std::string s = json_object_get_string(wv);
        bool any = false;
        size_t start = 0;
        for (size_t i = 0; i <= s.size(); i++) {
            if (i == s.size() || s[i] == ',') {
                std::string tok = trim_str(s.substr(start, i - start));
                start = i + 1;
                if (tok.empty()) continue;
                any = true;
                std::string ip; int port = 0; bool hp = false;
                if (!parse_endpoint_item(tok, ip, port, hp)) { bad_item = tok; return false; }
            }
        }
        if (!any) { bad_item = s; return false; }
        return true;
    }
    if (json_object_is_type(wv, json_type_array)) {
        int n = json_object_array_length(wv);
        if (n <= 0) { bad_item = "empty whitelist"; return false; }
        for (int i = 0; i < n; i++) {
            json_object* e = json_object_array_get_idx(wv, i);
            if (!e) { bad_item = "null entry"; return false; }
            if (json_object_is_type(e, json_type_string)) {
                std::string s = json_object_get_string(e);
                /* Allow comma string inside an array element. */
                size_t start = 0;
                bool any = false;
                for (size_t k = 0; k <= s.size(); k++) {
                    if (k == s.size() || s[k] == ',') {
                        std::string tok = trim_str(s.substr(start, k - start));
                        start = k + 1;
                        if (tok.empty()) continue;
                        any = true;
                        std::string ip; int port = 0; bool hp = false;
                        if (!parse_endpoint_item(tok, ip, port, hp)) { bad_item = tok; return false; }
                    }
                }
                if (!any) { bad_item = s; return false; }
            } else if (json_object_is_type(e, json_type_object)) {
                json_object* iv = nullptr;
                std::string ip;
                if (json_object_object_get_ex(e, "sip", &iv) && iv) ip = json_object_get_string(iv);
                else if (json_object_object_get_ex(e, "ip", &iv) && iv) ip = json_object_get_string(iv);
                else if (json_object_object_get_ex(e, "source", &iv) && iv) ip = json_object_get_string(iv);
                ip = trim_str(ip);
                json_object* pv = nullptr;
                bool has_p = false; int p = 0;
                if ((json_object_object_get_ex(e, "udp_port", &pv) ||
                     json_object_object_get_ex(e, "udpPort", &pv) ||
                     json_object_object_get_ex(e, "port", &pv)) && pv) {
                    /* json-c ints come back as int; strings are also accepted. */
                    if (json_object_is_type(pv, json_type_string)) {
                        std::string ps = trim_str(json_object_get_string(pv));
                        if (ps.empty()) return false;
                        for (char c : ps) if (c < '0' || c > '9') { bad_item = ip + ":" + ps; return false; }
                        p = atoi(ps.c_str());
                    } else {
                        p = json_object_get_int(pv);
                    }
                    has_p = true;
                }
                if (!valid_ip4(ip)) { bad_item = ip.empty() ? "missing ip" : ip; return false; }
                if (has_p && (p < 1 || p > 65535)) { bad_item = ip + ":" + std::to_string(p); return false; }
            } else { bad_item = "bad entry type"; return false; }
        }
        return true;
    }
    if (json_object_is_type(wv, json_type_object)) {
        json_object* inner = nullptr;
        if (json_object_object_get_ex(wv, "whitelist", &inner) && inner)
            return validate_whitelist_value(inner, bad_item);
        if (json_object_object_get_ex(wv, "sources", &inner) && inner)
            return validate_whitelist_value(inner, bad_item);
        bad_item = "bad whitelist object";
        return false;
    }
    bad_item = "bad whitelist value";
    return false;
}

/* NET-1 system apply (env-gated). Without DVS_ALLOW_NETWORK_APPLY the call
 * is a dry-run that only persists intent to /tmp; with the env set it also
 * persists to /var/lib and reports IO failures. DVS_FORCE_NETWORK_FAIL=1
 * forces failure for rollback testing. */
static bool apply_system_network(const std::string& mode, const std::string& ip,
                                 const std::string& mask, const std::string& gw,
                                 const std::string& dns, std::string& err) {
    if (std::getenv("DVS_FORCE_NETWORK_FAIL") &&
        std::string(std::getenv("DVS_FORCE_NETWORK_FAIL")) == "1") {
        err = "injected failure (DVS_FORCE_NETWORK_FAIL=1)";
        return false;
    }
    const char* allow = std::getenv("DVS_ALLOW_NETWORK_APPLY");
    std::string intent = std::string("{\"mode\":\"") + mode +
        "\",\"ip\":\"" + ip + "\",\"mask\":\"" + mask +
        "\",\"gateway\":\"" + gw + "\",\"dns\":\"" + dns + "\"}";
    if (!allow) {
        fprintf(stderr, "dvs-agent: network dry-run (set DVS_ALLOW_NETWORK_APPLY=1 to apply): %s\n",
                intent.c_str());
        return true;
    }
    {
        std::string dir = "/var/lib/dvs_agent";
        std::string cmd = std::string("mkdir -p ") + dir + " 2>/dev/null";
        if (system(cmd.c_str()) != 0) {
            err = "cannot create state dir";
            return false;
        }
        std::ofstream f(dir + "/network_applied.json", std::ios::binary | std::ios::trunc);
        if (!f) { err = "cannot persist network intent"; return false; }
        f << intent;
        f.close();
        if (!f) { err = "cannot persist network intent"; return false; }
    }
    fprintf(stderr, "dvs-agent: network applied: %s\n", intent.c_str());
    return true;
}

/* CAP-1: pull engine capability once and merge into node.capabilities.
 * Same merge rule as GET /capability: empty input/output arrays never
 * overwrite the seeded ports; mtl/ptp/scalars always merge. Log-only. */
static void refresh_capability_from_media() {
    if (!g_media || !g_store) return;
    std::string cr = g_media->request("capability", nullptr);
    json_object* co = json_tokener_parse(cr.c_str());
    if (!co) { fprintf(stderr, "dvs-agent: startup capability pull failed (unparsable)\n"); return; }
    json_object* okv = nullptr;
    json_object* data = nullptr;
    if (json_object_object_get_ex(co, "ok", &okv) && okv && json_object_get_boolean(okv) &&
        json_object_object_get_ex(co, "data", &data) && data) {
        json_object* patch = json_object_new_object();
        json_object* caps = nullptr;
        json_object_object_get_ex(g_store->node(), "capabilities", &caps);
        json_object* merged = caps ? json_object_get(caps) : json_object_new_object();
        json_object_object_foreach(data, k, v) {
            if ((strcmp(k, "inputPorts") == 0 || strcmp(k, "outputPorts") == 0) &&
                json_object_is_type(v, json_type_array) && json_object_array_length(v) == 0)
                continue;
            json_object_object_add(merged, k, json_object_get(v));
        }
        json_object_object_add(patch, "capabilities", merged);
        g_store->update_node(patch);
        json_object_put(patch);
        fprintf(stderr, "dvs-agent: startup capability merged\n");
    } else {
        fprintf(stderr, "dvs-agent: startup capability pull returned error\n");
    }
    json_object_put(co);
}

static bool tcp_reachable(const std::string& host, int port, int timeout_ms) {
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char ps[16];
    snprintf(ps, sizeof(ps), "%d", port);
    struct addrinfo* ai = nullptr;
    if (getaddrinfo(host.c_str(), ps, &hints, &ai) != 0) return false;
    bool ok = false;
    for (struct addrinfo* p = ai; p; p = p->ai_next) {
        int fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0) continue;
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        int rc = connect(fd, p->ai_addr, p->ai_addrlen);
        if (rc == 0) { ok = true; close(fd); break; }
        if (errno == EINPROGRESS) {
            struct pollfd pfd{.fd = fd, .events = POLLOUT};
            if (poll(&pfd, 1, timeout_ms) > 0 && (pfd.revents & POLLOUT)) {
                int err = 0;
                socklen_t n = sizeof(err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &n) == 0 && err == 0) ok = true;
            }
        }
        close(fd);
        if (ok) break;
    }
    freeaddrinfo(ai);
    return ok;
}

static std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string guess_ct(const std::string& path) {
    if (path.find(".html") != std::string::npos) return "text/html; charset=utf-8";
    if (path.find(".css") != std::string::npos) return "text/css; charset=utf-8";
    if (path.find(".js") != std::string::npos) return "text/javascript; charset=utf-8";
    if (path.find(".json") != std::string::npos) return "application/json; charset=utf-8";
    if (path.find(".svg") != std::string::npos) return "image/svg+xml";
    if (path.find(".png") != std::string::npos) return "image/png";
    return "application/octet-stream";
}

static std::string env_or(const char* k, const char* d) {
    const char* v = std::getenv(k);
    return v ? std::string(v) : std::string(d ? d : "");
}

/* Node capacity self-report (S5): CPU busy ratio (since boot), memory usage,
 * load average and uptime, read from /proc. No external deps. */
static json_object* node_capacity_json() {
    json_object* o = json_object_new_object();

    /* memory from /proc/meminfo */
    long long mem_total = 0, mem_avail = 0;
    {
        std::ifstream f("/proc/meminfo");
        std::string line;
        while (std::getline(f, line)) {
            char key[64]; long long val = 0;
            if (sscanf(line.c_str(), "%63[^:]: %lld", key, &val) == 2) {
                if (strcmp(key, "MemTotal") == 0) mem_total = val;
                else if (strcmp(key, "MemAvailable") == 0) mem_avail = val;
            }
        }
    }

    /* load average + uptime */
    double l1 = 0, l5 = 0, l15 = 0, uptime = 0;
    {
        std::ifstream f("/proc/loadavg");
        std::string line;
        if (std::getline(f, line)) sscanf(line.c_str(), "%lf %lf %lf", &l1, &l5, &l15);
    }
    {
        std::ifstream f("/proc/uptime");
        double idle = 0;
        if (f >> uptime >> idle) { /* uptime is the first field */ }
    }

    /* cumulative CPU busy ratio (since boot) from /proc/stat cpu line */
    double cpu_pct = 0.0;
    {
        std::ifstream f("/proc/stat");
        std::string line;
        if (std::getline(f, line) && line.rfind("cpu ", 0) == 0) {
            long long u=0,n=0,s=0,idle=0,iow=0,irq=0,sirq=0,steal=0;
            sscanf(line.c_str() + 4, "%lld %lld %lld %lld %lld %lld %lld %lld",
                   &u, &n, &s, &idle, &iow, &irq, &sirq, &steal);
            long long idle_all = idle + iow;
            long long total = u + n + s + idle + iow + irq + sirq + steal;
            if (total > 0) cpu_pct = 100.0 * (double)(total - idle_all) / (double)total;
        }
    }

    json_object_object_add(o, "cpuBusyPercent", json_object_new_double(cpu_pct));
    json_object_object_add(o, "memTotalKB", json_object_new_int64(mem_total));
    json_object_object_add(o, "memAvailableKB", json_object_new_int64(mem_avail));
    int mem_used_pct = mem_total > 0
        ? (int)(100.0 * (double)(mem_total - mem_avail) / (double)mem_total) : 0;
    json_object_object_add(o, "memUsedPercent", json_object_new_int(mem_used_pct));
    json_object_object_add(o, "load1", json_object_new_double(l1));
    json_object_object_add(o, "load5", json_object_new_double(l5));
    json_object_object_add(o, "load15", json_object_new_double(l15));
    json_object_object_add(o, "uptimeSec", json_object_new_int64((long long)uptime));
    return o;
}

static json_object* envelope_ok(json_object* data, const char* msg) {
    json_object* o = json_object_new_object();
    json_object_object_add(o, "ok", json_object_new_boolean(1));
    json_object_object_add(o, "status", json_object_new_int(200));
    json_object_object_add(o, "code", json_object_new_string("OK"));
    json_object_object_add(o, "msg", json_object_new_string(msg ? msg : ""));
    json_object_object_add(o, "data", data ? data : json_object_new_object());
    json_object_object_add(o, "requestId", json_object_new_string("r-1"));
    return o;
}

static json_object* envelope_fail(const char* code, int status, const char* msg) {
    json_object* o = json_object_new_object();
    json_object_object_add(o, "ok", json_object_new_boolean(0));
    json_object_object_add(o, "status", json_object_new_int(status));
    json_object_object_add(o, "code", json_object_new_string(code));
    json_object_object_add(o, "msg", json_object_new_string(msg ? msg : code));
    json_object_object_add(o, "data", json_object_new_object());
    json_object_object_add(o, "requestId", json_object_new_string("r-1"));
    return o;
}

static void send_json(HttpResponse& res, json_object* o) {
    const char* s = json_object_to_json_string(o);
    res.body = s ? s : "{}";
    res.content_type = "application/json; charset=utf-8";
    json_object_put(o);
}

static std::string get_str(json_object* o, const char* key) {
    json_object* v = nullptr;
    if (o && json_object_object_get_ex(o, key, &v) && v) return json_object_get_string(v);
    return "";
}

/* Background reporter. After the agent forwards a platform sub-layout to the
 * media engine (prepare IPC), the engine emits a prepare-result event once
 * it has readied its RX sessions. This thread watches the IPC event ring,
 * finds the matching operation, and calls the platform commit endpoint so
 * the barrier can collect this node's reply and switch at the target time.
 * Runs detached; never blocks the HTTP handler. Only ready/blocked leave
 * this node; anything else (including timeouts) is reported as blocked. */
static void agent_report_commit(const std::string& opid, int rev,
                                int64_t applyAtNs, const std::string& nodeId,
                                const std::string& platform_url) {
    (void)rev; (void)applyAtNs;  // reserved for barrier diagnostics
    if (platform_url.empty()) return;  // local-autonomous: nothing to report
    std::string status = "ready";
    std::string lanes_json = "[]";
    bool done = false;
    for (int i = 0; i < 50 && !done; i++) {  // up to 5s
        json_object* evs = g_media->drain_events();
        int n = json_object_array_length(evs);
        for (int j = 0; j < n; j++) {
            json_object* e = json_object_array_get_idx(evs, j);
            std::string t = get_str(e, "type");
            if (t == "media.prepare.result") {
                json_object* dd = nullptr;
                if (json_object_object_get_ex(e, "data", &dd) && dd) {
                    if (get_str(dd, "operationId") == opid) {
                        std::string s = get_str(dd, "status");
                        status = (s == "ready") ? "ready" : "blocked";
                        json_object* lj = nullptr;
                        if (json_object_object_get_ex(dd, "lanes", &lj) && lj) {
                            const char* ls = json_object_to_json_string(lj);
                            if (ls) lanes_json = ls;
                        }
                        done = true;
                    }
                }
            }
        }
        json_object_put(evs);
        if (!done) std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    /* fallback: if the event was missed (e.g. drained elsewhere), probe state */
    if (!done) {
        std::string qs = g_media->query_state();
        json_object* st = json_tokener_parse(qs.c_str());
        if (st) {
            json_object* d = nullptr;
            if (json_object_object_get_ex(st, "data", &d) && d) {
                std::string s = get_str(d, "state");
                status = (s == "prepared" || s == "idle") ? "ready" : "blocked";
                json_object* lj = nullptr;
                if (json_object_object_get_ex(d, "lanes", &lj) && lj) {
                    const char* ls = json_object_to_json_string(lj);
                    if (ls) lanes_json = ls;
                }
            } else {
                status = "blocked";
            }
            json_object_put(st);
        } else {
            status = "blocked";
        }
    }
    HttpClient cli;
    json_object* c = json_object_new_object();
    json_object_object_add(c, "operationId", json_object_new_string(opid.c_str()));
    json_object_object_add(c, "node", json_object_new_string(nodeId.c_str()));
    json_object_object_add(c, "status", json_object_new_string(status.c_str()));
    if (lanes_json != "[]") {
        json_object* lj = json_tokener_parse(lanes_json.c_str());
        if (lj) json_object_object_add(c, "lanes", lj);
    }
    const char* cb = json_object_to_json_string(c);
    std::string resp;
    cli.post_json(platform_url + "/api/v1/ops/commit",
                  std::string(cb ? cb : "{}"), resp, 5000, 2, g_api_token);
    json_object_put(c);
}

int main(int argc, char** argv) {
    std::string db_path = "/tmp/dvs_agent.db";
    std::string platform_url;
    std::string media_sock;
    int port = 8080;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--version") {
            printf("dvs-agent bundle %s ipc %d\n", DVS_NODE_BUNDLE_VERSION,
                   DVS_MEDIA_IPC_VERSION);
            return 0;
        }
        if (a == "--db" && i + 1 < argc) db_path = argv[++i];
        else if (a == "--port" && i + 1 < argc) port = std::stoi(argv[++i]);
        else if (a == "--platform-url" && i + 1 < argc) platform_url = argv[++i];
        else if (a == "--media-sock" && i + 1 < argc) media_sock = argv[++i];
        else if (a == "--web-root" && i + 1 < argc) g_web_root = argv[++i];
    }
    if (g_web_root.empty()) g_web_root = env_or("DVS_WEB_ROOT", "./web/src");
    if (media_sock.empty()) media_sock = env_or("DVS_MEDIA_SOCK", "");
    if (platform_url.empty()) platform_url = env_or("DVS_PLATFORM_URL", "");

    g_store = new NodeStore(db_path);
    g_media = new MediaIpcClient(media_sock);
    g_maint = new Maintenance(g_store, g_media);

    // Bundle handshake: confirm the local media engine is a co-released
    // peer. Warn-only; the engine may simply start later than the agent.
    {
        std::string hr = g_media->hello();
        json_object* ho = json_tokener_parse(hr.c_str());
        bool match = false;
        if (ho) {
            json_object* okv = nullptr;
            json_object* data = nullptr;
            if (json_object_object_get_ex(ho, "ok", &okv) && okv &&
                json_object_get_boolean(okv) &&
                json_object_object_get_ex(ho, "data", &data) && data) {
                json_object* iv = nullptr;
                json_object* bv = nullptr;
                json_object_object_get_ex(data, "ipcVersion", &iv);
                json_object_object_get_ex(data, "bundleVersion", &bv);
                match = iv && json_object_get_int(iv) == DVS_MEDIA_IPC_VERSION &&
                        bv && std::string(json_object_get_string(bv)) ==
                                  DVS_NODE_BUNDLE_VERSION;
            }
            json_object_put(ho);
        }
        if (match)
            printf("dvs-agent: media engine bundle %s ipc %d confirmed\n",
                   DVS_NODE_BUNDLE_VERSION, DVS_MEDIA_IPC_VERSION);
        else
            printf("dvs-agent: WARNING media engine handshake mismatch "
                   "(see /api/v1/node/version)\n");
        fflush(stdout);
    }

    // Derive this agent's externally reachable base URL. Operator may
    // override via --agent-base / DVS_AGENT_BASE; otherwise auto-build from the
    // node's network.ip and the listen port so the platform can fan out to it.
    std::string agent_base = env_or("DVS_AGENT_BASE", "");
    if (agent_base.empty()) {
        std::string ip = "127.0.0.1";
        json_object* nd = g_store->node();
        json_object* net = nullptr;
        if (nd && json_object_object_get_ex(nd, "network", &net) && net) {
            const char* nip = nullptr;
            json_object* n2 = nullptr;
            if (json_object_object_get_ex(net, "ip", &n2) && n2) nip = json_object_get_string(n2);
            if (nip) ip = nip;
        }
        agent_base = "http://" + ip + ":" + std::to_string(port);
    }
    g_reg = new Registration(g_store, g_media, platform_url, 3000, agent_base);

    /* CAP-1: actively merge engine capability at startup (MTL/PTP/ports). */
    refresh_capability_from_media();

    // sync configStates into node at startup
    json_object* cs = g_store->all_states();
    json_object* patch = json_object_new_object();
    json_object_object_add(patch, "configStates", cs);
    json_object* cr = g_store->config_revision();
    json_object_object_add(patch, "configRevision", cr);
    g_store->update_node(patch);
    json_object_put(patch);

    HttpServer::Handler handler = [&](const HttpRequest& req, HttpResponse& res) {
        // login
        if (req.method == "POST" && req.path == "/api/v1/node/login") {
            json_object* body = json_tokener_parse(req.body.c_str());
            std::string u = get_str(body, "username");
            std::string p = get_str(body, "password");
            if (body) json_object_put(body);
            if (!g_limiter.allow(u)) {
                send_json(res, envelope_fail("TOO_MANY_REQUESTS", 429, "登录尝试过多,请稍后再试"));
                return;
            }
            if (g_store->verify_password(u, p)) {
                g_limiter.clear(u);
                std::string tok = g_sessions.issue();
                json_object* d = json_object_new_object();
                json_object_object_add(d, "session", json_object_new_string(tok.c_str()));
                json_object_object_add(d, "expiresIn", json_object_new_int(3600));
                res.headers["Set-Cookie"] = "session=" + tok + "; Path=/; HttpOnly";
                json_object* pc = json_object_new_object();
                json_object_object_add(pc, "passwordChangeRequired",
                                      json_object_new_boolean(g_store->password_change_required() ? 1 : 0));
                json_object_object_add(d, "requirement", pc);
                send_json(res, envelope_ok(d, "登录成功"));
            } else {
                send_json(res, envelope_fail("UNAUTHORIZED", 401, "账号或密码错误"));
            }
            return;
        }

        // password change
        if (req.method == "POST" && req.path == "/api/v1/node/password") {
            json_object* body = json_tokener_parse(req.body.c_str());
            std::string user = get_str(body, "username");
            if (user.empty()) user = "admin";  // default account
            std::string oldp = get_str(body, "oldPassword");
            std::string newp = get_str(body, "newPassword");
            if (body) json_object_put(body);
            if (!g_store->verify_password(user, oldp)) {
                send_json(res, envelope_fail("UNAUTHORIZED", 401, "原密码错误"));
                return;
            }
            if (newp.size() < 6) {
                send_json(res, envelope_fail("INVALID_ARGUMENT", 400, "新密码至少 6 位"));
                return;
            }
            g_store->set_password(newp);
            send_json(res, envelope_ok(json_object_new_object(), "密码已修改"));
            return;
        }

        if (req.method == "POST" && req.path == "/api/v1/node/logout") {
            g_sessions.invalidate(session_from_cookie(req.headers));
            res.headers["Set-Cookie"] = "session=; Path=/; Max-Age=0";
            send_json(res, envelope_ok(json_object_new_object(), "已退出"));
            return;
        }

        // auth check (cookie)
        bool authed = g_sessions.valid(session_from_cookie(req.headers));
        // Strict auth is opt-in via DVS_REQUIRE_AUTH=1 (default off) so the
        // node-admin prototype keeps working without a login wall. When enabled,
        // every /api request except login must carry a valid session cookie.
        static bool g_require_auth = (std::getenv("DVS_REQUIRE_AUTH") != nullptr);
        // Browser sessions use the cookie. Internal platform calls use a
        // bearer token so barrier delivery cannot be forged on a shared LAN.
        bool token_ok = false;
        if (!g_api_token.empty()) {
            auto it = req.headers.find("Authorization");
            if (it != req.headers.end() && it->second == ("Bearer " + g_api_token))
                token_ok = true;
        }
        if (g_require_auth && req.path.rfind("/api/", 0) == 0 &&
            req.path != "/api/v1/node/login" && !authed && !token_ok) {
            send_json(res, envelope_fail("UNAUTHORIZED", 401, "未登录或缺少令牌"));
            return;
        }

        if (req.method == "GET" && req.path == "/api/v1/node/info") {
            send_json(res, envelope_ok(json_object_get(g_store->node()), ""));
            return;
        }
        if (req.method == "GET" && req.path == "/api/v1/node/role") {
            json_object* n = g_store->node();
            json_object* d = json_object_new_object();
            json_object* role = nullptr; json_object_object_get_ex(n, "role", &role);
            json_object* pr = nullptr; json_object_object_get_ex(n, "pendingRole", &pr);
            json_object_object_add(d, "role", json_object_get(role));
            json_object_object_add(d, "pendingRole", json_object_get(pr));
            json_object* cr = nullptr; json_object_object_get_ex(n, "configRevision", &cr);
            json_object* rrev = nullptr;
            if (cr) json_object_object_get_ex(cr, "role", &rrev);
            int rev = 1;
            if (rrev) { json_object* dd=nullptr; if (json_object_object_get_ex(rrev,"desired",&dd)) rev=json_object_get_int(dd); }
            json_object_object_add(d, "revision", json_object_new_int(rev));
            send_json(res, envelope_ok(d, ""));
            return;
        }

        // config scope
        {
            // regex manual
            std::string prefix = "/api/v1/node/config/";
            if (req.path.rfind(prefix, 0) == 0) {
                std::string rest = req.path.substr(prefix.size());
                std::string scope = rest;
                std::string action;
                size_t slash = rest.find('/');
                if (slash != std::string::npos) { scope = rest.substr(0, slash); action = rest.substr(slash+1); }
                // validate scope
                bool valid = false;
                for (int i = 0; i < 6; i++) if (scope == NodeStore::SCOPES[i]) valid = true;
                if (!valid) { send_json(res, envelope_fail("INVALID_ARGUMENT", 400, "不支持的配置范围")); return; }
                std::string node_id = req.query.count("nodeId") ? req.query.at("nodeId") : get_str(g_store->node(), "id");
                if (req.method == "GET" && action.empty()) {
                    json_object* cfg = g_store->get_config(scope, node_id);
                    send_json(res, envelope_ok(cfg, ""));
                    return;
                }
                if (req.method == "POST" && action.empty()) {
                    json_object* body = json_tokener_parse(req.body.c_str());
                    json_object* dv = nullptr;
                    if (body) {
                        if (!json_object_object_get_ex(body, "desiredValue", &dv)) json_object_object_get_ex(body, "value", &dv);
                    }
                    json_object* val = dv ? json_object_get(dv) : json_object_new_object();
                    const char* opid = nullptr;
                    std::string opstr;
                    if (body) {
                        json_object* o = nullptr;
                        if (json_object_object_get_ex(body, "operationId", &o) && o) { opstr = json_object_get_string(o); opid = opstr.c_str(); }
                    }
                    std::string cur_role = get_str(g_store->node(), "role");
                    g_store->set_desired(scope, val, opid);
                    std::string fail_code;
                    std::string fail_reason;
                    json_object* node_patch = json_object_new_object();
                    bool forward_media = false;
                    json_object* fwd = nullptr;
                    if (scope == "role") {
                        json_object* r = nullptr;
                        std::string want = (val && json_object_object_get_ex(val, "role", &r) && r) ? json_object_get_string(r) : "";
                        if (want != "input" && want != "output") {
                            fail_code = "INVALID_ARGUMENT";
                            fail_reason = "角色取值非法";
                        } else if (want == cur_role) {
                            json_object_object_add(node_patch, "pendingRole", json_object_new_null());
                            g_store->update_node(node_patch);
                            json_object_put(node_patch);
                            node_patch = json_object_new_object();
                            g_store->mark_applied(scope, 0);
                        } else {
                            json_object_object_add(node_patch, "pendingRole", json_object_new_string(want.c_str()));
                            g_store->update_node(node_patch);
                            json_object_put(node_patch);
                            node_patch = json_object_new_object();
                        }
                    } else if (scope == "input") {
                        json_object* pv = nullptr;
                        std::string port = (val && json_object_object_get_ex(val, "port", &pv) && pv) ? json_object_get_string(pv) : "";
                        bool ok = !port.empty() && port.size() < 32;
                        if (ok) {
                            json_object* caps = nullptr;
                            json_object_object_get_ex(g_store->node(), "capabilities", &caps);
                            json_object* arr = nullptr;
                            if (caps && json_object_object_get_ex(caps, "inputPorts", &arr) && arr &&
                                json_object_is_type(arr, json_type_array) && json_object_array_length(arr) > 0) {
                                ok = false;
                                for (size_t i = 0; i < (size_t)json_object_array_length(arr); i++) {
                                    json_object* e = json_object_array_get_idx(arr, (int)i);
                                    json_object* idv = nullptr;
                                    if (e && json_object_object_get_ex(e, "id", &idv) && idv &&
                                        port == json_object_get_string(idv)) { ok = true; break; }
                                }
                                if (!ok) { fail_code = "INVALID_ARGUMENT"; fail_reason = "采集口超出本机能力"; }
                            }
                        } else { fail_code = "INVALID_ARGUMENT"; fail_reason = "采集口不能为空"; }
                        if (fail_code.empty()) {
                            json_object* wv = nullptr;
                            json_object* wsrc = nullptr;
                            if (val && (json_object_object_get_ex(val, "whitelist", &wv) ||
                                        json_object_object_get_ex(val, "sources", &wsrc)) && (wv || wsrc)) {
                                json_object* wcheck = wv ? wv : wsrc;
                                std::string bad;
                                if (!validate_whitelist_value(wcheck, bad)) {
                                    fail_code = "INVALID_ARGUMENT";
                                    fail_reason = std::string("白名单地址/端口非法: ") + bad;
                                }
                            }
                        }
                        if (fail_code.empty()) {
                            json_object_object_add(node_patch, "input", json_object_get(val));
                            json_object* wv = nullptr;
                            if (val && (json_object_object_get_ex(val, "whitelist", &wv) ||
                                        json_object_object_get_ex(val, "sources", &wv)) && wv) {
                                fwd = json_object_new_object();
                                json_object_object_add(fwd, "whitelist", json_object_get(wv));
                                forward_media = true;
                            }
                            g_store->update_node(node_patch);
                            json_object_put(node_patch);
                            node_patch = json_object_new_object();
                            g_store->mark_applied(scope, 0);
                        }
                    } else if (scope == "output") {
                        json_object* wv = nullptr; json_object* hv = nullptr; json_object* zv = nullptr;
                        json_object* mv = nullptr; json_object* cv = nullptr;
                        int w = (val && json_object_object_get_ex(val, "width", &wv) && wv) ? json_object_get_int(wv) : 0;
                        int h = (val && json_object_object_get_ex(val, "height", &hv) && hv) ? json_object_get_int(hv) : 0;
                        int z = (val && json_object_object_get_ex(val, "refreshHz", &zv) && zv) ? json_object_get_int(zv) : 0;
                        std::string mode = (val && json_object_object_get_ex(val, "mode", &mv) && mv) ? json_object_get_string(mv) : "force";
                        std::string conn = (val && json_object_object_get_ex(val, "connector", &cv) && cv) ? json_object_get_string(cv) : "";
                        int lim_w = 7680, lim_h = 4320, lim_z = 60;
                        bool conn_known = false;
                        json_object* caps = nullptr;
                        json_object_object_get_ex(g_store->node(), "capabilities", &caps);
                        json_object* arr = nullptr;
                        if (caps && json_object_object_get_ex(caps, "outputPorts", &arr) && arr &&
                            json_object_is_type(arr, json_type_array)) {
                            for (size_t i = 0; i < (size_t)json_object_array_length(arr); i++) {
                                json_object* e = json_object_array_get_idx(arr, (int)i);
                                json_object* idv = nullptr;
                                if (!e || !json_object_object_get_ex(e, "id", &idv) || !idv) continue;
                                if (!conn.empty() && conn != json_object_get_string(idv)) continue;
                                conn_known = true;
                                json_object* q = nullptr;
                                if (json_object_object_get_ex(e, "maxWidth", &q) && q) lim_w = json_object_get_int(q);
                                if (json_object_object_get_ex(e, "maxHeight", &q) && q) lim_h = json_object_get_int(q);
                                if (json_object_object_get_ex(e, "maxRefreshHz", &q) && q) lim_z = json_object_get_int(q);
                                break;
                            }
                            if (!conn.empty() && !conn_known) { fail_code = "INVALID_ARGUMENT"; fail_reason = "输出口不存在"; }
                        }
                        if (fail_code.empty()) {
                            if (w < 640 || w > lim_w || h < 480 || h > lim_h || z < 24 || z > lim_z) {
                                fail_code = "INVALID_ARGUMENT";
                                fail_reason = "输出参数超出本机能力范围";
                            } else if (mode != "force" && mode != "hotplug") {
                                fail_code = "INVALID_ARGUMENT";
                                fail_reason = "输出模式取值非法";
                            }
                        }
                        if (fail_code.empty()) {
                            json_object_object_add(node_patch, "output", json_object_get(val));
                            g_store->update_node(node_patch);
                            json_object_put(node_patch);
                            node_patch = json_object_new_object();
                            fwd = json_object_new_object();
                            json_object_object_add(fwd, "scope", json_object_new_string("output"));
                            json_object_object_add(fwd, "value", json_object_get(val));
                            forward_media = true;
                            g_store->mark_applied(scope, 0);
                        }
                    } else if (scope == "network") {
                        json_object* mv = nullptr;
                        std::string nmode = (val && json_object_object_get_ex(val, "mode", &mv) && mv) ? json_object_get_string(mv) : "dhcp";
                        json_object* iv = nullptr; json_object* kv = nullptr;
                        json_object* gv = nullptr; json_object* dvv = nullptr; json_object* sv = nullptr;
                        std::string ip = (val && json_object_object_get_ex(val, "ip", &iv) && iv) ? json_object_get_string(iv) : "";
                        std::string mask = (val && json_object_object_get_ex(val, "mask", &kv) && kv) ? json_object_get_string(kv) : "";
                        std::string gw = (val && json_object_object_get_ex(val, "gateway", &gv) && gv) ? json_object_get_string(gv) : "";
                        std::string dns = (val && json_object_object_get_ex(val, "dns", &dvv) && dvv) ? json_object_get_string(dvv) : "";
                        std::string srv = (val && json_object_object_get_ex(val, "server", &sv) && sv) ? json_object_get_string(sv) : "";
                        if (nmode != "dhcp" && nmode != "static") { fail_code = "INVALID_ARGUMENT"; fail_reason = "网络方式非法"; }
                        else if (nmode == "static" && (!valid_ip4(ip) || !valid_ip4(mask) || !valid_ip4(gw) || !valid_ip4(dns))) {
                            fail_code = "INVALID_ARGUMENT"; fail_reason = "静态地址格式不正确";
                        } else if (srv.empty()) { fail_code = "INVALID_ARGUMENT"; fail_reason = "缺少注册服务器地址"; }
                        if (fail_code.empty()) {
                            /* NET-1: snapshot before mutating, apply to the system
                             * (env-gated), roll back the node object on failure. */
                            json_object* net_old = nullptr;
                            json_object_object_get_ex(g_store->node(), "network", &net_old);
                            json_object* net_old_copy = net_old ? json_object_get(net_old) : json_object_new_object();
                            std::string ip_old = get_str(g_store->node(), "ip");
                            std::string apply_err;
                            if (!apply_system_network(nmode, ip, mask, gw, dns, apply_err)) {
                                json_object* rb = json_object_new_object();
                                json_object_object_add(rb, "network", net_old_copy);
                                json_object_object_add(rb, "ip", json_object_new_string(ip_old.c_str()));
                                g_store->update_node(rb);
                                json_object_put(rb);
                                fail_code = "NET_APPLY_FAILED";
                                fail_reason = std::string("网络系统应用失败，已回滚: ") + apply_err;
                                json_object_put(net_old_copy);
                            } else {
                                json_object_put(net_old_copy);
                                json_object* net = json_object_new_object();
                                json_object_object_add(net, "mode", json_object_new_string(nmode.c_str()));
                                if (nmode == "static") {
                                    json_object_object_add(net, "mask", json_object_new_string(mask.c_str()));
                                    json_object_object_add(net, "gateway", json_object_new_string(gw.c_str()));
                                    json_object_object_add(net, "dns", json_object_new_string(dns.c_str()));
                                    json_object_object_add(net, "server", json_object_new_string(srv.c_str()));
                                    json_object_object_add(node_patch, "ip", json_object_new_string(ip.c_str()));
                                    json_object_object_add(net, "ip", json_object_new_string(ip.c_str()));
                                } else {
                                    json_object_object_add(net, "server", json_object_new_string(srv.c_str()));
                                }
                                json_object_object_add(node_patch, "network", net);
                                g_store->update_node(node_patch);
                                json_object_put(node_patch);
                                node_patch = json_object_new_object();
                                g_store->mark_applied(scope, 0);
                            }
                        }
                    } else if (scope == "registration") {
                        json_object* mv = nullptr; json_object* sv = nullptr;
                        std::string rm = (val && json_object_object_get_ex(val, "mode", &mv) && mv) ? json_object_get_string(mv) : "";
                        std::string srv = (val && json_object_object_get_ex(val, "server", &sv) && sv) ? json_object_get_string(sv) : "";
                        if ((rm != "auto" && rm != "manual") || srv.empty()) {
                            fail_code = "INVALID_ARGUMENT"; fail_reason = "缺少注册服务器地址";
                        } else {
                            json_object_object_add(node_patch, "registerMode", json_object_new_string(rm.c_str()));
                            json_object* net0 = nullptr;
                            json_object_object_get_ex(g_store->node(), "network", &net0);
                            json_object* net = net0 ? json_object_get(net0) : json_object_new_object();
                            if (!json_object_is_type(net, json_type_object)) { json_object_put(net); net = json_object_new_object(); }
                            json_object_object_add(net, "server", json_object_new_string(srv.c_str()));
                            json_object_object_add(node_patch, "network", net);
                            g_store->update_node(node_patch);
                            json_object_put(node_patch);
                            node_patch = json_object_new_object();
                            g_store->mark_applied(scope, 0);
                            if (rm == "auto" && g_reg) g_reg->trigger_async();
                        }
                    } else if (scope == "sip") {
                        json_object* ev = nullptr;
                        bool enabled = val && json_object_object_get_ex(val, "enabled", &ev) && ev && json_object_get_boolean(ev);
                        if (!enabled) {
                            json_object* sip = json_object_new_object();
                            json_object_object_add(sip, "enabled", json_object_new_boolean(0));
                            json_object_object_add(sip, "state", json_object_new_string("disabled"));
                            json_object_object_add(node_patch, "sip", sip);
                            g_store->update_node(node_patch);
                            json_object_put(node_patch);
                            node_patch = json_object_new_object();
                            g_store->mark_applied(scope, 0);
                        } else {
                            json_object* sv = nullptr; json_object* pv = nullptr;
                            json_object* dvv = nullptr; json_object* uv = nullptr;
                            std::string srv = (val && json_object_object_get_ex(val, "server", &sv) && sv) ? json_object_get_string(sv) : "";
                            int port = (val && json_object_object_get_ex(val, "port", &pv) && pv) ? json_object_get_int(pv) : 0;
                            std::string dev = (val && json_object_object_get_ex(val, "deviceId", &dvv) && dvv) ? json_object_get_string(dvv) : "";
                            std::string usr = (val && json_object_object_get_ex(val, "user", &uv) && uv) ? json_object_get_string(uv) : "";
                            if (srv.empty() || port < 1 || port > 65535 || dev.empty() || usr.empty()) {
                                fail_code = "INVALID_ARGUMENT"; fail_reason = "SIP 参数不完整";
                            } else if (!tcp_reachable(srv, port, 3000)) {
                                json_object* sip = json_object_new_object();
                                json_object_object_add(sip, "enabled", json_object_new_boolean(1));
                                json_object_object_add(sip, "state", json_object_new_string("failed"));
                                json_object_object_add(sip, "server", json_object_new_string(srv.c_str()));
                                json_object_object_add(sip, "port", json_object_new_int(port));
                                json_object_object_add(node_patch, "sip", sip);
                                g_store->update_node(node_patch);
                                json_object_put(node_patch);
                                node_patch = json_object_new_object();
                                fail_code = "SIP_UNREACHABLE"; fail_reason = "SIP 服务不可达";
                            } else {
                                json_object* sip = json_object_new_object();
                                json_object_object_add(sip, "enabled", json_object_new_boolean(1));
                                json_object_object_add(sip, "state", json_object_new_string("connected"));
                                json_object_object_add(sip, "server", json_object_new_string(srv.c_str()));
                                json_object_object_add(sip, "port", json_object_new_int(port));
                                json_object_object_add(node_patch, "sip", sip);
                                g_store->update_node(node_patch);
                                json_object_put(node_patch);
                                node_patch = json_object_new_object();
                                g_store->mark_applied(scope, 0);
                            }
                        }
                    }
                    if (!fail_code.empty()) g_store->mark_failed(scope, fail_code.c_str(), fail_reason.c_str());
                    /* FWD-1: media forward failure is log-only, never flips the
                     * applied/failed decision. OUT-1: output scope forwards in
                     * the background so compositor rebuild never blocks HTTP;
                     * no-picture case is save-only on the engine side. */
                    if (forward_media && fwd) {
                        if (scope == "output") {
                            json_object* fwd_bg = json_object_get(fwd);
                            std::string sc = scope;
                            std::thread([fwd_bg, sc]() {
                                std::string fr = g_media->request("configure", fwd_bg);
                                json_object* fo = json_tokener_parse(fr.c_str());
                                bool ok = false;
                                std::string code, msg;
                                if (fo) {
                                    json_object* okv = nullptr;
                                    json_object* codev = nullptr;
                                    json_object* msgv = nullptr;
                                    if (json_object_object_get_ex(fo, "ok", &okv) && okv)
                                        ok = json_object_get_boolean(okv) != 0;
                                    if (json_object_object_get_ex(fo, "code", &codev) && codev)
                                        code = json_object_get_string(codev);
                                    if (json_object_object_get_ex(fo, "msg", &msgv) && msgv)
                                        msg = json_object_get_string(msgv);
                                    json_object_put(fo);
                                }
                                if (!ok)
                                    fprintf(stderr, "dvs-agent: media configure (%s) failed (log-only) code=%s msg=%s\n",
                                            sc.c_str(), code.c_str(), msg.c_str());
                                else
                                    fprintf(stderr, "dvs-agent: media configure (%s) ok (background)\n", sc.c_str());
                                json_object_put(fwd_bg);
                            }).detach();
                            json_object_put(fwd);
                        } else {
                            std::string fr = g_media->request("configure", fwd);
                            json_object* fo = json_tokener_parse(fr.c_str());
                            if (fo) {
                                json_object* okv = nullptr;
                                bool ok = false;
                                std::string code, msg;
                                if (json_object_object_get_ex(fo, "ok", &okv) && okv)
                                    ok = json_object_get_boolean(okv) != 0;
                                json_object* codev = nullptr;
                                if (json_object_object_get_ex(fo, "code", &codev) && codev)
                                    code = json_object_get_string(codev);
                                json_object* msgv = nullptr;
                                if (json_object_object_get_ex(fo, "msg", &msgv) && msgv)
                                    msg = json_object_get_string(msgv);
                                if (!ok)
                                    fprintf(stderr, "dvs-agent: media configure (%s) failed (log-only) code=%s msg=%s\n",
                                            scope.c_str(), code.c_str(), msg.c_str());
                                json_object_put(fo);
                            } else {
                                fprintf(stderr, "dvs-agent: media configure (%s) unparsable (log-only)\n", scope.c_str());
                            }
                            json_object_put(fwd);
                        }
                    } else if (fwd) { json_object_put(fwd); }
                    json_object* patch = json_object_new_object();
                    json_object_object_add(patch, "configStates", g_store->all_states());
                    json_object_object_add(patch, "configRevision", g_store->config_revision());
                    g_store->update_node(patch);
                    json_object_put(patch);
                    json_object_put(node_patch);
                    json_object* cfg = g_store->get_config(scope, node_id);
                    if (body) json_object_put(body);
                    send_json(res, envelope_ok(cfg, fail_code.empty() ? "配置已提交" : "配置失败"));
                    return;
                }
                if (req.method == "POST" && action == "retry") {
                    g_store->retry(scope);
                    json_object* patch = json_object_new_object();
                    json_object_object_add(patch, "configStates", g_store->all_states());
                    g_store->update_node(patch);
                    json_object_put(patch);
                    json_object* cfg = g_store->get_config(scope, node_id);
                    send_json(res, envelope_ok(cfg, "配置已重新提交，等待应用"));
                    return;
                }
                send_json(res, envelope_fail("NOT_FOUND", 404, "未定义的节点配置操作"));
                return;
            }
        }

        if (req.method == "GET" && req.path == "/api/v1/nodes") {
            json_object* arr = json_object_new_array();
            json_object_array_add(arr, json_object_get(g_store->node()));
            json_object* d = json_object_new_object();
            json_object_object_add(d, "items", arr);
            json_object_object_add(d, "page", json_object_new_int(1));
            json_object_object_add(d, "pageSize", json_object_new_int(1));
            json_object_object_add(d, "total", json_object_new_int(1));
            send_json(res, envelope_ok(d, ""));
            return;
        }

        if (req.method == "POST" && req.path == "/api/v1/nodes/heartbeat") {
            json_object* body = json_tokener_parse(req.body.c_str());
            json_object* patch = nullptr;
            if (body) {
                if (!json_object_object_get_ex(body, "patch", &patch)) patch = body;
            }
            // update lastHeartbeat
            json_object* hp = json_object_new_object();
            json_object_object_add(hp, "lastHeartbeat", json_object_new_string("now"));
            if (patch) {
                json_object* m = nullptr;
                if (json_object_object_get_ex(patch, "metrics", &m)) json_object_object_add(hp, "metrics", json_object_get(m));
            }
            g_store->update_node(hp);
            json_object_put(hp);
            if (body) json_object_put(body);
            send_json(res, envelope_ok(json_object_get(g_store->node()), "心跳已接收"));
            return;
        }

        if (req.method == "POST" && req.path == "/api/v1/nodes/upgrade") {
            json_object* body = json_tokener_parse(req.body.c_str());
            json_object* ids = nullptr;
            if (body) json_object_object_get_ex(body, "targetNodeIds", &ids);
            if (!ids && body) json_object_object_get_ex(body, "nodeIds", &ids);
            json_object* d = json_object_new_object();
            json_object* arr = json_object_new_array();
            if (ids) {
                // assume array
                int len = json_object_array_length(ids);
                for (int i = 0; i < len; i++) json_object_array_add(arr, json_object_get(json_object_array_get_idx(ids, i)));
            }
            json_object_object_add(d, "nodeIds", arr);
            json_object_object_add(d, "status", json_object_new_string("applied"));
            if (body) json_object_put(body);
            send_json(res, envelope_ok(d, ""));
            return;
        }

        // node by id routes
        if (req.path.rfind("/api/v1/nodes/", 0) == 0) {
            std::string rest = req.path.substr(strlen("/api/v1/nodes/"));
            std::string id = rest;
            std::string action;
            size_t slash = rest.find('/');
            if (slash != std::string::npos) { id = rest.substr(0, slash); action = rest.substr(slash+1); }
            if (req.method == "GET" && action.empty()) {
                send_json(res, envelope_ok(json_object_get(g_store->node()), ""));
                return;
            }
            if (req.method == "PUT" && action.empty()) {
                json_object* body = json_tokener_parse(req.body.c_str());
                std::string name = get_str(body, "name");
                if (body) json_object_put(body);
                json_object* patch = json_object_new_object();
                json_object_object_add(patch, "name", json_object_new_string(name.c_str()));
                g_store->update_node(patch);
                json_object_put(patch);
                send_json(res, envelope_ok(json_object_get(g_store->node()), ""));
                return;
            }
            if (req.method == "POST" && action == "retire") {
                json_object* patch = json_object_new_object();
                json_object_object_add(patch, "lifecycle", json_object_new_string("retired"));
                json_object_object_add(patch, "online", json_object_new_boolean(0));
                json_object_object_add(patch, "bind", json_object_new_string("—"));
                g_store->update_node(patch);
                json_object_put(patch);
                send_json(res, envelope_ok(json_object_get(g_store->node()), ""));
                return;
            }
            if (req.method == "POST" && action == "heartbeat") {
                json_object* body = json_tokener_parse(req.body.c_str());
                json_object* patch = nullptr;
                if (body) {
                    if (!json_object_object_get_ex(body, "patch", &patch)) patch = body;
                }
                json_object* hp = json_object_new_object();
                json_object_object_add(hp, "lastHeartbeat", json_object_new_string("now"));
                if (patch) {
                    json_object* m = nullptr;
                    if (json_object_object_get_ex(patch, "metrics", &m)) json_object_object_add(hp, "metrics", json_object_get(m));
                }
                g_store->update_node(hp);
                json_object_put(hp);
                if (body) json_object_put(body);
                send_json(res, envelope_ok(json_object_get(g_store->node()), "心跳已接收"));
                return;
            }
        }

        // media endpoints
        if (req.method == "GET" && req.path == "/api/v1/node/media/status") {
            // pass through the engine response (or structured error envelope)
            std::string r = g_media->query_state();
            json_object* o = json_tokener_parse(r.c_str());
            if (!o) o = json_object_new_object();
            send_json(res, o); // already a full envelope from the media engine
            return;
        }
        if (req.method == "GET" && req.path == "/api/v1/node/media/events") {
            send_json(res, envelope_ok(g_media->drain_events(), ""));
            return;
        }
        if (req.method == "POST" && req.path == "/api/v1/node/media/prepare") {
            json_object* body = json_tokener_parse(req.body.c_str());
            std::string r = g_media->request("prepare", body);
            if (body) json_object_put(body);
            json_object* o = json_tokener_parse(r.c_str());
            if (!o) o = json_object_new_object();
            send_json(res, o);
            return;
        }
        // Receive a platform-computed per-node sub-layout, forward it to the
        // media engine as a prepare, and report the result back to the
        // platform barrier. The node never re-fragments.
        if (req.method == "POST" && req.path == "/api/v1/node/media/layout") {
            if (!g_api_token.empty() && !token_ok) {
                send_json(res, envelope_fail("UNAUTHORIZED", 401, "missing or bad token"));
                return;
            }
            json_object* body = json_tokener_parse(req.body.c_str());
            std::string opid = get_str(body, "operationId");
            int rev = 1;
            json_object* rv = nullptr;
            if (body && json_object_object_get_ex(body, "revision", &rv)) rev = json_object_get_int(rv);
            int64_t applyAtNs = 0;
            json_object* av = nullptr;
            if (body && json_object_object_get_ex(body, "applyAtNs", &av))
                applyAtNs = (int64_t)json_object_get_int64(av);
            if (opid.empty()) {
                if (body) json_object_put(body);
                send_json(res, envelope_fail("INVALID_ARGUMENT", 400, "missing operationId"));
                return;
            }
            json_object* wins0 = nullptr;
            if (body) json_object_object_get_ex(body, "windows", &wins0);
            if (wins0 && !json_object_is_type(wins0, json_type_array)) {
                if (body) json_object_put(body);
                send_json(res, envelope_fail("INVALID_ARGUMENT", 400, "windows must be an array"));
                return;
            }
            json_object* prep = json_object_new_object();
            json_object_object_add(prep, "operationId", json_object_new_string(opid.c_str()));
            json_object_object_add(prep, "revision", json_object_new_int(rev));
            json_object_object_add(prep, "applyAtNs", json_object_new_int64(applyAtNs));
            json_object* wins = nullptr;
            if (body && json_object_object_get_ex(body, "windows", &wins))
                json_object_object_add(prep, "windows", json_object_get(wins));
            json_object* geo = nullptr;
            if (body && json_object_object_get_ex(body, "geometry", &geo))
                json_object_object_add(prep, "geometry", json_object_get(geo));
            std::string r = g_media->request("prepare", prep);
            json_object_put(prep);
            std::string nodeId = get_str(g_store->node(), "id");
            if (nodeId.empty()) nodeId = "local-node";
            // detach reporter; platform_url captured by value for the thread.
            std::thread([opid, rev, applyAtNs, nodeId, platform_url]() {
                agent_report_commit(opid, rev, applyAtNs, nodeId, platform_url);
            }).detach();
            json_object* o = json_tokener_parse(r.c_str());
            if (!o) o = json_object_new_object();
            send_json(res, o);
            if (body) json_object_put(body);
            return;
        }
        // Expose per-lane RX readiness from the engine for barrier checks.
        if (req.method == "GET" && req.path == "/api/v1/node/media/lanes") {
            std::string r = g_media->query_state();
            json_object* o = json_tokener_parse(r.c_str());
            json_object* d = nullptr;
            json_object* lanes = nullptr;
            if (o && json_object_object_get_ex(o, "data", &d) && d)
                json_object_object_get_ex(d, "lanes", &lanes);
            send_json(res, envelope_ok(lanes ? json_object_get(lanes)
                                             : json_object_new_array(), ""));
            if (o) json_object_put(o);
            return;
        }
        if (req.method == "GET" && req.path == "/api/v1/node/media/sources") {
            send_json(res, envelope_ok(json_object_new_object(), ""));
            return;
        }
        if (req.method == "GET" && req.path == "/api/v1/node/media/logs") {
            json_object* d = json_object_new_object();
            json_object_object_add(d, "items", json_object_new_array());
            send_json(res, envelope_ok(d, ""));
            return;
        }
        // Accept the platform-owned config subset. Only geometry and role
        // apply; every other key is reported as ignored and never stored.
        if (req.method == "POST" && req.path == "/api/v1/node/config/sync") {
            if (!g_api_token.empty() && !token_ok) {
                send_json(res, envelope_fail("UNAUTHORIZED", 401, "missing or bad token"));
                return;
            }
            json_object* body = json_tokener_parse(req.body.c_str());
            json_object* cfg = nullptr;
            if (body) json_object_object_get_ex(body, "config", &cfg);
            json_object* patch = json_object_new_object();
            json_object* ignored = json_object_new_array();
            if (cfg && json_object_is_type(cfg, json_type_object)) {
                json_object_object_foreach(cfg, key, val) {
                    std::string k(key);
                    if (k == "geometry") {
                        json_object_object_add(patch, "geometry", json_object_get(val));
                    } else if (k == "role") {
                        g_store->set_desired("role", json_object_get(val), nullptr);
                        json_object* rv = nullptr;
                        if (json_object_is_type(val, json_type_object) &&
                            json_object_object_get_ex(val, "role", &rv) && rv) {
                            json_object* pr = json_object_new_string(json_object_get_string(rv));
                            json_object* pp = json_object_new_object();
                            json_object_object_add(pp, "pendingRole", pr);
                            g_store->update_node(pp);
                            json_object_put(pp);
                        } else if (json_object_is_type(val, json_type_string)) {
                            json_object* pr = json_object_new_string(json_object_get_string(val));
                            json_object* pp = json_object_new_object();
                            json_object_object_add(pp, "pendingRole", pr);
                            g_store->update_node(pp);
                            json_object_put(pp);
                        }
                    } else {
                        json_object_array_add(ignored, json_object_new_string(k.c_str()));
                    }
                }
            }
            json_object_object_add(patch, "configSource", json_object_new_string("platform"));
            g_store->update_node(patch);
            json_object_put(patch);
            json_object* d = json_object_new_object();
            json_object_object_add(d, "applied", json_object_new_boolean(1));
            json_object_object_add(d, "source", json_object_new_string("platform"));
            json_object_object_add(d, "ignored", ignored);
            if (body) json_object_put(body);
            send_json(res, envelope_ok(d, "config synced"));
            return;
        }
        if (req.method == "GET" && req.path == "/api/v1/node/capability") {
            std::string cr = g_media->request("capability", nullptr);
            json_object* co = json_tokener_parse(cr.c_str());
            if (co) {
                json_object* okv = nullptr;
                json_object* data = nullptr;
                if (json_object_object_get_ex(co, "ok", &okv) && okv && json_object_get_boolean(okv) &&
                    json_object_object_get_ex(co, "data", &data) && data) {
                    json_object* patch = json_object_new_object();
                    json_object* caps = nullptr;
                    json_object_object_get_ex(g_store->node(), "capabilities", &caps);
                    json_object* merged = caps ? json_object_get(caps) : json_object_new_object();
                    json_object_object_foreach(data, k, v) {
                        if ((strcmp(k, "inputPorts") == 0 || strcmp(k, "outputPorts") == 0) &&
                            json_object_is_type(v, json_type_array) && json_object_array_length(v) == 0)
                            continue;
                        json_object_object_add(merged, k, json_object_get(v));
                    }
                    json_object_object_add(patch, "capabilities", merged);
                    g_store->update_node(patch);
                    json_object_put(patch);
                }
                json_object_put(co);
            }
            json_object* n = g_store->node();
            json_object* cap = nullptr;
            json_object_object_get_ex(n, "capabilities", &cap);
            send_json(res, envelope_ok(cap ? json_object_get(cap) : json_object_new_object(), ""));
            return;
        }
        if (req.method == "POST" && req.path == "/api/v1/node/register") {
            bool ok = g_reg ? g_reg->trigger_once() : false;
            json_object* d = json_object_new_object();
            json_object_object_add(d, "registerState", json_object_new_string(ok ? "registered" : "failed"));
            json_object_object_add(d, "online", json_object_new_boolean(g_reg && g_reg->online() ? 1 : 0));
            send_json(res, envelope_ok(d, ok ? "设备注册成功" : "注册失败，可修改地址后重试"));
            return;
        }
        if (req.method == "POST" && req.path == "/api/v1/node/upgrade/upload") {
            std::string fn = req.query.count("filename") ? req.query.at("filename") : "fw_upload.bin";
            auto hit = req.headers.find("X-Filename");
            if (hit != req.headers.end() && !hit->second.empty()) fn = hit->second;
            std::string base = fn;
            size_t slash = base.find_last_of("/\\");
            if (slash != std::string::npos) base = base.substr(slash + 1);
            if (base.empty() || base.size() > 128) {
                send_json(res, envelope_fail("INVALID_ARGUMENT", 400, "文件名非法"));
                return;
            }
            std::string dest = "/tmp/" + base;
            std::ofstream out(dest, std::ios::binary);
            if (!out) {
                send_json(res, envelope_fail("INTERNAL", 500, "无法写入暂存"));
                return;
            }
            out.write(req.body.data(), (std::streamsize)req.body.size());
            out.close();
            json_object* d = json_object_new_object();
            json_object_object_add(d, "path", json_object_new_string(dest.c_str()));
            json_object_object_add(d, "size", json_object_new_int64((int64_t)req.body.size()));
            send_json(res, envelope_ok(d, "固件已上传"));
            return;
        }
        if (req.method == "GET" && req.path == "/api/v1/node/capacity") {
            send_json(res, envelope_ok(node_capacity_json(), ""));
            return;
        }
        if (req.method == "GET" && req.path == "/api/v1/node/version") {
            // Bundle contract report: this agent's versions plus the live
            // media engine hello, so ops can confirm a co-released pair.
            std::string hr = g_media->hello();
            json_object* ho = json_tokener_parse(hr.c_str());
            json_object* d = json_object_new_object();
            json_object_object_add(d, "bundleVersion",
                                  json_object_new_string(DVS_NODE_BUNDLE_VERSION));
            json_object_object_add(d, "ipcVersion",
                                  json_object_new_int(DVS_MEDIA_IPC_VERSION));
            json_object_object_add(d, "media",
                                  ho ? ho : json_object_new_object());
            send_json(res, envelope_ok(d, ""));
            return;
        }

        // maintenance
        if (req.method == "POST" && req.path == "/api/v1/node/reboot") {
            send_json(res, envelope_ok(g_maint->reboot(), ""));
            return;
        }
        if (req.method == "POST" && req.path == "/api/v1/node/factory-reset") {
            send_json(res, envelope_ok(g_maint->factory_reset(), ""));
            return;
        }
        if (req.method == "POST" && req.path == "/api/v1/node/upgrade") {
            json_object* body = json_tokener_parse(req.body.c_str());
            std::string fn = body ? get_str(body, "filename") : std::string("fw.bin");
            if (fn.empty()) fn = "fw.bin";
            std::string sha = body ? get_str(body, "sha256") : std::string();
            if (body) json_object_put(body);
            send_json(res, envelope_ok(g_maint->upgrade(fn, sha), ""));
            return;
        }

        if (req.method == "GET" && req.path == "/api/v1/audit-events") {
            json_object* d = json_object_new_object();
            json_object_object_add(d, "items", json_object_new_array());
            json_object_object_add(d, "page", json_object_new_int(1));
            json_object_object_add(d, "pageSize", json_object_new_int(100));
            json_object_object_add(d, "total", json_object_new_int(0));
            send_json(res, envelope_ok(d, ""));
            return;
        }

        // static files
        if (req.method == "GET") {
            std::string path = req.path;
            if (path == "/" || path.empty()) path = "/index.html";
            std::string full = g_web_root + path;
            struct stat st;
            if (stat(full.c_str(), &st) == 0 && S_ISREG(st.st_mode)) {
                std::string content = read_file(full);
                res.status = 200;
                res.body = content;
                res.content_type = guess_ct(full);
                res.headers["Cache-Control"] = "no-cache";
                res.headers["X-Content-Type-Options"] = "nosniff";
                return;
            }
        }

        send_json(res, envelope_fail("NOT_FOUND", 404, ("未定义的 API：" + req.method + " " + req.path).c_str()));
    };

    HttpServer server(port, handler);
    server.start();
    printf("dvs-agent listening on 0.0.0.0:%d\n", port);
    fflush(stdout);

    // registration + heartbeat loop (no-op / local-autonomous when platform URL
    // is empty; never disturbs media output on transport failure)
    g_reg->start();

    while (true) std::this_thread::sleep_for(std::chrono::seconds(3600));
    return 0;
}
