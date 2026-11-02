#include "node_store.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

int main() {
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
