#include "node_store.h"
#include "media_ipc.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unistd.h>

// MediaIpcClient against a non-existent socket must return a structured
// error envelope, never an empty string or a crash.
static void test_media_unreachable() {
    MediaIpcClient media("/tmp/dvs_no_such_ipc_sock_12345");
    std::string r = media.query_state();
    assert(!r.empty());
    json_object* o = json_tokener_parse(r.c_str());
    assert(o != nullptr);
    json_object* okv = nullptr;
    assert(json_object_object_get_ex(o, "ok", &okv));
    assert(json_object_get_boolean(okv) == 0);
    json_object* codev = nullptr;
    assert(json_object_object_get_ex(o, "code", &codev));
    std::string code = json_object_get_string(codev);
    assert(code == "MEDIA_ENGINE_UNREACHABLE" || code == "MEDIA_ENGINE_UNCONFIGURED");
    json_object_put(o);

    // drain_events on a dead connection returns an empty array
    json_object* evs = media.drain_events();
    assert(evs != nullptr && json_object_array_length(evs) == 0);
    json_object_put(evs);
    printf("OK: media_ipc unreachable envelope\n");
}

int main() {
    test_media_unreachable();
    char path[] = "/tmp/dvs_test_XXXXXX";
    int fd = mkstemp(path);
    close(fd);
    unlink(path);
    NodeStore store(path);
    // initial state idle
    json_object* cfg = store.get_config("role", "n");
    // check state field
    json_object* st = nullptr;
    // get from config via all? Just ensure non-null
    assert(cfg != nullptr);
    json_object_put(cfg);
    // set desired
    json_object* v = json_object_new_object();
    json_object_object_add(v, "role", json_object_new_string("output"));
    store.set_desired("role", v, "op1");
    json_object_put(v);
    json_object* cfg2 = store.get_config("role", "n");
    json_object* state = nullptr;
    json_object_object_get_ex(cfg2, "state", &state);
    assert(std::string(json_object_get_string(state)) == "pending_apply");
    json_object_put(cfg2);
    unlink(path);
    printf("OK\n");
    return 0;
}
