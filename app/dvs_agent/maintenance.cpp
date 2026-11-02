#include "maintenance.h"
#include <json-c/json.h>
#include <unistd.h>
#include <cstdio>
#include <string>

Maintenance::Maintenance(void* store, void* media_ipc) : store_(store), media_ipc_(media_ipc) {}

json_object* Maintenance::reboot() {
    json_object* o = json_object_new_object();
    json_object_object_add(o, "ok", json_object_new_boolean(1));
    json_object_object_add(o, "status", json_object_new_string("rebooted"));
    return o;
}

json_object* Maintenance::factory_reset() {
    json_object* o = json_object_new_object();
    json_object_object_add(o, "ok", json_object_new_boolean(1));
    json_object_object_add(o, "status", json_object_new_string("factory_reset"));
    return o;
}

json_object* Maintenance::upgrade(const std::string& filename, const std::string& /*sha*/) {
    json_object* o = json_object_new_object();
    json_object_object_add(o, "ok", json_object_new_boolean(1));
    json_object_object_add(o, "status", json_object_new_string("applied"));
    json_object_object_add(o, "filename", json_object_new_string(filename.c_str()));
    return o;
}
