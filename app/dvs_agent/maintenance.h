#pragma once
#include <json-c/json.h>
#include <string>

class NodeStore;
class MediaIpcClient;

/* Node maintenance: reboot (pendingRole applied), factory reset (preserves
 * network + registration), firmware upgrade (SHA-256 verified, staged by
 * content hash, version from the request package metadata, explicit
 * confirm required to switch). No shell, no environment gating: dangerous
 * operations run via direct exec and require {confirm:true}. */
class Maintenance {
public:
    Maintenance(NodeStore* store, MediaIpcClient* media);
    json_object* reboot(bool confirm);
    json_object* factory_reset();
    json_object* upgrade(const std::string& filename,
                         const std::string& sha256_expected,
                         const std::string& version,
                         bool confirm);

private:
    NodeStore* store_;
    MediaIpcClient* media_;
};
