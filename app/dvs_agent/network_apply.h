#pragma once
#include <string>

/* Real system network apply (systemd-networkd single style).
 *
 * Responsibility split: this module owns parsing, file rendering and the
 * privileged execution (snapshot/apply/connectivity/rollback). main.cpp owns
 * HTTP gating and the config state machine. Only net_render_networkd and the
 * parsers are pure; net_apply_real forks and must run in an isolated env.
 */
struct NetExecParams {
    std::string mode;  // "dhcp" | "static"
    std::string ip;
    std::string mask;
    std::string gw;
    std::string dns;
    std::string server;  // "host[:port]" used for the connectivity check
    std::string iface;   // e.g. "eth0"
    std::string opid;    // audit id, non-empty when execution is requested
};

struct NetExecResult {
    bool executed = false;    // a real system change was attempted
    bool ok = false;          // new network up and server reachable
    bool rolledBack = false;  // snapshot was restored after a failure
    std::string error;        // short reason when !ok
    std::string method;       // "systemd-networkd"
};

/* Parse "host[:port]". Port defaults to 80. Host must be IPv4 or a DNS name. */
bool net_parse_server(const std::string& in, std::string& host, int& port,
                      std::string& err);

/* Dotted mask -> prefix length (only contiguous masks accepted). */
bool net_mask_to_prefix(const std::string& mask, int& prefix, std::string& err);

/* Interface name allowlist: [A-Za-z0-9._-]{1,16}. */
bool net_valid_iface(const std::string& iface);

/* Execution gate: explicit {confirm:true} plus a non-empty operationId.
 * No environment gating: the request body is the operator intent. */
bool net_execution_requested(bool confirm, const std::string& opid);

/* Test-only fault injection. True only in debug builds with
 * DVS_FORCE_NETWORK_FAIL=1; release builds never inject. */
bool net_inject_fail_requested();

/* Render the .network file (pure, unit-tested). */
std::string net_render_networkd(const NetExecParams& p);

/* .network path, overridable via DVS_NETWORKD_FILE for tests. */
std::string net_networkd_path();

/* Snapshot -> write -> reload -> connectivity check -> rollback on failure.
 * Never throws; all failures are reported in NetExecResult. */
NetExecResult net_apply_real(const NetExecParams& p);
