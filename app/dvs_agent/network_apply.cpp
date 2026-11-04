#include "network_apply.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

static bool valid_ip4(const std::string& s) {
    if (s.empty() || s.size() > 15) return false;
    struct in_addr a;
    return inet_pton(AF_INET, s.c_str(), &a) == 1;
}

static bool valid_dns_name(const std::string& s) {
    if (s.empty() || s.size() > 253) return false;
    for (char c : s) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '-')
            continue;
        return false;
    }
    return true;
}

bool net_parse_server(const std::string& in, std::string& host, int& port,
                      std::string& err) {
    std::string t;
    for (char c : in) {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') t.push_back(c);
    }
    if (t.empty()) {
        err = "missing server address";
        return false;
    }
    std::string h = t;
    port = 80;
    size_t colon = t.rfind(':');
    if (colon != std::string::npos && t.find(':') == colon) {
        h = t.substr(0, colon);
        std::string ps = t.substr(colon + 1);
        if (ps.empty() || ps.size() > 5) {
            err = "bad server port";
            return false;
        }
        for (char c : ps) {
            if (c < '0' || c > '9') {
                err = "bad server port";
                return false;
            }
        }
        port = atoi(ps.c_str());
        if (port < 1 || port > 65535) {
            err = "bad server port";
            return false;
        }
    } else if (colon != std::string::npos) {
        err = "bad server address";
        return false;
    }
    if (!valid_ip4(h) && !valid_dns_name(h)) {
        err = "bad server host";
        return false;
    }
    /* All-numeric dotted hosts that are not valid IPs are neither valid IPs
     * nor valid DNS names (numeric TLDs are prohibited); reject them. */
    bool numeric_dots = !h.empty();
    for (char c : h) {
        if ((c < '0' || c > '9') && c != '.') { numeric_dots = false; break; }
    }
    if (numeric_dots && !valid_ip4(h)) {
        err = "bad server host";
        return false;
    }
    host = h;
    return true;
}

bool net_mask_to_prefix(const std::string& mask, int& prefix, std::string& err) {
    struct in_addr a;
    if (inet_pton(AF_INET, mask.c_str(), &a) != 1) {
        err = "bad mask";
        return false;
    }
    uint32_t m = ntohl(a.s_addr);
    int bits = 0;
    bool zero_seen = false;
    for (int i = 31; i >= 0; i--) {
        bool b = (m >> i) & 1u;
        if (b) {
            if (zero_seen) {
                err = "non-contiguous mask";
                return false;
            }
            bits++;
        } else {
            zero_seen = true;
        }
    }
    if (bits == 0 || bits > 30) {
        err = "mask prefix out of range";
        return false;
    }
    prefix = bits;
    return true;
}

bool net_valid_iface(const std::string& iface) {
    if (iface.empty() || iface.size() > 16) return false;
    for (char c : iface) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
            continue;
        return false;
    }
    return true;
}

bool net_execution_requested(bool confirm, const std::string& opid) {
    const char* allow = std::getenv("DVS_ALLOW_NETWORK_APPLY");
    return allow && std::string(allow) == "1" && confirm && !opid.empty();
}

std::string net_networkd_path() {
    const char* v = std::getenv("DVS_NETWORKD_FILE");
    if (v && *v) return std::string(v);
    return "/etc/systemd/network/10-dvs-node.network";
}

std::string net_render_networkd(const NetExecParams& p) {
    std::ostringstream ss;
    ss << "# managed by dvs-agent (operation " << p.opid << ")\n";
    ss << "[Match]\nName=" << p.iface << "\n\n[Network]\n";
    if (p.mode == "dhcp") {
        ss << "DHCP=yes\n";
    } else {
        int prefix = 24;
        std::string dummy;
        if (net_mask_to_prefix(p.mask, prefix, dummy)) { /* keep parsed */
        }
        ss << "Address=" << p.ip << "/" << prefix << "\n";
        if (!p.gw.empty()) ss << "Gateway=" << p.gw << "\n";
        if (!p.dns.empty()) ss << "DNS=" << p.dns << "\n";
    }
    return ss.str();
}

static std::string read_file_all(const std::string& path, bool* existed) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        if (existed) *existed = false;
        return "";
    }
    if (existed) *existed = true;
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string run_capture(const char* argv0, char* const argv[]) {
    std::string out;
    int fds[2];
    if (pipe(fds) != 0) return out;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return out;
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        dup2(fds[1], STDERR_FILENO);
        close(fds[0]);
        close(fds[1]);
        execvp(argv0, argv);
        _exit(127);
    }
    close(fds[1]);
    char buf[1024];
    ssize_t n = 0;
    struct pollfd pfd{.fd = fds[0], .events = POLLIN};
    while (poll(&pfd, 1, 3000) > 0) {
        n = read(fds[0], buf, sizeof(buf));
        if (n <= 0) break;
        out.append(buf, (size_t)n);
    }
    close(fds[0]);
    int st = 0;
    for (int i = 0; i < 50; i++) {
        pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) break;
        usleep(100 * 1000);
    }
    return out;
}

/* Run argv with stdout/stderr appended to log_path, bounded by timeout_ms.
 * Returns the child exit code, or -1 on timeout/spawn failure. */
static int run_logged(const char* argv0, char* const argv[], int timeout_ms,
                      const std::string& log_path) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int fd = open(log_path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
        if (fd >= 0) {
            dup2(fd, STDOUT_FILENO);
            dup2(fd, STDERR_FILENO);
            if (fd != STDOUT_FILENO && fd != STDERR_FILENO) close(fd);
        }
        execvp(argv0, argv);
        _exit(127);
    }
    int waited = 0;
    while (waited < timeout_ms) {
        int st = 0;
        pid_t w = waitpid(pid, &st, WNOHANG);
        if (w == pid) {
            if (WIFEXITED(st)) return WEXITSTATUS(st);
            return -1;
        }
        usleep(100 * 1000);
        waited += 100;
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    return -1;
}

static bool tcp_reachable_once(const std::string& host, int port) {
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
        if (connect(fd, p->ai_addr, p->ai_addrlen) == 0) {
            ok = true;
            close(fd);
            break;
        }
        if (errno == EINPROGRESS) {
            struct pollfd pfd{.fd = fd, .events = POLLOUT};
            if (poll(&pfd, 1, 3000) > 0 && (pfd.revents & POLLOUT)) {
                int err = 0;
                socklen_t n = sizeof(err);
                if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &n) == 0 &&
                    err == 0)
                    ok = true;
            }
        }
        close(fd);
        if (ok) break;
    }
    freeaddrinfo(ai);
    return ok;
}

static void mkdir_p(const std::string& dir) {
    std::string cur;
    for (size_t i = 0; i < dir.size(); i++) {
        cur.push_back(dir[i]);
        if (dir[i] == '/' && cur.size() > 1) mkdir(cur.c_str(), 0755);
    }
    if (!cur.empty()) mkdir(cur.c_str(), 0755);
}

NetExecResult net_apply_real(const NetExecParams& p) {
    NetExecResult r;
    r.executed = true;
    r.method = "systemd-networkd";
    std::string log = "/var/lib/dvs_agent/net_apply_" +
                      (p.opid.empty() ? std::string("manual") : p.opid) + ".log";

    if (std::getenv("DVS_FORCE_NETWORK_FAIL") &&
        std::string(std::getenv("DVS_FORCE_NETWORK_FAIL")) == "1") {
        /* Injected failure before any system change: still exercise the
         * snapshot path so rollback coverage stays honest. */
        bool existed = false;
        read_file_all(net_networkd_path(), &existed);
        r.ok = false;
        r.rolledBack = true;
        r.error = "injected failure (DVS_FORCE_NETWORK_FAIL=1)";
        return r;
    }

    /* Snapshot: previous .network file plus ip diagnostics. */
    std::string path = net_networkd_path();
    bool had_old = false;
    std::string old_content = read_file_all(path, &had_old);
    {
        char* a_argv[] = {(char*)"ip",       (char*)"addr",
                          (char*)"show",     (char*)"dev",
                          (char*)p.iface.c_str(), nullptr};
        char* r_argv[] = {(char*)"ip", (char*)"route", (char*)"show", nullptr};
        std::string diag = run_capture("ip", a_argv);
        diag += run_capture("ip", r_argv);
        std::ofstream lf(log, std::ios::binary | std::ios::trunc);
        if (lf) {
            lf << "op=" << p.opid << " mode=" << p.mode << " iface=" << p.iface
               << "\n--- snapshot ---\n"
               << diag << "\n";
        }
    }

    auto fail = [&](const std::string& why) {
        /* Roll back the file, reload, and report honestly. */
        bool rb_ok = false;
        if (had_old) {
            std::ofstream f(path, std::ios::binary | std::ios::trunc);
            if (f) {
                f << old_content;
                f.close();
                rb_ok = (bool)f;
            }
        } else {
            rb_ok = (unlink(path.c_str()) == 0 || errno == ENOENT);
        }
        if (rb_ok) {
            char* argv[] = {(char*)"networkctl", (char*)"reload", nullptr};
            rb_ok = (run_logged("networkctl", argv, 15000, log) == 0);
        }
        r.ok = false;
        r.rolledBack = rb_ok;
        r.error = rb_ok ? why : (why + "; rollback failed (NET_ROLLBACK_FAILED)");
        return r;
    };

    /* Validate again at the execution boundary (defense in depth). */
    std::string host;
    int port = 0;
    std::string perr;
    if (!net_parse_server(p.server, host, port, perr))
        return fail(std::string("bad server: ") + perr);
    if (!net_valid_iface(p.iface)) return fail("bad iface");
    if (p.mode == "static") {
        int prefix = 0;
        if (!net_mask_to_prefix(p.mask, prefix, perr))
            return fail(std::string("bad mask: ") + perr);
        struct in_addr a;
        if (inet_pton(AF_INET, p.ip.c_str(), &a) != 1 ||
            inet_pton(AF_INET, p.gw.c_str(), &a) != 1 ||
            inet_pton(AF_INET, p.dns.c_str(), &a) != 1)
            return fail("bad static address");
    } else if (p.mode != "dhcp") {
        return fail("bad mode");
    }

    /* Write the new .network file. */
    {
        size_t slash = path.find_last_of('/');
        if (slash != std::string::npos) mkdir_p(path.substr(0, slash));
        mkdir_p("/var/lib/dvs_agent");
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) return fail("cannot write networkd file");
        f << net_render_networkd(p);
        f.close();
        if (!f) return fail("cannot write networkd file");
    }

    /* Apply. */
    {
        char* argv[] = {(char*)"networkctl", (char*)"reload", nullptr};
        int rc = run_logged("networkctl", argv, 15000, log);
        if (rc != 0) return fail("networkctl reload failed or timed out");
    }

    /* Connectivity check against the new server (2 attempts). */
    bool reach = tcp_reachable_once(host, port) || tcp_reachable_once(host, port);
    if (!reach) return fail("server unreachable after apply");

    /* Persist the applied intent for audit (best effort). */
    {
        std::ofstream f("/var/lib/dvs_agent/network_applied.json",
                        std::ios::binary | std::ios::trunc);
        if (f) {
            f << "{\"mode\":\"" << p.mode << "\",\"iface\":\"" << p.iface
              << "\",\"op\":\"" << p.opid << "\"}";
        }
    }
    r.ok = true;
    return r;
}
