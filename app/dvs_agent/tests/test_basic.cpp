#include "sha256.h"
#include "dvs_node_bundle.h"
#include "node_store.h"
#include "media_ipc.h"
#include "registration.h"
#include "http_client.h"
#include "network_apply.h"
#include "tx_config.h"

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
  CHECK(!media.connected(), "unconfigured client is not connected");
  MediaIpcClient bad("/tmp/dvs_agent_test_nonexistent.sock");
  std::string br = bad.hello();
  json_object* bo = json_tokener_parse(br.c_str());
  json_object* bokv = nullptr;
  json_object* bcodev = nullptr;
  if (bo) {
    json_object_object_get_ex(bo, "ok", &bokv);
    json_object_object_get_ex(bo, "code", &bcodev);
  }
  CHECK(bo && bokv && !json_object_get_boolean(bokv), "hello to bad path is not ok");
  CHECK(bcodev && std::string(json_object_get_string(bcodev)) == "MEDIA_ENGINE_UNREACHABLE",
        "hello to bad path reports UNREACHABLE");
  if (bo) json_object_put(bo);
  CHECK(!bad.connected(), "unreachable client is not connected");
  json_object* evs = bad.drain_events();
  CHECK(evs && json_object_is_type(evs, json_type_array) &&
        json_object_array_length(evs) == 0,
        "drain on unreachable returns empty array");
  if (evs) json_object_put(evs);
}

static void test_http_client_empty_url() {
  HttpClient cli;
  std::string resp;
  CHECK(cli.post_json("", "{}", resp) == -1, "empty URL returns -1 (no throw)");
  CHECK(cli.get("", resp) == -1, "empty URL GET returns -1");
}

static void test_network_apply_parsers() {
  std::string host, err;
  int port = 0;
  CHECK(net_parse_server("192.168.10.200:8080", host, port, err) && host == "192.168.10.200" && port == 8080,
        "server with port parses");
  CHECK(net_parse_server("192.168.10.200", host, port, err) && host == "192.168.10.200" && port == 80,
        "server without port defaults to 80");
  CHECK(net_parse_server("platform.local:5060", host, port, err) && host == "platform.local" && port == 5060,
        "dns server parses");
  CHECK(!net_parse_server("", host, port, err), "empty server rejected");
  CHECK(!net_parse_server("192.168.10.200:99999", host, port, err), "bad server port rejected");
  CHECK(!net_parse_server("bad host!:8080", host, port, err), "bad server host rejected");
  CHECK(!net_parse_server("999.1.1.1", host, port, err), "bad server ip rejected");

  int prefix = 0;
  CHECK(net_mask_to_prefix("255.255.255.0", prefix, err) && prefix == 24, "mask /24");
  CHECK(net_mask_to_prefix("255.255.0.0", prefix, err) && prefix == 16, "mask /16");
  CHECK(!net_mask_to_prefix("255.0.255.0", prefix, err), "non-contiguous mask rejected");
  CHECK(!net_mask_to_prefix("not-a-mask", prefix, err), "garbage mask rejected");

  CHECK(net_valid_iface("eth0"), "eth0 valid");
  CHECK(net_valid_iface("enp0s31f6"), "long iface valid");
  CHECK(!net_valid_iface(""), "empty iface rejected");
  CHECK(!net_valid_iface("eth0;reboot"), "iface injection rejected");
  CHECK(!net_valid_iface(std::string(17, 'a')), "overlong iface rejected");
}

static void test_network_render_and_gate() {
  NetExecParams p;
  p.mode = "dhcp"; p.iface = "eth0"; p.opid = "op-1"; p.server = "10.0.0.1";
  std::string f = net_render_networkd(p);
  CHECK(f.find("DHCP=yes") != std::string::npos, "dhcp renders DHCP=yes");
  CHECK(f.find("Name=eth0") != std::string::npos, "render pins iface");
  p.mode = "static"; p.ip = "192.168.10.51"; p.mask = "255.255.255.0";
  p.gw = "192.168.10.1"; p.dns = "192.168.10.1";
  f = net_render_networkd(p);
  CHECK(f.find("Address=192.168.10.51/24") != std::string::npos, "static renders address/prefix");
  CHECK(f.find("Gateway=192.168.10.1") != std::string::npos, "static renders gateway");

  unsetenv("DVS_ALLOW_NETWORK_APPLY");
  CHECK(!net_execution_requested(true, "op-1"), "gate closed without env");
  setenv("DVS_ALLOW_NETWORK_APPLY", "1", 1);
  CHECK(!net_execution_requested(false, "op-1"), "gate closed without confirm");
  CHECK(!net_execution_requested(true, ""), "gate closed without opid");
  CHECK(net_execution_requested(true, "op-1"), "gate opens with env+confirm+opid");
  unsetenv("DVS_ALLOW_NETWORK_APPLY");
}

static void test_tx_config() {
  std::string err;
  TxParams tp;
  /* nested shape */
  json_object* v = json_object_new_object();
  json_object_object_add(v, "port", json_object_new_string("HDMI1"));
  json_object* enc = json_object_new_object();
  json_object_object_add(enc, "codec", json_object_new_string("h265"));
  json_object_object_add(enc, "width", json_object_new_int(3840));
  json_object_object_add(enc, "height", json_object_new_int(2160));
  json_object_object_add(enc, "fps", json_object_new_int(60));
  json_object_object_add(enc, "bitrateKbps", json_object_new_int(20000));
  json_object_object_add(v, "encode", enc);
  json_object* tgt = json_object_new_object();
  json_object_object_add(tgt, "sip", json_object_new_string("239.0.0.1"));
  json_object_object_add(tgt, "udpPort", json_object_new_int(5000));
  json_object_object_add(v, "target", tgt);
  CHECK(tx_parse(v, tp, err) && tp.hasEncode && tp.hasTarget, "nested tx parses");
  TxCaps frozen;
  CHECK(tx_validate(tp, frozen, err), "max-spec tx validates");
  json_object_put(v);

  /* legacy port-only shape: no tx requested */
  v = json_object_new_object();
  json_object_object_add(v, "port", json_object_new_string("HDMI1"));
  CHECK(tx_parse(v, tp, err) && !tp.hasEncode && !tp.hasTarget, "port-only parses without tx");
  CHECK(tx_validate(tp, frozen, err), "port-only validates (nothing to check)");
  json_object_put(v);

  /* flat fallback with defaults */
  v = json_object_new_object();
  json_object_object_add(v, "port", json_object_new_string("HDMI1"));
  json_object_object_add(v, "targetSip", json_object_new_string("10.0.0.2"));
  json_object_object_add(v, "udpPort", json_object_new_int(6000));
  CHECK(tx_parse(v, tp, err) && !tp.hasEncode && tp.hasTarget, "flat target parses");
  CHECK(tx_validate(tp, frozen, err), "flat target validates");
  json_object_put(v);

  /* rejections */
  v = json_object_new_object();
  json_object_object_add(v, "port", json_object_new_string("HDMI1"));
  json_object* e2 = json_object_new_object();
  json_object_object_add(e2, "codec", json_object_new_string("vp9"));
  json_object_object_add(v, "encode", e2);
  json_object* t2 = json_object_new_object();
  json_object_object_add(t2, "sip", json_object_new_string("10.0.0.2"));
  json_object_object_add(t2, "udpPort", json_object_new_int(6000));
  json_object_object_add(v, "target", t2);
  CHECK(tx_parse(v, tp, err), "bad codec parses (rejected at validate)");
  CHECK(!tx_validate(tp, frozen, err), "bad codec rejected");
  json_object_put(v);

  v = json_object_new_object();
  json_object_object_add(v, "port", json_object_new_string("HDMI1"));
  json_object* e3 = json_object_new_object();
  json_object_object_add(e3, "width", json_object_new_int(8192));
  json_object_object_add(v, "encode", e3);
  json_object* t3 = json_object_new_object();
  json_object_object_add(t3, "sip", json_object_new_string("10.0.0.2"));
  json_object_object_add(t3, "udpPort", json_object_new_int(6000));
  json_object_object_add(v, "target", t3);
  CHECK(tx_parse(v, tp, err), "oversize parses (rejected at validate)");
  CHECK(!tx_validate(tp, frozen, err), "oversize width rejected");
  json_object_put(v);

  v = json_object_new_object();
  json_object_object_add(v, "port", json_object_new_string(""));
  CHECK(!tx_parse(v, tp, err), "empty port rejected at parse");
  json_object_put(v);

  /* caps narrowing: reported wins per-field */
  json_object* caps = json_object_new_object();
  json_object* txc = json_object_new_object();
  json_object* arr = json_object_new_array();
  json_object_array_add(arr, json_object_new_string("h264"));
  json_object_object_add(txc, "codecs", arr);
  json_object_object_add(txc, "maxWidth", json_object_new_int(1920));
  json_object_object_add(caps, "tx", txc);
  TxCaps narrowed = tx_caps_from_node(caps);
  CHECK(narrowed.maxW == 1920 && narrowed.maxH == 2160, "reported maxW narrows, missing maxH keeps frozen");
  TxParams hp;
  hp.port = "HDMI1"; hp.hasEncode = true;
  hp.enc.codec = "h265"; hp.enc.width = 1920; hp.enc.height = 1080;
  hp.enc.fps = 30; hp.enc.bitrate = 4000;
  hp.hasTarget = true; hp.tgt.sip = "10.0.0.2"; hp.tgt.udpPort = 5000;
  CHECK(!tx_validate(hp, narrowed, err), "h265 rejected under reported h264-only caps");
  hp.enc.codec = "h264"; hp.enc.width = 3840;
  CHECK(!tx_validate(hp, narrowed, err), "3840 rejected under reported 1920 cap");
  json_object_put(caps);

  /* payload shape */
  hp.enc.codec = "h264"; hp.enc.width = 1920;
  json_object* pl = tx_start_payload(hp);
  json_object* pe = nullptr;
  CHECK(pl && json_object_object_get_ex(pl, "encode", &pe) && pe, "payload carries encode");
  json_object* pt = nullptr;
  CHECK(pl && json_object_object_get_ex(pl, "target", &pt) && pt, "payload carries target");
  json_object_put(pl);
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
  test_network_apply_parsers();
  test_network_render_and_gate();
  test_tx_config();

  if (g_failures == 0) {
    printf("\nALL TESTS PASSED\n");
    return 0;
  }
  printf("\n%d TEST(S) FAILED\n", g_failures);
  return 1;
}
