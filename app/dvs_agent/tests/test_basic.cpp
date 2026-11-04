#include "sha256.h"
#include "dvs_node_bundle.h"
#include "node_store.h"
#include "media_ipc.h"
#include "registration.h"
#include "http_client.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); g_failures++; } \
    else { printf("ok:   %s\n", msg); } \
  } while (0)

static std::string sha256_hex(const std::string& s) {
  uint8_t out[32];
  dvs::sha256((const uint8_t*)s.data(), s.size(), out);
  return dvs::to_hex(out, 32);
}

static void test_sha256() {
  CHECK(sha256_hex("") ==
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
        "sha256 empty string");
  CHECK(sha256_hex("abc") ==
        "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
        "sha256 'abc'");
}

static void test_pbkdf2_deterministic() {
  uint8_t out1[32], out2[32];
  uint8_t salt[16] = {0};
  dvs::pbkdf2_hmac_sha256((const uint8_t*)"password", 8, salt, 16, 100000, out1);
  dvs::pbkdf2_hmac_sha256((const uint8_t*)"password", 8, salt, 16, 100000, out2);
  CHECK(memcmp(out1, out2, 32) == 0, "pbkdf2 deterministic for same inputs");
  dvs::pbkdf2_hmac_sha256((const uint8_t*)"other", 5, salt, 16, 100000, out2);
  CHECK(memcmp(out1, out2, 32) != 0, "pbkdf2 differs for different password");
}

static void test_node_store_password() {
  std::string db = "/tmp/dvs_agent_test_pw.db";
  std::remove(db.c_str());
  {
    NodeStore store(db);
    CHECK(store.verify_password("admin", "admin123"), "default password verifies");
    CHECK(!store.verify_password("admin", "wrong"), "wrong password rejected");
    CHECK(store.password_change_required(), "first-login change required by default");
    store.set_password("newpass");
    CHECK(store.verify_password("admin", "newpass"), "new password verifies");
    CHECK(!store.verify_password("admin", "admin123"), "old password rejected after change");
    CHECK(!store.password_change_required(), "change-required cleared after set_password");
  }
  /* reopen: password persists */
  {
    NodeStore store(db);
    CHECK(store.verify_password("admin", "newpass"), "password persists across reopen");
  }
  std::remove(db.c_str());
}

static void test_node_store_state_machine() {
  std::string db = "/tmp/dvs_agent_test_sm.db";
  std::remove(db.c_str());
  NodeStore store(db);

  json_object* val = json_object_new_object();
  json_object_object_add(val, "role", json_object_new_string("input"));
  store.set_desired("role", val, "op-1");
  json_object_put(val);

  json_object* cfg = store.get_config("role", "local-node");
  CHECK(json_object_get_int(json_object_object_get(cfg, "desiredRevision")) == 2,
        "desiredRevision incremented to 2");
  const char* st = json_object_get_string(json_object_object_get(cfg, "state"));
  CHECK(strcmp(st, "pending_apply") == 0, "state is pending_apply after set_desired");
  store.mark_applied("role", 2);
  json_object_put(cfg);

  cfg = store.get_config("role", "local-node");
  st = json_object_get_string(json_object_object_get(cfg, "state"));
  CHECK(strcmp(st, "applied") == 0, "state applied after mark_applied");
  CHECK(json_object_get_int(json_object_object_get(cfg, "appliedRevision")) == 2,
        "appliedRevision == 2");
  store.mark_failed("role", "E_CONF", "boom");
  json_object_put(cfg);

  cfg = store.get_config("role", "local-node");
  st = json_object_get_string(json_object_object_get(cfg, "state"));
  CHECK(strcmp(st, "failed") == 0, "state failed after mark_failed");
  json_object_put(cfg);
  std::remove(db.c_str());
}

static void test_node_store_persistence() {
  std::string db = "/tmp/dvs_agent_test_persist.db";
  std::remove(db.c_str());
  {
    NodeStore store(db);
    json_object* val = json_object_new_object();
    json_object_object_add(val, "x", json_object_new_int(42));
    store.set_desired("network", val, nullptr);
    store.mark_applied("network", 1);
    json_object_put(val);
  }
  {
    NodeStore store(db);  // reopen same db
    json_object* cfg = store.get_config("network", "local-node");
    CHECK(json_object_get_int(json_object_object_get(cfg, "appliedRevision")) == 1,
          "applied revision persisted across reopen");
    json_object_put(cfg);
  }
  std::remove(db.c_str());
}

static void test_registration_local_mode() {
  std::string db = "/tmp/dvs_agent_test_reg.db";
  std::remove(db.c_str());
  NodeStore store(db);
  MediaIpcClient media("");  // unconfigured media IPC
  Registration reg(&store, &media, "" /* empty platform => local autonomous */);
  CHECK(std::string(reg.register_state()) == "unregistered",
        "empty platform URL => unregistered");
  CHECK(reg.online(), "local autonomous mode is online");
  std::remove(db.c_str());
}

static void test_bundle_contract() {
  CHECK(std::string(DVS_NODE_BUNDLE_VERSION) == "1", "bundle version is 1");
  CHECK(DVS_MEDIA_IPC_VERSION == 1, "media IPC version is 1");
  /* Unconfigured engine socket surfaces a structured envelope, never empty. */
  MediaIpcClient media("");
  std::string hr = media.hello();
  json_object* ho = json_tokener_parse(hr.c_str());
  json_object* okv = nullptr;
  json_object* codev = nullptr;
  if (ho) {
    json_object_object_get_ex(ho, "ok", &okv);
    json_object_object_get_ex(ho, "code", &codev);
  }
  CHECK(ho && okv && !json_object_get_boolean(okv), "hello without socket is not ok");
  CHECK(codev && std::string(json_object_get_string(codev)) == "MEDIA_ENGINE_UNCONFIGURED",
        "hello without socket reports UNCONFIGURED");
  if (ho) json_object_put(ho);
}

static void test_http_client_empty_url() {
  HttpClient cli;
  std::string resp;
  CHECK(cli.post_json("", "{}", resp) == -1, "empty URL returns -1 (no throw)");
  CHECK(cli.get("", resp) == -1, "empty URL GET returns -1");
}

int main() {
  test_sha256();
  test_pbkdf2_deterministic();
  test_node_store_password();
  test_node_store_state_machine();
  test_node_store_persistence();
  test_registration_local_mode();
  test_bundle_contract();
  test_http_client_empty_url();

  if (g_failures == 0) {
    printf("\nALL TESTS PASSED\n");
    return 0;
  }
  printf("\n%d TEST(S) FAILED\n", g_failures);
  return 1;
}
