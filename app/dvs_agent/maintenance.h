#pragma once
#include <json-c/json.h>
#include <string>

class NodeStore;
class MediaIpcClient;

/* Node maintenance operations (req 03 §10): reboot (with pendingRole applied),
 * factory reset (preserves network + registration), and firmware upgrade
 * (SHA-256 verified, A/B staging, old version kept on failure). */
class Maintenance {
public:
    Maintenance(NodeStore* store, MediaIpcClient* media);
    json_object* reboot();
    json_object* factory_reset();
    json_object* upgrade(const std::string& filename, const std::string& sha256_expected);

private:
    NodeStore* store_;
    MediaIpcClient* media_;
};
