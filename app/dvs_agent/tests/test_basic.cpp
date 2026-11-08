#include "sha256.h"
#include "dvs_contract.h"
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

// Forward declarations: JSON field readers used across test functions.
static int tget(json_object* o, const char* k);
static int tget64(json_object* o, const char* k);
static std::string tgets(json_object* o, const char* k);

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

static void test_media_ipc_health_and_gate() {
  /* Failure accounting without an engine: every transport miss bumps the
   * streak and remembers its code; a good answer would reset it. */
  MediaIpcClient bad("/tmp/dvs_agent_test_nonexistent.sock");
  CHECK(bad.fail_streak() == 0, "fresh client has no failures");
  CHECK(bad.last_ok_ms() == 0, "fresh client never succeeded");
  std::string br = bad.hello();
  (void)br;
  CHECK(bad.fail_streak() == 1, "unreachable hello bumps fail streak");
  CHECK(bad.last_code() == "MEDIA_ENGINE_UNREACHABLE", "last code remembers UNREACHABLE");
  CHECK(bad.last_err_ms() > 0, "error time recorded");
  /* Blocking result wait on a dead socket returns promptly with no match. */
  std::string st, lanes;
  CHECK(!bad.wait_prepare_result("op-missing", 200, st, lanes),
        "wait on unreachable finds no result");
  CHECK(!bad.wait_prepare_result("", 200, st, lanes),
        "wait with empty operation id refuses");
  /* Hello verdict matrix for the version gate. */
  {
    bool answered = false, match = false;
    std::string eb;
    int ei = -1;
    CHECK(MediaIpcClient::parse_hello(
              "{\"ok\":true,\"data\":{\"ipcVersion\":1,\"bundleVersion\":\"1\"}}",
              answered, match, eb, ei) &&
              answered && match && eb == "1" && ei == 1,
          "co-released hello matches");
    CHECK(MediaIpcClient::parse_hello(
              "{\"ok\":true,\"data\":{\"ipcVersion\":2,\"bundleVersion\":\"1\"}}",
              answered, match, eb, ei) &&
              answered && !match,
          "ipc generation drift is answered but mismatched");
    CHECK(MediaIpcClient::parse_hello(
              "{\"ok\":true,\"data\":{\"ipcVersion\":1,\"bundleVersion\":\"9\"}}",
              answered, match, eb, ei) &&
              answered && !match,
          "bundle drift is answered but mismatched");
    CHECK(MediaIpcClient::parse_hello(
              "{\"ok\":false,\"code\":\"MEDIA_ENGINE_UNREACHABLE\"}", answered,
              match, eb, ei) &&
              !answered,
          "transport error envelope is not an answer");
    CHECK(!MediaIpcClient::parse_hello("not json", answered, match, eb, ei),
          "garbage hello is unparsable");
  }
}

static void test_http_client_empty_url() {
  HttpClient cli;
  std::string resp;
  CHECK(cli.post_json("", "{}", resp) == -1, "empty URL returns -1 (no throw)");
  CHECK(cli.get("", resp) == -1, "empty URL GET returns -1");
  WsClient ws;
  CHECK(!ws.connect_ws("", "", 200), "ws empty url fails");
  CHECK(!ws.is_open(), "ws not open after fail");
}

static void test_channel_contract() {
  namespace ct = dvs::contract;
  CHECK(ct::next_request_id("a") != ct::next_request_id("a"), "request ids unique");
  CHECK(ct::next_msg_id("a") != ct::next_msg_id("a"), "msg ids unique");
  json_object* d = json_object_new_object();
  json_object* ok = ct::make_ok(d, "ok", "a");
  json_object* okv = nullptr;
  CHECK(ok && json_object_object_get_ex(ok, "ok", &okv) && json_object_get_boolean(okv),
        "contract ok envelope");
  if (ok) json_object_put(ok);
  json_object* f = ct::make_fail("BAD", 400, "bad", "a");
  json_object* fv = nullptr;
  CHECK(f && json_object_object_get_ex(f, "ok", &fv) && !json_object_get_boolean(fv),
        "contract fail envelope");
  if (f) json_object_put(f);
  CHECK(ct::bearer_ok("Bearer tok", "tok"), "bearer ok");
  CHECK(!ct::bearer_ok("", "tok"), "empty bearer rejected when expected");
  CHECK(ct::bearer_ok("anything", ""), "empty expected disables check");
  json_object* h = json_object_new_object();
  json_object_object_add(h, "id", json_object_new_string("n1"));
  json_object* g = json_object_new_object();
  json_object_object_add(g, "x", json_object_new_int(0));
  json_object_object_add(g, "y", json_object_new_int(0));
  json_object_object_add(g, "w", json_object_new_int(1280));
  json_object_object_add(g, "h", json_object_new_int(720));
  json_object_object_add(h, "geometry", g);
  json_object_object_add(h, "capability", json_object_new_object());
  CHECK(ct::validate_hello(h).empty(), "hello valid");
  json_object_put(h);
  json_object* bad = json_object_new_object();
  CHECK(!ct::validate_hello(bad).empty(), "hello missing id rejected");
  json_object_put(bad);
  unsetenv("DVS_TRANSPORT_MODE");
  CHECK(ct::transport_mode() == "auto" && ct::use_ws() && !ct::ws_only(),
        "default transport auto prefers ws");
  setenv("DVS_TRANSPORT_MODE", "short", 1);
  CHECK(!ct::use_ws(), "short disables ws");
  setenv("DVS_TRANSPORT_MODE", "ws", 1);
  CHECK(ct::ws_only(), "ws-only mode");
  unsetenv("DVS_TRANSPORT_MODE");
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

  /* port-only shape: no tx requested */
  v = json_object_new_object();
  json_object_object_add(v, "port", json_object_new_string("HDMI1"));
  CHECK(tx_parse(v, tp, err) && !tp.hasEncode && !tp.hasTarget, "port-only parses without tx");
  CHECK(tx_validate(tp, frozen, err), "port-only validates (nothing to check)");
  json_object_put(v);

  /* flat keys are not a shape: ignored, no tx requested */
  v = json_object_new_object();
  json_object_object_add(v, "port", json_object_new_string("HDMI1"));
  json_object_object_add(v, "targetSip", json_object_new_string("10.0.0.2"));
  json_object_object_add(v, "udpPort", json_object_new_int(6000));
  CHECK(tx_parse(v, tp, err) && !tp.hasEncode && !tp.hasTarget, "flat keys ignored");
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

  /* GOP interval + audio port: defaults omitted, explicit values validated. */
  v = json_object_new_object();
  json_object_object_add(v, "port", json_object_new_string("HDMI1"));
  json_object* enc2 = json_object_new_object();
  json_object_object_add(enc2, "codec", json_object_new_string("h264"));
  json_object_object_add(enc2, "width", json_object_new_int(1920));
  json_object_object_add(enc2, "height", json_object_new_int(1080));
  json_object_object_add(enc2, "fps", json_object_new_int(30));
  json_object_object_add(enc2, "bitrateKbps", json_object_new_int(4000));
  json_object_object_add(enc2, "gopMs", json_object_new_int(500));
  json_object_object_add(v, "encode", enc2);
  json_object* tgt2 = json_object_new_object();
  json_object_object_add(tgt2, "sip", json_object_new_string("239.0.0.1"));
  json_object_object_add(tgt2, "udpPort", json_object_new_int(5000));
  json_object_object_add(tgt2, "audioUdpPort", json_object_new_int(6000));
  json_object_object_add(v, "target", tgt2);
  CHECK(tx_parse(v, tp, err) && tp.enc.gopMs == 500 && tp.tgt.audioUdpPort == 6000,
        "gopMs and audioUdpPort parse");
  CHECK(tx_validate(tp, frozen, err), "gopMs/audio validates");
  json_object* pl2 = tx_start_payload(tp);
  json_object* pe2 = nullptr;
  CHECK(pl2 && json_object_object_get_ex(pl2, "encode", &pe2) && pe2 &&
            tget(pe2, "gopMs") == 500, "payload carries gopMs");
  json_object* pt2 = nullptr;
  CHECK(pl2 && json_object_object_get_ex(pl2, "target", &pt2) && pt2 &&
            tget(pt2, "audioUdpPort") == 6000, "payload carries audioUdpPort");
  json_object_put(pl2);
  tp.enc.gopMs = 100;
  CHECK(!tx_validate(tp, frozen, err), "short gop rejected");
  tp.enc.gopMs = 0;
  tp.tgt.audioUdpPort = 70000;
  CHECK(!tx_validate(tp, frozen, err), "bad audio port rejected");
  json_object_put(v);
}

static void test_role_pending_projection() {
  std::string db = "/tmp/dvs_agent_test_role.db";
  std::remove(db.c_str());
  NodeStore store(db);
  /* pendingRole mirror converges: different role sets pending, same clears. */
  json_object* cur = store.node();
  json_object* rv = nullptr;
  std::string role = "output";
  json_object* rv0 = nullptr;
  if (cur && json_object_object_get_ex(cur, "role", &rv0) && rv0) role = json_object_get_string(rv0);
  CHECK(role == "output", "seed role is output");
  json_object* pp = json_object_new_object();
  json_object_object_add(pp, "pendingRole", json_object_new_string("input"));
  store.update_node(pp);
  json_object_put(pp);
  json_object* n1 = store.node();
  json_object* pr = nullptr;
  CHECK(n1 && json_object_object_get_ex(n1, "pendingRole", &pr) && pr &&
        !json_object_is_type(pr, json_type_null) &&
        std::string(json_object_get_string(pr)) == "input", "pendingRole stored");
  CHECK(store.apply_pending_role(), "reboot promotes pending role");
  json_object* n2 = store.node();
  json_object* r2 = nullptr;
  json_object_object_get_ex(n2, "role", &r2);
  CHECK(r2 && std::string(json_object_get_string(r2)) == "input", "role promoted to input");
  /* registration payloads carry the pending intent for platform cleanup. */
  MediaIpcClient media("");
  Registration reg(&store, &media, "");
  json_object* hb = reg.build_heartbeat();
  json_object* phb = nullptr;
  CHECK(hb && json_object_object_get_ex(hb, "pendingRole", &phb), "heartbeat carries pendingRole");
  json_object* rg = reg.build_register();
  json_object* prg = nullptr;
  CHECK(rg && json_object_object_get_ex(rg, "pendingRole", &prg), "register carries pendingRole");
  if (hb) json_object_put(hb);
  if (rg) json_object_put(rg);
  (void)rv;
  /* playback projection lands on the node object for /info. */
  json_object* pb = json_object_new_object();
  json_object* cur2 = json_object_new_object();
  json_object_object_add(cur2, "status", json_object_new_string("playing"));
  json_object_object_add(cur2, "lanesReady", json_object_new_int(2));
  json_object_object_add(pb, "currentPlayback", cur2);
  store.update_node(pb);
  json_object_put(pb);
  json_object* n3 = store.node();
  json_object* cpb = nullptr;
  CHECK(n3 && json_object_object_get_ex(n3, "currentPlayback", &cpb) && cpb &&
        !json_object_is_type(cpb, json_type_null), "currentPlayback projected");
  std::remove(db.c_str());
}

static json_object* mk_ep(const char* sig, const char* addr, int port,
                          int fps = 60, int audio_port = 0) {
  json_object* e = json_object_new_object();
  json_object_object_add(e, "sig", json_object_new_string(sig));
  json_object_object_add(e, "address", json_object_new_string(addr));
  json_object_object_add(e, "port", json_object_new_int(port));
  if (fps > 0) json_object_object_add(e, "fps", json_object_new_int(fps));
  if (audio_port > 0)
    json_object_object_add(e, "audioPort", json_object_new_int(audio_port));
  return e;
}
static json_object* mk_subwin(const char* id, const char* sig, int dx, int dy,
                              int dw, int dh, int sx, int sy, int sw, int sh) {
  json_object* w = json_object_new_object();
  json_object_object_add(w, "windowId", json_object_new_string(id));
  json_object_object_add(w, "sig", json_object_new_string(sig));
  json_object* dst = json_object_new_object();
  json_object_object_add(dst, "x", json_object_new_int(dx));
  json_object_object_add(dst, "y", json_object_new_int(dy));
  json_object_object_add(dst, "w", json_object_new_int(dw));
  json_object_object_add(dst, "h", json_object_new_int(dh));
  json_object_object_add(w, "dst", dst);
  json_object* src = json_object_new_object();
  json_object_object_add(src, "x", json_object_new_int(sx));
  json_object_object_add(src, "y", json_object_new_int(sy));
  json_object_object_add(src, "w", json_object_new_int(sw));
  json_object_object_add(src, "h", json_object_new_int(sh));
  json_object_object_add(w, "src", src);
  return w;
}
static json_object* mk_body() {
  json_object* b = json_object_new_object();
  json_object* geo = json_object_new_object();
  json_object_object_add(geo, "x", json_object_new_int(0));
  json_object_object_add(geo, "y", json_object_new_int(0));
  json_object_object_add(geo, "w", json_object_new_int(1920));
  json_object_object_add(geo, "h", json_object_new_int(1080));
  json_object_object_add(b, "geometry", geo);
  json_object* eps = json_object_new_array();
  json_object_array_add(eps, mk_ep("s1", "239.0.0.1", 5000));
  json_object_object_add(b, "endpoints", eps);
  json_object_object_add(b, "windows", json_object_new_array());
  return b;
}
static int tget64(json_object* o, const char* k) {
  json_object* v = nullptr;
  if (o && json_object_object_get_ex(o, k, &v) && v &&
      json_object_is_type(v, json_type_int))
    return (int)json_object_get_int64(v);
  return -1;
}
static int tget(json_object* o, const char* k) {
  json_object* v = nullptr;
  if (o && json_object_object_get_ex(o, k, &v) && v)
    return json_object_get_int(v);
  return -9999;
}
static std::string tgets(json_object* o, const char* k) {
  json_object* v = nullptr;
  if (o && json_object_object_get_ex(o, k, &v) && v &&
      json_object_is_type(v, json_type_string))
    return json_object_get_string(v);
  return "";
}

static void test_translate_platform_layout() {
  std::string err;
  json_object* prep = nullptr;
  CHECK(!translate_platform_layout(nullptr, &prep, err), "null body rejected");
  CHECK(prep == nullptr, "rejected body leaves no payload");
  json_object* notobj = json_object_new_array();
  CHECK(!translate_platform_layout(notobj, &prep, err), "non-object rejected");
  json_object_put(notobj);
  // Empty object translates to an empty (clear) stream set.
  json_object* empty = json_object_new_object();
  CHECK(translate_platform_layout(empty, &prep, err), "empty body clears");
  json_object* st0 = nullptr;
  CHECK(prep && json_object_object_get_ex(prep, "streams", &st0) &&
            json_object_array_length(st0) == 0,
        "clear carries empty streams");
  if (prep) json_object_put(prep);
  prep = nullptr;
  json_object_put(empty);
  // Single full window maps 1:1 onto one lane.
  json_object* b = mk_body();
  json_object* wins = nullptr;
  json_object_object_get_ex(b, "windows", &wins);
  json_object_array_add(wins, mk_subwin("w1", "s1", 0, 0, 960, 540, 0, 0, 960, 540));
  CHECK(translate_platform_layout(b, &prep, err), "single window translates");
  json_object* st = nullptr;
  CHECK(prep && json_object_object_get_ex(prep, "streams", &st) &&
            json_object_array_length(st) == 1,
        "one stream emitted");
  json_object* s0 = json_object_array_get_idx(st, 0);
  CHECK(tget(s0, "lane") == 0, "lane zero");
  json_object* mv = nullptr;
  CHECK(json_object_object_get_ex(s0, "main", &mv) && json_object_get_boolean(mv),
        "first lane is main");
  CHECK(tgets(s0, "sip") == "239.0.0.1" && tget(s0, "udpPort") == 5000,
        "endpoint resolved");
  CHECK(tget(s0, "fps") == 60, "fps carried from endpoint table");
  CHECK(tget(s0, "width") == 960 && tget(s0, "height") == 540, "frame size");
  CHECK(tget(s0, "x") == 0 && tget(s0, "y") == 0, "position");
  CHECK(tget(s0, "w") == 960 && tget(s0, "h") == 540, "size");
  CHECK(tget(s0, "srcX") == 0 && tget(s0, "srcY") == 0, "crop origin");
  CHECK(tget(s0, "srcW") == 960 && tget(s0, "srcH") == 540, "crop size");
  if (prep) json_object_put(prep);
  prep = nullptr;
  json_object_put(b);
  // Cross-node window forwards the crop with full-frame dims.
  b = mk_body();
  json_object_object_get_ex(b, "windows", &wins);
  json_object* cw = mk_subwin("w2", "s1", 0, 0, 1920, 1080, 1920, 0, 1920, 1080);
  json_object_object_add(cw, "fullW", json_object_new_int(3840));
  json_object_object_add(cw, "fullH", json_object_new_int(1080));
  json_object_object_add(cw, "partial", json_object_new_boolean(1));
  json_object_array_add(wins, cw);
  CHECK(translate_platform_layout(b, &prep, err), "partial window translates");
  CHECK(prep && json_object_object_get_ex(prep, "streams", &st) &&
            json_object_array_length(st) == 1,
        "partial yields one stream");
  s0 = json_object_array_get_idx(st, 0);
  CHECK(tget(s0, "width") == 3840 && tget(s0, "height") == 1080, "full frame kept");
  CHECK(tget(s0, "srcX") == 1920 && tget(s0, "srcW") == 1920, "crop forwarded");
  if (prep) json_object_put(prep);
  prep = nullptr;
  json_object_put(b);
  // Unknown signal rejects the whole order.
  b = mk_body();
  json_object_object_get_ex(b, "windows", &wins);
  json_object_array_add(wins, mk_subwin("w3", "sx", 0, 0, 100, 100, 0, 0, 100, 100));
  CHECK(!translate_platform_layout(b, &prep, err), "unknown signal rejected");
  CHECK(err.find("not in endpoint") != std::string::npos, "reason names the table");
  CHECK(prep == nullptr, "rejection stages nothing");
  json_object_put(b);
  // Over the lane cap rejects.
  b = mk_body();
  json_object_object_get_ex(b, "windows", &wins);
  for (int i = 0; i < 9; i++)
    json_object_array_add(wins, mk_subwin("w", "s1", 0, 0, 64, 64, 0, 0, 64, 64));
  CHECK(!translate_platform_layout(b, &prep, err), "nine windows rejected");
  json_object_put(b);
  // Degenerate, outside-canvas and non-object windows are skipped.
  b = mk_body();
  json_object_object_get_ex(b, "windows", &wins);
  json_object* deg = json_object_new_object();
  json_object_object_add(deg, "windowId", json_object_new_string("d"));
  json_object_object_add(deg, "sig", json_object_new_string("s1"));
  json_object_array_add(wins, deg);
  json_object_array_add(wins, mk_subwin("o", "s1", 5000, 0, 100, 100, 0, 0, 100, 100));
  json_object_array_add(wins, json_object_new_int(3));
  CHECK(translate_platform_layout(b, &prep, err), "skippable windows pass");
  CHECK(prep && json_object_object_get_ex(prep, "streams", &st) &&
            json_object_array_length(st) == 0,
        "nothing staged for skipped windows");
  if (prep) json_object_put(prep);
  prep = nullptr;
  json_object_put(b);
  // Lanes follow z ascending; envelope fields pass through. The window-id
  // owner resolves to its lane (lo z=1 -> lane 0, hi z=5 -> lane 1).
  b = mk_body();
  json_object_object_add(b, "operationId", json_object_new_string("op-1"));
  json_object_object_add(b, "revision", json_object_new_int(7));
  json_object_object_add(b, "applyAtNs", json_object_new_int64(123));
  json_object_object_add(b, "audioOwner", json_object_new_string("hi"));
  json_object_object_add(b, "volume", json_object_new_int(60));
  json_object_object_get_ex(b, "windows", &wins);
  json_object* hi = mk_subwin("hi", "s1", 0, 0, 100, 100, 0, 0, 100, 100);
  json_object_object_add(hi, "z", json_object_new_int(5));
  json_object* lo = mk_subwin("lo", "s1", 200, 0, 100, 100, 0, 0, 100, 100);
  json_object_object_add(lo, "z", json_object_new_int(1));
  json_object_array_add(wins, hi);
  json_object_array_add(wins, lo);
  CHECK(translate_platform_layout(b, &prep, err), "z-ordered pair translates");
  CHECK(prep && json_object_object_get_ex(prep, "streams", &st) &&
            json_object_array_length(st) == 2,
        "two streams emitted");
  json_object* e0 = json_object_array_get_idx(st, 0);
  json_object* e1 = json_object_array_get_idx(st, 1);
  CHECK(tget(e0, "x") == 200 && tget(e1, "x") == 0, "lanes follow z ascending");
  CHECK(tget(e0, "lane") == 0 && tget(e1, "lane") == 1, "lanes numbered in order");
  CHECK(tgets(prep, "operationId") == "op-1", "operation id passes through");
  CHECK(tget(prep, "revision") == 7, "revision passes through");
  CHECK(tgets(prep, "audioOwner") == "lane1", "window owner resolves to lane");
  CHECK(tget(prep, "audioVolume") == 60, "volume passes through");
  if (prep) json_object_put(prep);
  prep = nullptr;
  json_object_put(b);
  // Owner with no lane on this node is omitted (local mute), not rejected.
  b = mk_body();
  json_object_object_add(b, "audioOwner", json_object_new_string("ghost"));
  json_object_object_get_ex(b, "windows", &wins);
  json_object_array_add(wins, mk_subwin("w1", "s1", 0, 0, 100, 100, 0, 0, 100, 100));
  CHECK(translate_platform_layout(b, &prep, err), "unknown owner still translates");
  json_object* no_owner = nullptr;
  CHECK(prep && !json_object_object_get_ex(prep, "audioOwner", &no_owner),
        "owner without lane omitted");
  if (prep) json_object_put(prep);
  prep = nullptr;
  json_object_put(b);
  // Endpoint entries without a usable port never register.
  b = mk_body();
  json_object* eps = nullptr;
  json_object_object_get_ex(b, "endpoints", &eps);
  json_object* bad = json_object_new_object();
  json_object_object_add(bad, "sig", json_object_new_string("s2"));
  json_object_object_add(bad, "address", json_object_new_string("239.0.0.2"));
  json_object_object_add(bad, "port", json_object_new_int(0));
  json_object_array_add(eps, bad);
  json_object_object_get_ex(b, "windows", &wins);
  json_object_array_add(wins, mk_subwin("w4", "s2", 0, 0, 100, 100, 0, 0, 100, 100));
  CHECK(!translate_platform_layout(b, &prep, err), "portless endpoint rejected");
  json_object_put(b);
  // Endpoint entries without fps reject the order (engine needs the rate).
  b = mk_body();
  json_object_object_get_ex(b, "endpoints", &eps);
  json_object_array_add(eps, mk_ep("s2", "239.0.0.2", 5002, 0));
  json_object_object_get_ex(b, "windows", &wins);
  json_object_array_add(wins, mk_subwin("w5", "s2", 0, 0, 100, 100, 0, 0, 100, 100));
  CHECK(!translate_platform_layout(b, &prep, err), "fpsless endpoint rejected");
  CHECK(err.find("fps") != std::string::npos, "rejection names fps");
  json_object_put(b);
  // audioPort rides the table into audioUdpPort; absent means no key.
  b = mk_body();
  json_object_object_get_ex(b, "endpoints", &eps);
  json_object_array_add(eps, mk_ep("s2", "239.0.0.2", 5002, 30, 6002));
  json_object_object_get_ex(b, "windows", &wins);
  json_object_array_add(wins, mk_subwin("w6", "s2", 0, 0, 100, 100, 0, 0, 100, 100));
  CHECK(translate_platform_layout(b, &prep, err), "audio endpoint translates");
  CHECK(prep && json_object_object_get_ex(prep, "streams", &st) &&
            json_object_array_length(st) == 1,
        "one stream emitted");
  json_object* sa = json_object_array_get_idx(st, 0);
  CHECK(tget(sa, "fps") == 30, "per-signal fps honored");
  CHECK(tget(sa, "audioUdpPort") == 6002, "audio port forwarded");
  if (prep) json_object_put(prep);
  prep = nullptr;
  json_object_put(b);
  // Frame anchor + source filter + still + adjust pass through untouched.
  b = mk_body();
  json_object_object_add(b, "epochTaiNs", json_object_new_int64(900));
  json_object_object_add(b, "fps", json_object_new_double(60.0));
  json_object_object_add(b, "frameIndex", json_object_new_int64(6));
  json_object_object_get_ex(b, "endpoints", &eps);
  json_object* se = json_object_new_object();
  json_object_object_add(se, "sig", json_object_new_string("s9"));
  json_object_object_add(se, "address", json_object_new_string("239.0.0.9"));
  json_object_object_add(se, "port", json_object_new_int(5009));
  json_object_object_add(se, "fps", json_object_new_int(30));
  json_object_object_add(se, "sourceIp", json_object_new_string("192.168.1.9"));
  json_object_array_add(eps, se);
  json_object_object_get_ex(b, "windows", &wins);
  json_object_array_add(wins, mk_subwin("w1", "s1", 0, 0, 960, 540, 0, 0, 960, 540));
  json_object_array_add(wins, mk_subwin("w9", "s9", 0, 0, 100, 100, 0, 0, 100, 100));
  json_object* bi = json_object_new_object();
  json_object_object_add(bi, "revision", json_object_new_int(2));
  json_object_object_add(bi, "url", json_object_new_string("http://x/b.png"));
  json_object_object_add(b, "baseImage", bi);
  json_object* ia = json_object_new_object();
  json_object_object_add(ia, "brightness", json_object_new_int(5));
  json_object_object_add(b, "imageAdjust", ia);
  CHECK(translate_platform_layout(b, &prep, err), "anchored layout translates");
  CHECK(prep && tget64(prep, "epochTaiNs") == 900, "epoch passes through");
  CHECK(tget(prep, "frameIndex") == 6, "frame passes through");
  CHECK(prep && json_object_object_get_ex(prep, "baseImage", &bi) && bi,
        "still passes through");
  CHECK(prep && json_object_object_get_ex(prep, "imageAdjust", &ia) && ia,
        "adjust passes through");
  CHECK(prep && json_object_object_get_ex(prep, "streams", &st) && st &&
            json_object_array_length(st) == 2, "two streams emitted");
  json_object* s9 = json_object_array_get_idx(st, 1);
  CHECK(tgets(s9, "srcIp") == "192.168.1.9", "source filter forwarded");
  if (prep) json_object_put(prep);
  prep = nullptr;
  json_object_put(b);
}

int main() {
  test_sha256();
  test_pbkdf2_deterministic();
  test_node_store_password();
  test_node_store_state_machine();
  test_node_store_persistence();
  test_registration_local_mode();
  test_bundle_contract();
  test_media_ipc_health_and_gate();
  test_http_client_empty_url();
  test_channel_contract();
  test_network_apply_parsers();
  test_network_render_and_gate();
  test_tx_config();
  test_role_pending_projection();
  test_translate_platform_layout();

  if (g_failures == 0) {
    printf("\nALL TESTS PASSED\n");
    return 0;
  }
  printf("\n%d TEST(S) FAILED\n", g_failures);
  return 1;
}
