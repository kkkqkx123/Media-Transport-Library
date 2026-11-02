#pragma once
#include <json-c/json.h>
#include <string>

class Maintenance {
public:
    Maintenance(void* store, void* media_ipc);
    json_object* reboot();
    json_object* factory_reset();
    json_object* upgrade(const std::string& filename, const std::string& sha256_expected);
private:
    void* store_;
    void* media_ipc_;
};
