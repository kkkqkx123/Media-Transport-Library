#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>

#include "layout.h"
#include "ops.h"
#include "platform_context.h"
#include "platform_store.h"
#include "upgrade_jobs.h"

#include "dvs_contract.h"
#include "dvs_node_bundle.h"
#include "http_client.h"
#include "http_server.h"
#include "sha256.h"

static void restore_env(const char* k, const std::string& keep, bool had);

static void test_layout_cell_grid() {
    int cw = -1, ch = -1;
    assert(layout_cell_grid(3840, 2160, 2, 2, &cw, &ch));
    assert(cw == 1920 && ch == 1080);
    assert(layout_cell_grid(10, 10, 3, 3, &cw, &ch));
    assert(cw == 3 && ch == 3);
    assert(layout_cell_grid(10, 10, 2, 2, nullptr, nullptr));
    assert(!layout_cell_grid(0, 10, 1, 1, &cw, &ch));
    assert(!layout_cell_grid(10, 0, 1, 1, &cw, &ch));
    assert(!layout_cell_grid(10, 10, 0, 1, &cw, &ch));
    assert(!layout_cell_grid(10, 10, 1, 0, &cw, &ch));
    assert(!layout_cell_grid(-1, 10, 1, 1, &cw, &ch));
    assert(!layout_cell_grid(10, -1, 1, 1, &cw, &ch));
}

static void test_layout_apply_at() {
    uint64_t at = layout_apply_at_ns(1000000000ULL, 60.0);
    assert(at >= 1000000000ULL + 50000000ULL);

    at = layout_apply_at_ns(10ULL, 1.0);
    assert(at == 10ULL + 2000000000ULL);

    at = layout_apply_at_ns(10ULL, 0.0);
    assert(at == 10ULL + 50000000ULL);
    at = layout_apply_at_ns(10ULL, -3.0);
    assert(at == 10ULL + 50000000ULL);

    at = layout_apply_at_ns(10ULL, 1000000.0);
    assert(at == 10ULL + 50000000ULL);
}

static void test_ops() {
    OpCoordinator c;
    Operation& op = c.begin("op-1", 7, {"n1", "n2"}, 12345);
    assert(op.op_id == "op-1" && op.revision == 7 && op.apply_at_ns == 12345);
    assert(op.state == "preparing");
    assert(c.find("op-1") == &op);
    assert(c.on_reply("op-1", "n1", true, "ready") == "preparing");
    assert(!c.missing("op-1").empty());
    assert(c.missing("op-1").count("n2") == 1);
    assert(c.on_reply("op-1", "n2", true, "ready") == "committed");
    assert(c.missing("op-1").empty());

    {
        OpCoordinator c2;
        c2.begin("op-2", 8, {"n1", "n2"}, 0);
        assert(c2.on_reply("op-2", "n1", false, "blocked") == "blocked");
        int rev = 0;
        std::string rb = c2.rollback_op("op-2", &rev);
        assert(!rb.empty() && rev == 7);
        assert(c2.find("op-2")->state == "rolledback");
        assert(c2.rollback_op("op-2", &rev).empty());
    }

    {
        OpCoordinator c3;
        assert(c3.on_reply("nope", "n1", true, "ready") == "unknown");
        assert(c3.missing("nope").empty());
        int rev = 123;
        assert(c3.rollback_op("nope", &rev).empty());
        assert(c3.find("nope") == nullptr);
    }

    {
        OpCoordinator c4;
        c4.begin("op-4", 1, {"a"}, 0);
        assert(c4.on_reply("op-4", "outsider", true, "ready") == "preparing");
        assert(c4.find("op-4")->ready.count("outsider") == 0);
        assert(c4.on_reply("op-4", "a", true, "ready") == "committed");
    }

    {
        OpCoordinator c5;
        c5.begin("op-5", 1, {"a"}, 0);
        c5.on_reply("op-5", "a", false, "blocked");
        int rev = 0;
        assert(c5.rollback_op("op-5", &rev) == "op-5-rollback");
        assert(rev == 0);
    }
    {
        OpCoordinator c6;
        c6.begin("op-6", 0, {"a"}, 0);
        c6.on_reply("op-6", "a", false, "blocked");
        int rev = 0;
        assert(c6.rollback_op("op-6", &rev) == "op-6-rollback");
        assert(rev == -1);
    }

    {
        OpCoordinator c7;
        c7.begin("op-7", 1, {"a", "b"}, 5);
        c7.on_reply("op-7", "a", true, "ready");
        c7.begin("op-7", 2, {"x"}, 9);
        assert(c7.find("op-7")->revision == 2);
        assert(c7.find("op-7")->members.count("a") == 0);
        assert(c7.find("op-7")->ready.empty());
        assert(c7.find("op-7")->state == "preparing");
    }
}

static void test_store() {
    const char* db = "/tmp/dvs_platform_test_store.db";
    std::remove(db);

    {
        PlatformStore s(db);
        json_object* d = json_object_new_object();
        json_object_object_add(d, "id", json_object_new_string("w1"));
        json_object_object_add(d, "n", json_object_new_int(42));
        assert(s.put_doc("walls", "w1", d));
        json_object_put(d);

        json_object* g = s.get_doc("walls", "w1");
        assert(g != nullptr);
        assert(json_object_get_int(json_object_object_get(g, "n")) == 42);
        json_object_put(g);

        assert(s.list_ids("walls").size() == 1);
        assert(s.list_ids("nope").empty());

        json_object* d2 = json_object_new_object();
        json_object_object_add(d2, "n", json_object_new_int(7));
        assert(s.put_doc("walls", "w1", d2));
        json_object_put(d2);
        json_object* g2 = s.get_doc("walls", "w1");
        assert(json_object_get_int(json_object_object_get(g2, "n")) == 7);
        json_object_put(g2);

        assert(s.put_doc("walls", "w2", nullptr));
        json_object* g3 = s.get_doc("walls", "w2");
        assert(g3 != nullptr);
        assert(json_object_object_length(g3) == 0);
        json_object_put(g3);

        assert(s.del_doc("walls", "w1"));
        assert(s.get_doc("walls", "w1") == nullptr);
        assert(s.del_doc("walls", "w1"));
    }

    {
        PlatformStore s(":memory:");
        assert(s.get_doc("walls", "missing") == nullptr);
    }

    {
        PlatformStore s(":memory:");
        s.audit("{\"e\":1}");
        s.audit("not-json");
        json_object* al = s.audit_list(10);
        assert(al != nullptr);
        json_object* items = nullptr;
        assert(json_object_object_get_ex(al, "items", &items));
        assert(json_object_is_type(items, json_type_array));
        assert(json_object_array_length(items) == 2);
        json_object* first = json_object_array_get_idx(items, 0);
        assert(json_object_object_length(first) == 0);
        json_object* second = json_object_array_get_idx(items, 1);
        assert(json_object_get_int(json_object_object_get(second, "e")) == 1);
        json_object_put(al);

        json_object* al1 = s.audit_list(1);
        json_object* it1 = nullptr;
        json_object_object_get_ex(al1, "items", &it1);
        assert(json_object_array_length(it1) == 1);
        json_object_put(al1);

        json_object* al0 = s.audit_list(0);
        json_object* it0 = nullptr;
        json_object_object_get_ex(al0, "items", &it0);
        assert(json_object_array_length(it0) == 2);
        json_object_put(al0);
    }

    {
        PlatformStore s(":memory:");
        const int64_t ttl = 30LL * 24 * 3600;
        assert(!s.tombstone_check("n9", 1000));
        s.tombstone_add("n9", 1000);
        assert(s.tombstone_check("n9", 1000));
        assert(s.tombstone_check("n9", 1000 + ttl - 1));
        assert(!s.tombstone_check("n9", 1000 + ttl));
        s.tombstone_gc(1000 + ttl);
        assert(!s.tombstone_check("n9", 1000));

        s.tombstone_add("n9", 2000);
        assert(s.tombstone_check("n9", 2000 + ttl - 1));
        assert(!s.tombstone_check("n9", 2000 + ttl));
    }

    {
        {
            PlatformStore s(db);
            json_object* d = json_object_new_object();
            json_object_object_add(d, "x", json_object_new_int(9));
            assert(s.put_doc("nodes", "n1", d));
            json_object_put(d);
        }
        {
            PlatformStore s(db);
            json_object* g = s.get_doc("nodes", "n1");
            assert(g != nullptr);
            assert(json_object_get_int(json_object_object_get(g, "x")) == 9);
            json_object_put(g);
        }
    }

    std::remove(db);
}

static void test_contract() {
    namespace ct = dvs::contract;
    std::string a = ct::next_request_id("p");
    std::string b = ct::next_request_id("p");
    assert(a != b && a.rfind("p-", 0) == 0);
    std::string m1 = ct::next_msg_id("p");
    std::string m2 = ct::next_msg_id("p");
    assert(m1 != m2);
    json_object* d = json_object_new_object();
    json_object* ok = ct::make_ok(d, "hi", "p");
    json_object* okv = nullptr;
    assert(json_object_object_get_ex(ok, "ok", &okv) && json_object_get_boolean(okv));
    json_object_put(ok);
    json_object* f = ct::make_fail("BAD", 400, "no", "p");
    json_object* fv = nullptr;
    assert(json_object_object_get_ex(f, "ok", &fv) && !json_object_get_boolean(fv));
    json_object_put(f);
    assert(ct::bearer_ok("", ""));
    assert(ct::bearer_ok("Bearer x", ""));
    assert(ct::bearer_ok("Bearer s3cr3t", "s3cr3t"));
    assert(!ct::bearer_ok("", "s3cr3t"));
    assert(!ct::bearer_ok("Bearer wrong", "s3cr3t"));
    assert(ct::bearer_ok("s3cr3t", "s3cr3t"));
    // hello
    json_object* h = json_object_new_object();
    json_object_object_add(h, "id", json_object_new_string("n1"));
    json_object* g = json_object_new_object();
    json_object_object_add(g, "x", json_object_new_int(0));
    json_object_object_add(g, "y", json_object_new_int(0));
    json_object_object_add(g, "w", json_object_new_int(1920));
    json_object_object_add(g, "h", json_object_new_int(1080));
    json_object_object_add(h, "geometry", g);
    json_object_object_add(h, "capability", json_object_new_object());
    assert(ct::validate_hello(h).empty());
    json_object* hbad = json_object_new_object();
    assert(!ct::validate_hello(hbad).empty());
    json_object_put(hbad);
    // heartbeat
    json_object* hb = json_object_new_object();
    json_object_object_add(hb, "id", json_object_new_string("n1"));
    json_object* dg = json_object_new_object();
    json_object_object_add(dg, "ready", json_object_new_int(1));
    json_object_object_add(dg, "total", json_object_new_int(2));
    json_object_object_add(dg, "lostFrames", json_object_new_int64(0));
    json_object_object_add(hb, "lanesDigest", dg);
    assert(ct::validate_heartbeat(hb).empty());
    json_object_object_del(hb, "lanesDigest");
    assert(!ct::validate_heartbeat(hb).empty());
    // layout.prepare
    json_object* lp = json_object_new_object();
    json_object_object_add(lp, "operationId", json_object_new_string("op-1"));
    json_object_object_add(lp, "revision", json_object_new_int(3));
    json_object_object_add(lp, "applyAtNs", json_object_new_int64(1000));
    json_object_object_add(lp, "geometry", json_object_get(g));
    json_object_object_add(lp, "windows", json_object_new_array());
    assert(ct::validate_layout_prepare(lp).empty());
    json_object_put(lp);
    // barrier.ack
    json_object* ba = json_object_new_object();
    json_object_object_add(ba, "operationId", json_object_new_string("op-1"));
    json_object_object_add(ba, "node", json_object_new_string("n1"));
    json_object_object_add(ba, "status", json_object_new_string("ready"));
    assert(ct::validate_barrier_ack(ba).empty());
    json_object_object_add(ba, "status", json_object_new_string("bogus"));
    assert(!ct::validate_barrier_ack(ba).empty());
    json_object_put(ba);
    // config.update 只收 geometry/role，未知键直接拒绝
    json_object* cu = json_object_new_object();
    json_object* cc = json_object_new_object();
    json_object_object_add(cc, "geometry", json_object_get(g));
    json_object_object_add(cu, "config", cc);
    assert(ct::validate_config_update(cu).empty());
    json_object* cbad = json_object_new_object();
    json_object* ccb = json_object_new_object();
    json_object_object_add(ccb, "network", json_object_new_object());
    json_object_object_add(cbad, "config", ccb);
    assert(!ct::validate_config_update(cbad).empty());
    json_object_put(cbad);
    json_object_put(cu);
    // upgrade.task
    json_object* ut = json_object_new_object();
    json_object_object_add(ut, "taskId", json_object_new_string("t-1"));
    json_object_object_add(ut, "filename", json_object_new_string("fw.bin"));
    assert(ct::validate_upgrade_task(ut).empty());
    json_object_put(ut);
    // wrap/unwrap
    json_object* wd = json_object_new_object();
    json_object_object_add(wd, "k", json_object_new_int(1));
    json_object* w = ct::wrap("p-m1", ct::kHeartbeat, wd, "p-1");
    std::string mid, tp, rid;
    json_object* dd = nullptr;
    assert(ct::unwrap(w, mid, tp, dd, rid) && mid == "p-m1");
    assert(tp == std::string(ct::kHeartbeat));
    json_object_put(dd);
    json_object_put(w);
    json_object_put(hb);
    json_object_put(h);
}

static void test_contract_gaps() {
    namespace ct = dvs::contract;
    // Heartbeat: id-only identity, digest range, ptp shape.
    {
        json_object* hb = json_object_new_object();
        json_object_object_add(hb, "nodeId", json_object_new_string("n1"));
        assert(!ct::validate_heartbeat(hb).empty());
        json_object_put(hb);
    }
    {
        json_object* hb = json_object_new_object();
        json_object_object_add(hb, "id", json_object_new_string("n1"));
        json_object* dg = json_object_new_object();
        json_object_object_add(dg, "ready", json_object_new_int(0));
        json_object_object_add(dg, "total", json_object_new_int(0));
        json_object_object_add(hb, "lanesDigest", dg);
        assert(ct::validate_heartbeat(hb).empty());
        json_object_put(hb);
    }
    {
        json_object* hb = json_object_new_object();
        json_object_object_add(hb, "id", json_object_new_string("n1"));
        json_object* dg = json_object_new_object();
        json_object_object_add(dg, "ready", json_object_new_int(3));
        json_object_object_add(dg, "total", json_object_new_int(2));
        json_object_object_add(hb, "lanesDigest", dg);
        assert(!ct::validate_heartbeat(hb).empty());
        json_object_put(hb);
    }
    {
        json_object* hb = json_object_new_object();
        json_object_object_add(hb, "id", json_object_new_string("n1"));
        json_object* dg = json_object_new_object();
        json_object_object_add(dg, "ready", json_object_new_string("x"));
        json_object_object_add(dg, "total", json_object_new_int(2));
        json_object_object_add(hb, "lanesDigest", dg);
        assert(!ct::validate_heartbeat(hb).empty());
        json_object_put(hb);
    }
    {
        json_object* hb = json_object_new_object();
        json_object_object_add(hb, "id", json_object_new_string("n1"));
        json_object* dg = json_object_new_object();
        json_object_object_add(dg, "ready", json_object_new_int(1));
        json_object_object_add(dg, "total", json_object_new_int(2));
        json_object_object_add(hb, "lanesDigest", dg);
        json_object_object_add(hb, "ptp", json_object_new_string("x"));
        assert(!ct::validate_heartbeat(hb).empty());
        json_object_object_add(hb, "ptp", json_object_new_object());
        assert(!ct::validate_heartbeat(hb).empty());
        json_object_put(hb);
    }
    // Hello: id-only identity, capability-only caps, ptp-only clock.
    {
        json_object* h = json_object_new_object();
        json_object_object_add(h, "sn", json_object_new_string("n9"));
        json_object* g = json_object_new_object();
        json_object_object_add(g, "x", json_object_new_int(0));
        json_object_object_add(g, "y", json_object_new_int(0));
        json_object_object_add(g, "w", json_object_new_int(10));
        json_object_object_add(g, "h", json_object_new_int(10));
        json_object_object_add(h, "geometry", g);
        assert(!ct::validate_hello(h).empty());
        json_object_put(h);
    }
    {
        json_object* h = json_object_new_object();
        json_object_object_add(h, "id", json_object_new_string("n9"));
        json_object* g = json_object_new_object();
        json_object_object_add(g, "x", json_object_new_int(0));
        json_object_object_add(g, "y", json_object_new_int(0));
        json_object_object_add(g, "w", json_object_new_int(10));
        json_object_object_add(g, "h", json_object_new_int(10));
        json_object_object_add(h, "geometry", g);
        json_object_object_add(h, "capabilities", json_object_new_object());
        assert(!ct::validate_hello(h).empty());
        json_object_put(h);
    }
    {
        json_object* h = json_object_new_object();
        json_object_object_add(h, "id", json_object_new_string("n9"));
        json_object* g = json_object_new_object();
        json_object_object_add(g, "x", json_object_new_int(0));
        json_object_object_add(g, "y", json_object_new_int(0));
        json_object_object_add(g, "w", json_object_new_int(10));
        json_object_object_add(g, "h", json_object_new_int(10));
        json_object_object_add(h, "geometry", g);
        json_object_object_add(h, "capability", json_object_new_object());
        json_object* clk = json_object_new_object();
        json_object_object_add(clk, "state", json_object_new_string("synced"));
        json_object_object_add(clk, "offsetUs", json_object_new_int(5));
        json_object_object_add(h, "clock", clk);
        assert(ct::validate_hello(h).empty());
        json_object_put(h);
    }
    {
        json_object* h = json_object_new_object();
        json_object_object_add(h, "id", json_object_new_string("n1"));
        json_object* g = json_object_new_object();
        json_object_object_add(g, "x", json_object_new_int(0));
        json_object_object_add(g, "y", json_object_new_int(0));
        json_object_object_add(g, "w", json_object_new_string("x"));
        json_object_object_add(g, "h", json_object_new_int(10));
        json_object_object_add(h, "geometry", g);
        assert(!ct::validate_hello(h).empty());
        json_object_put(h);
    }
    {
        json_object* h = json_object_new_object();
        json_object_object_add(h, "id", json_object_new_string("n1"));
        json_object* g = json_object_new_object();
        json_object_object_add(g, "x", json_object_new_int(0));
        json_object_object_add(g, "y", json_object_new_int(0));
        json_object_object_add(g, "w", json_object_new_int(10));
        json_object_object_add(g, "h", json_object_new_int(10));
        json_object_object_add(h, "geometry", g);
        json_object_object_add(h, "network", json_object_new_string("x"));
        assert(!ct::validate_hello(h).empty());
        json_object_put(h);
    }
    // Barrier ack: lanes shape, missing fields.
    {
        json_object* ba = json_object_new_object();
        json_object_object_add(ba, "operationId", json_object_new_string("op-1"));
        json_object_object_add(ba, "node", json_object_new_string("n1"));
        json_object_object_add(ba, "status", json_object_new_string("blocked"));
        json_object_object_add(ba, "lanes", json_object_new_array());
        assert(ct::validate_barrier_ack(ba).empty());
        json_object_object_add(ba, "lanes", json_object_new_string("x"));
        assert(!ct::validate_barrier_ack(ba).empty());
        json_object_put(ba);
    }
    {
        json_object* ba = json_object_new_object();
        json_object_object_add(ba, "node", json_object_new_string("n1"));
        json_object_object_add(ba, "status", json_object_new_string("ready"));
        assert(!ct::validate_barrier_ack(ba).empty());
        json_object_put(ba);
    }
    // layout.prepare: missing operation id, bad revision type.
    {
        json_object* lp = json_object_new_object();
        json_object_object_add(lp, "revision", json_object_new_int(1));
        json_object_object_add(lp, "applyAtNs", json_object_new_int64(2));
        json_object_object_add(lp, "windows", json_object_new_array());
        json_object* g = json_object_new_object();
        json_object_object_add(g, "x", json_object_new_int(0));
        json_object_object_add(g, "y", json_object_new_int(0));
        json_object_object_add(g, "w", json_object_new_int(1));
        json_object_object_add(g, "h", json_object_new_int(1));
        json_object_object_add(lp, "geometry", g);
        assert(!ct::validate_layout_prepare(lp).empty());
        json_object_put(lp);
    }
    // config.ack and upgrade.progress: missing identity fields.
    {
        json_object* ca = json_object_new_object();
        assert(!ct::validate_config_ack(ca).empty());
        json_object_object_add(ca, "node", json_object_new_string("n1"));
        assert(ct::validate_config_ack(ca).empty());
        json_object_object_add(ca, "appliedRevision", json_object_new_string("x"));
        assert(!ct::validate_config_ack(ca).empty());
        json_object_put(ca);
    }
    {
        json_object* up = json_object_new_object();
        assert(!ct::validate_upgrade_progress(up).empty());
        json_object_object_add(up, "taskId", json_object_new_string("t"));
        json_object_object_add(up, "node", json_object_new_string("n"));
        assert(!ct::validate_upgrade_progress(up).empty());
        json_object_object_add(up, "status", json_object_new_string("done"));
        assert(ct::validate_upgrade_progress(up).empty());
        json_object_put(up);
    }
    // unwrap rejects a typeless frame.
    {
        json_object* m = json_object_new_object();
        std::string mid, tp, rid;
        json_object* dd = nullptr;
        assert(!ct::unwrap(m, mid, tp, dd, rid));
        json_object_put(m);
    }
}

static void test_ws_codec() {
    // RFC6455 附录握手向量。
    assert(HttpServer::ws_accept_key("dGhlIHNhbXBsZSBub25jZQ==") ==
           "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=");
    // 短帧往返（服务端不掩码）。
    std::string hello = "{\"type\":\"hello\"}";
    assert(HttpServer::ws_send_frame(9999, 0x1, hello, false) == false);  // 坏 fd 失败但不崩
    // 用 socketpair 做环回：发送掩码帧，接收解码。
    int sv[2];
    assert(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    assert(HttpServer::ws_send_text(sv[0], hello, true));
    uint8_t op = 0;
    std::string out;
    assert(HttpServer::ws_recv_frame(sv[1], op, out, 1000) && op == 0x1 && out == hello);
    assert(HttpServer::ws_send_text(sv[0], hello, false));
    assert(HttpServer::ws_recv_frame(sv[1], op, out, 1000) && out == hello);
    // 126 与 127 长度边界。
    std::string mid(200, 'x');
    assert(HttpServer::ws_send_text(sv[0], mid, true));
    assert(HttpServer::ws_recv_frame(sv[1], op, out, 1000) && out == mid);
    std::string big(70000, 'y');
    assert(HttpServer::ws_send_text(sv[0], big, true));
    assert(HttpServer::ws_recv_frame(sv[1], op, out, 2000) && out == big);
    // ping 自动回 pong：对端发 ping，本端 ws_recv_text 应回 pong 并继续。
    assert(HttpServer::ws_send_frame(sv[0], 0x9, "ping", false));
    assert(HttpServer::ws_send_text(sv[0], hello, false));
    // sv[1] 先收到 ping 会自动回 pong，再收到 text。
    std::string txt;
    assert(HttpServer::ws_recv_text(sv[1], txt, 1000, false) && txt == hello);
    ::close(sv[0]); ::close(sv[1]);
}

static void test_ops_deadline() {
    OpCoordinator c;
    c.begin_with_deadline("op-ws-1", 1, {"a", "b"}, 100, 200);
    assert(c.find("op-ws-1")->deadline_ns == 200);
    assert(c.expire_sweep(199).empty());
    auto exp = c.expire_sweep(200);
    assert(exp.size() == 1 && exp[0] == "op-ws-1");
    assert(c.find("op-ws-1")->state == "blocked");
    int rev = 0;
    assert(!c.rollback_op("op-ws-1", &rev).empty() && rev == 0);
    // 无 deadline 永不超时。
    c.begin("op-ws-2", 1, {"a"}, 0);
    assert(c.expire_sweep(999999).empty());
}

static void test_op_serialization() {
    Operation op;
    op.op_id = "op-x";
    op.revision = 7;
    op.apply_at_ns = 123456789ULL;
    op.deadline_ns = 223456789ULL;
    op.created_ns = 1000ULL;
    op.state = "preparing";
    op.windows_json = "[{\"x\":1}]";
    op.wall_w = 3840;
    op.wall_h = 2160;
    op.members = {"n1", "n2"};
    op.ready = {"n1"};
    op.result["n1"] = "ready";

    op.wall_id = "wall-9";
    op.scene_id = "scene-9";
    op.layout_hash = "hash-9";
    op.volume = 42;
    json_object* rec = operation_to_json(op);
    assert(rec != nullptr);
    Operation back;
    assert(operation_from_json(rec, &back));
    assert(back.op_id == op.op_id && back.revision == op.revision);
    assert(back.apply_at_ns == op.apply_at_ns && back.deadline_ns == op.deadline_ns);
    assert(back.created_ns == op.created_ns && back.state == op.state);
    assert(back.windows_json == op.windows_json);
    assert(back.wall_w == op.wall_w && back.wall_h == op.wall_h);
    assert(back.members == op.members && back.ready == op.ready);
    assert(back.result.size() == 1 && back.result["n1"] == "ready");
    assert(back.wall_id == "wall-9" && back.scene_id == "scene-9");
    assert(back.layout_hash == "hash-9" && back.volume == 42);
    json_object_put(rec);
    // Volume absent on the wire (older writer) still decodes to the default.
    {
        json_object* legacy = json_object_new_object();
        json_object_object_add(legacy, "opId", json_object_new_string("op-legacy"));
        json_object_object_add(legacy, "state", json_object_new_string("preparing"));
        Operation old;
        assert(operation_from_json(legacy, &old));
        assert(old.volume == 80 && old.wall_id.empty());
        json_object_put(legacy);
    }

    // Missing/empty required fields are rejected.
    assert(!operation_from_json(nullptr, &back));
    json_object* empty = json_object_new_object();
    assert(!operation_from_json(empty, &back));
    json_object_put(empty);
}

static void test_ops_persistence() {
    const char* db = "/tmp/dvs_platform_test_ops.db";
    std::remove(db);

    // Coordinator with a store persists every state change.
    {
        PlatformStore s(db);
        OpCoordinator c;
        c.set_store(&s);
        Operation& op = c.begin_with_deadline("op-1", 3, {"a", "b"}, 0, 999);
        op.windows_json = "[{\"x\":1}]";
        op.wall_w = 3840;
        op.wall_h = 2160;
        op.created_ns = 1000;
        c.persist("op-1");  // direct stamps bypass the coordinator: persist like main.cpp does
        json_object* d = s.get_doc("op", "op-1");
        assert(d != nullptr);
        {
            Operation back;
            assert(operation_from_json(d, &back));
            assert(back.state == "preparing" && back.ready.empty());
            assert(back.deadline_ns == 999 && back.windows_json == op.windows_json);
        }
        json_object_put(d);
        c.on_reply("op-1", "a", true, "ready");
        d = s.get_doc("op", "op-1");
        {
            Operation back;
            assert(operation_from_json(d, &back));
            assert(back.ready.size() == 1);
        }
        json_object_put(d);
        c.on_reply("op-1", "b", true, "ready");
        d = s.get_doc("op", "op-1");
        {
            Operation back;
            assert(operation_from_json(d, &back));
            assert(back.state == "committed");
        }
        json_object_put(d);
    }

    // "Restart": fresh store handle + fresh coordinator; restore() brings
    // committed snapshots back and find_committed() still works.
    {
        PlatformStore s(db);
        OpCoordinator c;
        c.set_store(&s);
        for (const auto& id : s.list_ids("op")) {
            json_object* d = s.get_doc("op", id);
            assert(d != nullptr);
            Operation op;
            assert(operation_from_json(d, &op));
            c.restore(op);
            json_object_put(d);
        }
        const Operation* snap = c.find_committed(3);
        assert(snap != nullptr && snap->op_id == "op-1");
        assert(snap->state == "committed" && !snap->windows_json.empty());

        // A blocked reply on the restored op rolls back against revision 2.
        c.begin("op-2", 4, {"a"}, 0);
        if (Operation* m = c.mutable_find("op-2")) {
            // Production stamps created_ns right after begin (see main.cpp
            // fanout); without it gc() treats the op as ageless and keeps
            // its doc forever.
            m->created_ns = 2000;
            c.persist("op-2");
        }
        assert(c.on_reply("op-2", "a", false, "blocked") == "blocked");
        int rev = 0;
        std::string rb = c.rollback_op("op-2", &rev);
        assert(!rb.empty() && rev == 3);
        assert(c.find_committed(3) != nullptr);
        // The rollback marker clears the replay scan.
        assert(c.pending_rollbacks().empty());
    }

    // Crash window: blocked op persisted WITHOUT the rollback marker, then a
    // fresh coordinator restores it — pending_rollbacks() must surface it,
    // the replayed rollback_op() must work, and the marker must persist.
    {
        PlatformStore s(db);
        {
            // Simulate the crash: blocked state on disk, no marker.
            json_object* d = json_object_new_object();
            json_object_object_add(d, "opId", json_object_new_string("op-hang"));
            json_object_object_add(d, "revision", json_object_new_int(5));
            json_object_object_add(d, "state", json_object_new_string("blocked"));
            json_object* members = json_object_new_array();
            json_object_array_add(members, json_object_new_string("a"));
            json_object_object_add(d, "members", members);
            json_object_object_add(d, "windowsJson",
                                   json_object_new_string("[{\"x\":1}]"));
            json_object_object_add(d, "wallW", json_object_new_int(3840));
            json_object_object_add(d, "wallH", json_object_new_int(2160));
            assert(s.put_doc("op", "op-hang", d));
            json_object_put(d);
        }
        OpCoordinator c;
        c.set_store(&s);
        for (const auto& id : s.list_ids("op")) {
            json_object* d = s.get_doc("op", id);
            assert(d != nullptr);
            Operation op;
            if (operation_from_json(d, &op)) c.restore(op);
            json_object_put(d);
        }
        assert(c.pending_rollbacks().size() == 1);
        assert(c.pending_rollbacks()[0] == "op-hang");
        int rev = 0;
        assert(!c.rollback_op("op-hang", &rev).empty());
        assert(rev == 4);
        assert(c.pending_rollbacks().empty());
        json_object* d = s.get_doc("op", "op-hang");
        {
            Operation op;
            assert(operation_from_json(d, &op));
            assert(op.state == "rolledback");
            assert(op.result.count("__rolledback__") == 1);
        }
        json_object_put(d);
    }

    // gc removes terminal ops past ttl and their docs, keeps preparing ones.
    // All persisted ops are restored first (as production does at startup),
    // otherwise gc — which only collects in-memory ops — could never reach
    // their docs.
    {
        PlatformStore s(db);
        OpCoordinator c;
        c.set_store(&s);
        for (const auto& id : s.list_ids("op")) {
            json_object* d = s.get_doc("op", id);
            assert(d != nullptr);
            Operation op;
            if (operation_from_json(d, &op)) c.restore(op);
            json_object_put(d);
        }
        // op-1 (committed) and op-2 (rolledback) are ancient and collected;
        // op-hang (rolledback but ageless: createdNs unknown) is kept.
        assert(c.gc(999999999ULL * 1000000000ULL, 3600ULL * 1000000000ULL).size() == 2);
        assert(s.get_doc("op", "op-1") == nullptr);
        assert(s.get_doc("op", "op-2") == nullptr);

        OpCoordinator c2;
        c2.set_store(&s);
        Operation& p = c2.begin("op-live", 1, {"a"}, 0);
        (void)p;
        assert(c2.gc(999999999ULL * 1000000000ULL, 3600ULL * 1000000000ULL).empty());
        assert(s.get_doc("op", "op-live") != nullptr);
    }

    std::remove(db);
}

static void test_frame_anchor() {
    namespace ct = dvs::contract;
    // Frame math: applyAt maps to the containing frame; frame start inverts.
    assert(ct::frame_index_for_apply_at(1000000000ULL, 60.0, 1000000000ULL) == 0);
    assert(ct::frame_index_for_apply_at(1000000000ULL, 60.0,
                                        1000000000ULL + 16666666ULL) == 0);
    assert(ct::frame_index_for_apply_at(1000000000ULL, 60.0,
                                        1000000000ULL + 16666667ULL) == 1);
    assert(ct::frame_index_for_apply_at(1000000000ULL, 60.0, 999ULL) == 0);
    assert(ct::frame_index_for_apply_at(0ULL, 60.0, 5000000000ULL) == 300);
    assert(ct::frame_start_ns(1000000000ULL, 60.0, 0) == 1000000000ULL);
    uint64_t s1 = ct::frame_start_ns(1000000000ULL, 60.0, 1);
    assert(s1 > 1000000000ULL && s1 < 1000000000ULL + 20000000ULL);
    assert(ct::frame_index_for_apply_at(1000000000ULL, 60.0, s1) == 1);
    assert(ct::frame_index_for_apply_at(0ULL, 0.0, 5000ULL) == 0);
    // Contract accepts the anchored payloads.
    {
        json_object* lp = json_object_new_object();
        json_object_object_add(lp, "operationId", json_object_new_string("op-1"));
        json_object_object_add(lp, "revision", json_object_new_int(3));
        json_object_object_add(lp, "applyAtNs", json_object_new_int64(1000));
        json_object_object_add(lp, "epochTaiNs", json_object_new_int64(900));
        json_object_object_add(lp, "fps", json_object_new_double(60.0));
        json_object_object_add(lp, "frameIndex", json_object_new_int64(6));
        json_object_object_add(lp, "windows", json_object_new_array());
        json_object* g = json_object_new_object();
        json_object_object_add(g, "x", json_object_new_int(0));
        json_object_object_add(g, "y", json_object_new_int(0));
        json_object_object_add(g, "w", json_object_new_int(10));
        json_object_object_add(g, "h", json_object_new_int(10));
        json_object_object_add(lp, "geometry", g);
        json_object* eps = json_object_new_array();
        json_object* e = json_object_new_object();
        json_object_object_add(e, "sig", json_object_new_string("s"));
        json_object_object_add(e, "address", json_object_new_string("239.0.0.1"));
        json_object_object_add(e, "sourceIp", json_object_new_string("192.168.1.2"));
        json_object_array_add(eps, e);
        json_object_object_add(lp, "endpoints", eps);
        json_object* bi = json_object_new_object();
        json_object_object_add(bi, "revision", json_object_new_int(2));
        json_object_object_add(bi, "url", json_object_new_string("http://x/b.png"));
        json_object_object_add(lp, "baseImage", bi);
        json_object* ia = json_object_new_object();
        json_object_object_add(ia, "brightness", json_object_new_int(10));
        json_object_object_add(lp, "imageAdjust", ia);
        assert(ct::validate_layout_prepare(lp).empty());
        json_object* bad = json_object_get(e);
        json_object_object_add(bad, "sourceIp", json_object_new_string("999.1.1.1"));
        assert(!ct::validate_layout_prepare(lp).empty());
        json_object_put(bad);
        json_object_put(lp);
    }
    {
        json_object* lp = json_object_new_object();
        json_object_object_add(lp, "operationId", json_object_new_string("op-1"));
        json_object_object_add(lp, "revision", json_object_new_int(1));
        json_object_object_add(lp, "applyAtNs", json_object_new_int64(1));
        json_object_object_add(lp, "epochTaiNs", json_object_new_int64(-5));
        json_object_object_add(lp, "windows", json_object_new_array());
        json_object* g = json_object_new_object();
        json_object_object_add(g, "x", json_object_new_int(0));
        json_object_object_add(g, "y", json_object_new_int(0));
        json_object_object_add(g, "w", json_object_new_int(1));
        json_object_object_add(g, "h", json_object_new_int(1));
        json_object_object_add(lp, "geometry", g);
        assert(!ct::validate_layout_prepare(lp).empty());
        json_object_put(lp);
    }
    // Barrier ack carries the commit report.
    {
        json_object* ba = json_object_new_object();
        json_object_object_add(ba, "operationId", json_object_new_string("op-1"));
        json_object_object_add(ba, "node", json_object_new_string("n1"));
        json_object_object_add(ba, "status", json_object_new_string("ready"));
        json_object_object_add(ba, "frameIndex", json_object_new_int64(9));
        json_object_object_add(ba, "appliedAtTaiNs", json_object_new_int64(1234));
        json_object_object_add(ba, "layoutHash", json_object_new_string("abc"));
        assert(ct::validate_barrier_ack(ba).empty());
        json_object_object_add(ba, "frameIndex", json_object_new_int64(-1));
        assert(!ct::validate_barrier_ack(ba).empty());
        json_object_put(ba);
    }
    // Canonical media error codes exist.
    assert(std::string(ct::media_err::kDecodeNotReady) == "DECODE_NOT_READY");
    assert(std::string(ct::media_err::kFrameDeadlineMissed) == "FRAME_DEADLINE_MISSED");
    // Whitelist source filter.
    {
        json_object* w = json_object_new_object();
        json_object_object_add(w, "sig", json_object_new_string("s1"));
        json_object_object_add(w, "address", json_object_new_string("239.0.0.1"));
        assert(validate_whitelist_shape(w).empty());
        json_object_object_add(w, "sourceIp",
                               json_object_new_string("192.168.10.11"));
        assert(validate_whitelist_shape(w).empty());
        json_object_object_add(w, "sourceIp", json_object_new_string("nope"));
        assert(!validate_whitelist_shape(w).empty());
        json_object_put(w);
    }
    // Image adjust + base image shapes.
    {
        json_object* ia = json_object_new_object();
        assert(!validate_image_adjust(ia).empty());
        json_object_object_add(ia, "contrast", json_object_new_int(200));
        assert(!validate_image_adjust(ia).empty());
        json_object_object_add(ia, "contrast", json_object_new_int(-20));
        assert(validate_image_adjust(ia).empty());
        json_object_put(ia);
        json_object* bi = json_object_new_object();
        assert(!validate_base_image(bi).empty());
        json_object_object_add(bi, "revision", json_object_new_int(1));
        json_object_object_add(bi, "url", json_object_new_string("http://x/b.png"));
        assert(validate_base_image(bi).empty());
        json_object* sub = json_object_new_object();
        json_object_object_add(sub, "x", json_object_new_int(0));
        json_object_object_add(sub, "y", json_object_new_int(0));
        json_object_object_add(bi, "sub", sub);
        assert(!validate_base_image(bi).empty());
        json_object_put(bi);
    }
    // Decode budget + signal counting + fixed snap + wall cells.
    {
        json_object* nd = json_object_new_object();
        assert(node_decode_budget(nd) < 0);
        json_object* cap = json_object_new_object();
        json_object_object_add(cap, "maxDecodeSessions", json_object_new_int(2));
        json_object_object_add(nd, "capability", cap);
        assert(node_decode_budget(nd) == 2);
        json_object_put(nd);
        json_object* wins = json_object_new_array();
        for (int i = 0; i < 3; i++) {
            json_object* w = json_object_new_object();
            char id[8];
            snprintf(id, sizeof(id), "w%d", i);
            json_object_object_add(w, "id", json_object_new_string(id));
            json_object_object_add(w, "sig", json_object_new_string(i < 2 ? "s1" : "s2"));
            json_object_object_add(w, "x", json_object_new_int(i * 100));
            json_object_object_add(w, "y", json_object_new_int(0));
            json_object_object_add(w, "w", json_object_new_int(100));
            json_object_object_add(w, "h", json_object_new_int(100));
            json_object_array_add(wins, w);
        }
        assert(count_node_signals(wins, Rect{0, 0, 1000, 1000}) == 2);
        json_object* wall = json_object_new_object();
        json_object_object_add(wall, "rows", json_object_new_int(1));
        json_object_object_add(wall, "cols", json_object_new_int(2));
        std::vector<std::pair<std::string, Rect>> cells;
        wall_cells(wall, 3840, 2160, &cells);
        assert(cells.size() == 2);
        assert(cells[1].second.x == 1920);
        assert(wall_min_window_side(wall, 3840, 2160) == 480);
        json_object* snapped = snap_windows_to_cells(wins, cells);
        assert(json_object_array_length(snapped) == 3);
        json_object* s0 = json_object_array_get_idx(snapped, 0);
        json_object* xv = nullptr;
        assert(json_object_object_get_ex(s0, "x", &xv) && xv &&
               json_object_get_int(xv) == 0);
        json_object* s2 = json_object_array_get_idx(snapped, 2);
        assert(json_object_object_get_ex(s2, "x", &xv) && xv &&
               json_object_get_int(xv) == 0);
        json_object_put(snapped);
        json_object_put(wall);
        json_object_put(wins);
    }
    // Operation persistence carries the frame base (legacy docs stay readable).
    {
        Operation op;
        op.op_id = "op-f";
        op.revision = 5;
        op.apply_at_ns = 777;
        op.state = "preparing";
        op.epoch_ns = 700;
        op.fps = 50.0;
        op.frame_index = 3;
        json_object* j = operation_to_json(op);
        Operation back;
        assert(operation_from_json(j, &back));
        assert(back.epoch_ns == 700 && back.fps == 50.0 && back.frame_index == 3);
        json_object_put(j);
        json_object* legacy = json_object_new_object();
        json_object_object_add(legacy, "opId", json_object_new_string("op-l"));
        json_object_object_add(legacy, "state", json_object_new_string("preparing"));
        Operation old;
        assert(operation_from_json(legacy, &old));
        assert(old.epoch_ns == 0 && old.frame_index == -1 && old.fps == 60.0);
        json_object_put(legacy);
    }
}

static void test_common() {
    uint8_t out[32];
    dvs::sha256(nullptr, 0, out);
    assert(dvs::to_hex(out, 32) ==
           "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

    const char* msg = "abc";
    dvs::sha256((const uint8_t*)msg, 3, out);
    assert(dvs::to_hex(out, 32) ==
           "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");

    assert(std::string(DVS_NODE_BUNDLE_VERSION) == "1");
    assert(DVS_MEDIA_IPC_VERSION == 1);

    HttpClient cli;
    std::string resp;
    assert(cli.post_json("", "{}", resp) == -1);
    assert(cli.get("", resp) == -1);
}

static void test_business_rules() {
    // Groups.
    {
        json_object* g = json_object_new_object();
        assert(!validate_group_shape(g).empty());
        json_object_object_add(g, "name", json_object_new_string("  A  "));
        assert(validate_group_shape(g).empty());
        assert(docs_lower(docs_trim("  A  ")) == "a");
        json_object_put(g);
    }
    // Walls.
    {
        json_object* w = json_object_new_object();
        assert(!validate_wall_shape(w).empty());
        json_object_object_add(w, "name", json_object_new_string("w1"));
        json_object_object_add(w, "rows", json_object_new_int(2));
        json_object_object_add(w, "cols", json_object_new_int(2));
        assert(validate_wall_shape(w).empty());
        json_object_object_add(w, "rows", json_object_new_int(7));
        assert(!validate_wall_shape(w).empty());
        json_object_object_del(w, "rows");
        json_object_object_add(w, "rows", json_object_new_int(1));
        json_object_object_add(w, "mode", json_object_new_string("BAD"));
        assert(!validate_wall_shape(w).empty());
        json_object_object_del(w, "mode");
        json_object_object_add(w, "mode", json_object_new_string("LED"));
        json_object* rp = json_object_new_array();
        json_object_array_add(rp, json_object_new_int(1080));
        json_object_object_add(w, "rowPixels", rp);
        assert(validate_wall_shape(w).empty());
        json_object_put(w);
        json_object* d = json_object_new_object();
        json_object_object_add(d, "deleted", json_object_new_boolean(1));
        assert(wall_is_deleted(d));
        json_object_put(d);
        json_object* nd = json_object_new_object();
        assert(!wall_is_deleted(nd));
        json_object_put(nd);
    }
    // Scenes.
    {
        json_object* s = json_object_new_object();
        json_object_object_add(s, "snapshotVersion", json_object_new_int(2));
        assert(!validate_scene_shape(s, 0, 0).empty());
        json_object_object_del(s, "snapshotVersion");
        json_object_object_add(s, "snapshotVersion", json_object_new_int(1));
        json_object_object_add(s, "volume", json_object_new_int(120));
        assert(!validate_scene_shape(s, 0, 0).empty());
        json_object_object_del(s, "volume");
        json_object_object_add(s, "volume", json_object_new_int(80));
        json_object* wins = json_object_new_array();
        json_object* w0 = json_object_new_object();
        json_object_object_add(w0, "id", json_object_new_string("w1"));
        json_object_object_add(w0, "sig", json_object_new_string("s1"));
        json_object_object_add(w0, "x", json_object_new_int(0));
        json_object_object_add(w0, "y", json_object_new_int(0));
        json_object_object_add(w0, "w", json_object_new_int(100));
        json_object_object_add(w0, "h", json_object_new_int(100));
        json_object_array_add(wins, w0);
        json_object_object_add(s, "windows", wins);
        assert(validate_scene_shape(s, 0, 0).empty());
        assert(!validate_scene_shape(s, 50, 50).empty());
        assert(validate_scene_shape(s, 1920, 1080).empty());
        json_object_put(s);
    }
    // Bindings / whitelist / playback shapes.
    {
        json_object* b = json_object_new_object();
        assert(!validate_binding_shape(b).empty());
        json_object_object_add(b, "wallId", json_object_new_string("w1"));
        json_object_object_add(b, "nodeId", json_object_new_string("n1"));
        assert(validate_binding_shape(b).empty());
        json_object_put(b);
        json_object* wl = json_object_new_object();
        assert(!validate_whitelist_shape(wl).empty());
        json_object_object_add(wl, "sig", json_object_new_string("s1"));
        json_object_object_add(wl, "address", json_object_new_string("239.10.0.1"));
        assert(validate_whitelist_shape(wl).empty());
        json_object_object_add(wl, "port", json_object_new_int(99999));
        assert(!validate_whitelist_shape(wl).empty());
        json_object_put(wl);
        json_object* t = json_object_new_object();
        assert(!validate_playback_shape(t).empty());
        json_object_object_add(t, "wallId", json_object_new_string("w1"));
        json_object_object_add(t, "sceneId", json_object_new_string("s1"));
        json_object_object_add(t, "targetNodeId", json_object_new_string("n1"));
        assert(validate_playback_shape(t).empty());
        json_object_object_add(t, "status", json_object_new_string("bogus"));
        assert(!validate_playback_shape(t).empty());
        json_object_put(t);
    }
    // Hash + dirty compare + subrect + clock gate.
    {
        json_object* a = json_object_new_array();
        json_object* b = json_object_new_array();
        assert(layout_windows_hash(a) == layout_windows_hash(b));
        json_object* e = json_object_new_object();
        json_object_object_add(e, "id", json_object_new_string("w1"));
        json_object_array_add(b, e);
        assert(layout_windows_hash(a) != layout_windows_hash(b));
        json_object* d = layout_dirty_compare(a, 80, b, 80);
        json_object* dv = nullptr;
        assert(json_object_object_get_ex(d, "dirty", &dv) && json_object_get_boolean(dv));
        json_object_put(d);
        json_object* d2 = layout_dirty_compare(a, 80, a, 80);
        assert(json_object_object_get_ex(d2, "dirty", &dv) && !json_object_get_boolean(dv));
        json_object_put(d2);
        json_object_put(a);
        json_object_put(b);
        Rect sub;
        assert(base_subrect(3840, 2160, Rect{1920, 0, 1920, 1080}, &sub));
        assert(sub.x == 1920 && sub.w == 1920);
        assert(!base_subrect(3840, 2160, Rect{5000, 0, 100, 100}, &sub));
        assert(ptp_max_offset_us() > 0);
        json_object* n = json_object_new_object();
        std::string reason;
        assert(!node_clock_ok(n, &reason));
        json_object_object_add(n, "online", json_object_new_boolean(1));
        json_object_object_add(n, "role", json_object_new_string("output"));
        assert(!node_clock_ok(n, &reason));
        json_object* ptp = json_object_new_object();
        json_object_object_add(ptp, "state", json_object_new_string("synced"));
        json_object_object_add(ptp, "offsetUs", json_object_new_int(100));
        json_object_object_add(n, "ptp", ptp);
        assert(node_clock_ok(n, &reason));
        json_object_put(n);
    }
}

static void test_validation_gaps() {
    // String helpers.
    assert(docs_trim("   \t\n ").empty());
    assert(docs_trim("").empty());
    assert(docs_lower("AbC DeF") == "abc def");
    // Groups.
    assert(!validate_group_shape(nullptr).empty());
    {
        json_object* g = json_object_new_string("x");
        assert(!validate_group_shape(g).empty());
        json_object_put(g);
    }
    {
        json_object* g = json_object_new_object();
        json_object_object_add(g, "name", json_object_new_string("   "));
        assert(!validate_group_shape(g).empty());
        json_object_put(g);
    }
    // Walls: bounds, enums, pixel arrays, cell grid shape.
    assert(!validate_wall_shape(nullptr).empty());
    {
        json_object* w = json_object_new_object();
        json_object_object_add(w, "name", json_object_new_string("w"));
        assert(validate_wall_shape(w).empty());
        json_object_object_add(w, "rows", json_object_new_int(0));
        assert(!validate_wall_shape(w).empty());
        json_object_object_add(w, "rows", json_object_new_int(7));
        assert(!validate_wall_shape(w).empty());
        json_object_object_add(w, "rows", json_object_new_int(2));
        json_object_object_add(w, "cols", json_object_new_int(0));
        assert(!validate_wall_shape(w).empty());
        json_object_object_add(w, "cols", json_object_new_int(16));
        assert(!validate_wall_shape(w).empty());
        json_object_object_add(w, "cols", json_object_new_int(2));
        json_object_object_add(w, "mode", json_object_new_string("LCD"));
        assert(validate_wall_shape(w).empty());
        json_object_object_add(w, "type", json_object_new_string(std::string(33, 'x').c_str()));
        assert(!validate_wall_shape(w).empty());
        json_object_object_del(w, "type");
        json_object* rp = json_object_new_string("nope");
        json_object_object_add(w, "rowPixels", rp);
        assert(!validate_wall_shape(w).empty());
        json_object_object_del(w, "rowPixels");
        json_object* short_rp = json_object_new_array();
        json_object_array_add(short_rp, json_object_new_int(1080));
        json_object_object_add(w, "rowPixels", short_rp);
        assert(!validate_wall_shape(w).empty());
        json_object_object_del(w, "rowPixels");
        json_object* bad_rp = json_object_new_array();
        json_object_array_add(bad_rp, json_object_new_int(1080));
        json_object_array_add(bad_rp, json_object_new_int(32));
        json_object_object_add(w, "rowPixels", bad_rp);
        assert(!validate_wall_shape(w).empty());
        json_object_object_del(w, "rowPixels");
        json_object* good_rp = json_object_new_array();
        json_object_array_add(good_rp, json_object_new_int(1080));
        json_object_array_add(good_rp, json_object_new_int(1080));
        json_object_object_add(w, "rowPixels", good_rp);
        assert(validate_wall_shape(w).empty());
        json_object* bad_cp = json_object_new_string("nope");
        json_object_object_add(w, "colPixels", bad_cp);
        assert(!validate_wall_shape(w).empty());
        json_object_object_del(w, "colPixels");
        json_object* cg = json_object_new_int(3);
        json_object_object_add(w, "cell_grid", cg);
        assert(!validate_wall_shape(w).empty());
        json_object_object_del(w, "cell_grid");
        json_object_object_add(w, "cell_grid", json_object_new_array());
        assert(validate_wall_shape(w).empty());
        json_object_put(w);
    }
    // Scenes: aliases, caps, per-window branches.
    assert(!validate_scene_shape(nullptr, 0, 0).empty());
    {
        json_object* s = json_object_new_object();
        json_object_object_add(s, "snapshotVersion", json_object_new_int(1));
        assert(validate_scene_shape(s, 0, 0).empty());
        json_object_object_add(s, "volume", json_object_new_string("80"));
        assert(!validate_scene_shape(s, 0, 0).empty());
        json_object_object_add(s, "volume", json_object_new_int(-1));
        assert(!validate_scene_shape(s, 0, 0).empty());
        json_object_object_add(s, "volume", json_object_new_int(80));
        json_object_object_add(s, "layoutMode", json_object_new_string("fixed"));
        assert(validate_scene_shape(s, 0, 0).empty());
        json_object_object_add(s, "layoutMode", json_object_new_string("tile"));
        assert(!validate_scene_shape(s, 0, 0).empty());
        json_object_object_del(s, "layoutMode");
        json_object_object_add(s, "layout", json_object_new_string("ignored"));
        assert(validate_scene_shape(s, 0, 0).empty());
        json_object_object_add(s, "windows", json_object_new_string("nope"));
        assert(!validate_scene_shape(s, 0, 0).empty());
        json_object_object_del(s, "windows");
        json_object* many = json_object_new_array();
        for (int i = 0; i < 5; i++) {
            json_object* e = json_object_new_object();
            json_object_object_add(e, "id", json_object_new_string("w"));
            json_object_object_add(e, "sig", json_object_new_string("s"));
            json_object_object_add(e, "w", json_object_new_int(100));
            json_object_object_add(e, "h", json_object_new_int(100));
            json_object_array_add(many, e);
        }
        json_object_object_add(s, "windows", many);
        // Wall-global window count is unbounded (per-node budget is enforced
        // at prepare, where member geometries are known): five overlapping
        // windows are a valid saved scene even though one node could not
        // display them all.
        assert(validate_scene_shape(s, 0, 0).empty());
        json_object_put(s);
    }
    {
        json_object* s = json_object_new_object();
        json_object* wins = json_object_new_array();
        json_object_array_add(wins, json_object_new_int(1));
        json_object_object_add(s, "windows", wins);
        assert(!validate_scene_shape(s, 0, 0).empty());
        json_object_put(s);
    }
    {
        json_object* s = json_object_new_object();
        json_object* wins = json_object_new_array();
        json_object* w0 = json_object_new_object();
        json_object_object_add(w0, "id", json_object_new_string("w1"));
        json_object_object_add(w0, "sig", json_object_new_string("s1"));
        json_object_object_add(w0, "x", json_object_new_int(1900));
        json_object_object_add(w0, "w", json_object_new_int(100));
        json_object_object_add(w0, "h", json_object_new_int(100));
        json_object_array_add(wins, w0);
        json_object_object_add(s, "windows", wins);
        assert(validate_scene_shape(s, 0, 0).empty());
        assert(!validate_scene_shape(s, 1920, 1080).empty());
        json_object_put(s);
    }
    {
        json_object* s = json_object_new_object();
        json_object* wins = json_object_new_array();
        json_object* w0 = json_object_new_object();
        json_object_object_add(w0, "sig", json_object_new_string("s1"));
        json_object_object_add(w0, "w", json_object_new_int(100));
        json_object_object_add(w0, "h", json_object_new_int(100));
        json_object_array_add(wins, w0);
        json_object_object_add(s, "windows", wins);
        assert(!validate_scene_shape(s, 0, 0).empty());
        json_object_put(s);
    }
    {
        json_object* s = json_object_new_object();
        json_object* wins = json_object_new_array();
        json_object* w0 = json_object_new_object();
        json_object_object_add(w0, "id", json_object_new_string("w1"));
        json_object_object_add(w0, "w", json_object_new_int(100));
        json_object_object_add(w0, "h", json_object_new_int(100));
        json_object_array_add(wins, w0);
        json_object_object_add(s, "windows", wins);
        assert(!validate_scene_shape(s, 0, 0).empty());
        json_object_put(s);
    }
    {
        json_object* s = json_object_new_object();
        json_object* wins = json_object_new_array();
        json_object* w0 = json_object_new_object();
        json_object_object_add(w0, "id", json_object_new_string("w1"));
        json_object_object_add(w0, "sig", json_object_new_string("s1"));
        json_object_object_add(w0, "w", json_object_new_int(10));
        json_object_object_add(w0, "h", json_object_new_int(10));
        json_object_array_add(wins, w0);
        json_object_object_add(s, "windows", wins);
        assert(!validate_scene_shape(s, 0, 0).empty());
        json_object_put(s);
    }
    // Bindings: canonical keys only, outputPort bounds.
    assert(!validate_binding_shape(nullptr).empty());
    {
        json_object* b = json_object_new_object();
        assert(!validate_binding_shape(b).empty());
        json_object_object_add(b, "wallId", json_object_new_string("w1"));
        json_object_object_add(b, "nodeId", json_object_new_string("n1"));
        assert(validate_binding_shape(b).empty());
        json_object_put(b);
        json_object* legacy = json_object_new_object();
        json_object_object_add(legacy, "wall", json_object_new_string("w1"));
        json_object_object_add(legacy, "node", json_object_new_string("n1"));
        assert(!validate_binding_shape(legacy).empty());
        json_object_put(legacy);
    }
    {
        json_object* b = json_object_new_object();
        json_object_object_add(b, "wallId", json_object_new_string("w1"));
        json_object_object_add(b, "nodeId", json_object_new_string("n1"));
        json_object_object_add(b, "outputPort", json_object_new_boolean(1));
        assert(!validate_binding_shape(b).empty());
        json_object_put(b);
    }
    {
        json_object* b = json_object_new_object();
        json_object_object_add(b, "wallId", json_object_new_string("w1"));
        json_object_object_add(b, "nodeId", json_object_new_string("n1"));
        json_object_object_add(b, "outputPort", json_object_new_array());
        assert(!validate_binding_shape(b).empty());
        json_object_put(b);
    }
    {
        json_object* b = json_object_new_object();
        json_object_object_add(b, "wallId", json_object_new_string("w1"));
        json_object_object_add(b, "nodeId", json_object_new_string("n1"));
        json_object_object_add(b, "outputPort", json_object_new_int(0));
        assert(!validate_binding_shape(b).empty());
        json_object_put(b);
    }
    {
        json_object* b = json_object_new_object();
        json_object_object_add(b, "wallId", json_object_new_string("w1"));
        json_object_object_add(b, "nodeId", json_object_new_string("n1"));
        json_object_object_add(b, "outputPort", json_object_new_string(""));
        assert(!validate_binding_shape(b).empty());
        json_object_put(b);
    }
    {
        json_object* b = json_object_new_object();
        json_object_object_add(b, "wallId", json_object_new_string("w1"));
        json_object_object_add(b, "nodeId", json_object_new_string("n1"));
        json_object_object_add(b, "outputPort", json_object_new_int(0));
        assert(!validate_binding_shape(b).empty());
        json_object_put(b);
    }
    {
        json_object* b = json_object_new_object();
        json_object_object_add(b, "wallId", json_object_new_string("w1"));
        json_object_object_add(b, "nodeId", json_object_new_string("n1"));
        json_object_object_add(b, "outputPort", json_object_new_string("HDMI-1"));
        assert(validate_binding_shape(b).empty());
        json_object_object_add(b, "sig", json_object_new_string(std::string(129, 's').c_str()));
        assert(!validate_binding_shape(b).empty());
        json_object_put(b);
    }
    {
        json_object* b = json_object_new_object();
        json_object_object_add(b, "wallId", json_object_new_string("w1"));
        json_object_object_add(b, "nodeId", json_object_new_string("n1"));
        json_object_object_add(b, "outputPort", json_object_new_int(3));
        assert(validate_binding_shape(b).empty());
        assert(binding_output_of(b) == "3");
        json_object_put(b);
    }
    assert(binding_output_of(nullptr).empty());
    assert(binding_sig_of(nullptr).empty());
    {
        json_object* b = json_object_new_object();
        json_object_object_add(b, "outputPort", json_object_new_int(5));
        assert(binding_output_of(b) == "5");
        json_object_object_add(b, "outputPort", json_object_new_int(0));
        assert(binding_output_of(b).empty());
        json_object_put(b);
    }
    {
        json_object* b = json_object_new_object();
        json_object_object_add(b, "outputPort", json_object_new_string("HDMI-2"));
        assert(binding_output_of(b) == "HDMI-2");
        json_object_put(b);
    }
    {
        json_object* b = json_object_new_object();
        json_object_object_add(b, "outputPort", json_object_new_int(4));
        assert(binding_output_of(b) == "4");
        json_object_put(b);
    }
    {
        json_object* b = json_object_new_object();
        json_object_object_add(b, "outputPort", json_object_new_int(6));
        assert(binding_output_of(b) == "6");
        json_object_put(b);
    }
    // Whitelist canonical keys and ceilings.
    assert(!validate_whitelist_shape(nullptr).empty());
    {
        json_object* w = json_object_new_object();
        json_object_object_add(w, "sig", json_object_new_string("s1"));
        json_object_object_add(w, "address", json_object_new_string("239.0.0.1"));
        json_object_object_add(w, "port", json_object_new_int(5000));
        assert(validate_whitelist_shape(w).empty());
        json_object_put(w);
    }
    {
        json_object* w = json_object_new_object();
        json_object_object_add(w, "id", json_object_new_string("s1"));
        json_object_object_add(w, "mcast_ip", json_object_new_string("239.0.0.1"));
        json_object_object_add(w, "mcast_port", json_object_new_int(5000));
        assert(!validate_whitelist_shape(w).empty());
        json_object_put(w);
    }
    {
        json_object* w = json_object_new_object();
        json_object_object_add(w, "sig", json_object_new_string("s1"));
        json_object_object_add(w, "address", json_object_new_string("239.0.0.1"));
        json_object_object_add(w, "port", json_object_new_int(0));
        assert(validate_whitelist_shape(w).empty());
        json_object_put(w);
    }
    {
        json_object* w = json_object_new_object();
        json_object_object_add(w, "sig", json_object_new_string("s1"));
        json_object_object_add(w, "address", json_object_new_string("239.0.0.1"));
        json_object_object_add(w, "codec", json_object_new_string("HEVC"));
        assert(validate_whitelist_shape(w).empty());
        json_object_object_add(w, "codec", json_object_new_string("vp9"));
        assert(!validate_whitelist_shape(w).empty());
        json_object_put(w);
    }
    {
        json_object* w = json_object_new_object();
        json_object_object_add(w, "sig", json_object_new_string("s1"));
        json_object_object_add(w, "address", json_object_new_string("239.0.0.1"));
        json_object_object_add(w, "width", json_object_new_int(0));
        assert(!validate_whitelist_shape(w).empty());
        json_object_object_add(w, "width", json_object_new_string("x"));
        assert(!validate_whitelist_shape(w).empty());
        json_object_put(w);
    }
    {
        json_object* w = json_object_new_object();
        json_object_object_add(w, "sig", json_object_new_string("s1"));
        json_object_object_add(w, "address", json_object_new_string("239.0.0.1"));
        json_object_object_add(w, "fps", json_object_new_double(29.97));
        assert(validate_whitelist_shape(w).empty());
        json_object_object_add(w, "fps", json_object_new_string("60"));
        assert(!validate_whitelist_shape(w).empty());
        json_object_object_add(w, "fps", json_object_new_int(0));
        assert(!validate_whitelist_shape(w).empty());
        json_object_object_add(w, "fps", json_object_new_int(121));
        assert(!validate_whitelist_shape(w).empty());
        json_object_put(w);
    }
    {
        json_object* w = json_object_new_object();
        json_object_object_add(w, "sig", json_object_new_string("s1"));
        json_object_object_add(w, "address", json_object_new_string("239.0.0.1"));
        json_object_object_add(w, "bitrate", json_object_new_int(4000));
        assert(validate_whitelist_shape(w).empty());
        json_object_object_add(w, "bitrate", json_object_new_int(0));
        assert(!validate_whitelist_shape(w).empty());
        json_object_put(w);
    }
    {
        json_object* w = json_object_new_object();
        json_object_object_add(w, "sig", json_object_new_string("s1"));
        json_object_object_add(w, "address", json_object_new_string("239.0.0.1"));
        json_object_object_add(w, "gop", json_object_new_int(30));
        assert(validate_whitelist_shape(w).empty());
        json_object_object_add(w, "gop", json_object_new_int(601));
        assert(!validate_whitelist_shape(w).empty());
        json_object_put(w);
    }
    // Playback aliases stay at the route layer: the shape validator takes
    // canonical keys only, plus the full status set.
    assert(!validate_playback_shape(nullptr).empty());
    {
        json_object* t = json_object_new_object();
        json_object_object_add(t, "wall", json_object_new_string("w"));
        json_object_object_add(t, "scene", json_object_new_string("s"));
        json_object_object_add(t, "node", json_object_new_string("n"));
        assert(!validate_playback_shape(t).empty());
        json_object_put(t);
        t = json_object_new_object();
        json_object_object_add(t, "wallId", json_object_new_string("w"));
        json_object_object_add(t, "sceneId", json_object_new_string("s"));
        json_object_object_add(t, "targetNodeId", json_object_new_string("n"));
        static const char* kStates[] = {"pending_check", "pending_execution",
                                        "executing", "playing", "succeeded",
                                        "failed", "cancelled", "expired"};
        for (auto st : kStates) {
            json_object_object_add(t, "status", json_object_new_string(st));
            assert(validate_playback_shape(t).empty());
        }
        json_object_put(t);
    }
    // Hash and dirty-compare branches.
    {
        json_object* e1 = json_object_new_array();
        json_object* e2 = json_object_new_array();
        assert(layout_windows_hash(e1) == layout_windows_hash(e2));
        assert(layout_windows_hash(nullptr) == layout_windows_hash(nullptr));
        json_object_put(e1);
        json_object_put(e2);
    }
    {
        json_object* o = json_object_new_object();
        json_object_object_add(o, "a", json_object_new_int(1));
        assert(layout_windows_hash(o) == layout_windows_hash(o));
        json_object_put(o);
    }
    {
        json_object* a = json_object_new_array();
        json_object* d = layout_dirty_compare(a, 80, a, 50);
        json_object* dv = nullptr;
        assert(json_object_object_get_ex(d, "dirty", &dv) && json_object_get_boolean(dv));
        json_object* diff = nullptr;
        assert(json_object_object_get_ex(d, "diff", &diff));
        assert(json_object_array_length(diff) == 1);
        json_object_put(d);
        json_object_put(a);
    }
    {
        json_object* d = layout_dirty_compare(nullptr, 80, nullptr, 80);
        json_object* dv = nullptr;
        assert(json_object_object_get_ex(d, "dirty", &dv) && !json_object_get_boolean(dv));
        json_object_put(d);
    }
    // wall_is_deleted variants.
    assert(!wall_is_deleted(nullptr));
    {
        json_object* w = json_object_new_object();
        json_object_object_add(w, "deletedAt", json_object_new_string("2024-01-01T00:00:00Z"));
        assert(wall_is_deleted(w));
        json_object_put(w);
    }
    {
        json_object* w = json_object_new_object();
        json_object_object_add(w, "deletedAt", json_object_new_int(123));
        assert(wall_is_deleted(w));
        json_object_put(w);
    }
    {
        json_object* w = json_object_new_object();
        json_object_object_add(w, "deletedAt", json_object_new_int(0));
        assert(!wall_is_deleted(w));
        json_object_put(w);
    }
    {
        json_object* w = json_object_new_object();
        json_object_object_add(w, "deletedAt", json_object_new_string(""));
        assert(!wall_is_deleted(w));
        json_object_put(w);
    }
    // base_subrect degenerate inputs.
    assert(!base_subrect(0, 1080, Rect{0, 0, 100, 100}, nullptr));
    assert(!base_subrect(1920, 1080, Rect{0, 0, 0, 100}, nullptr));
    {
        Rect sub{};
        assert(base_subrect(1920, 1080, Rect{-100, -50, 300, 200}, &sub));
        assert(sub.x == 0 && sub.y == 0 && sub.w == 200 && sub.h == 150);
    }
    assert(base_subrect(1920, 1080, Rect{0, 0, 1920, 1080}, nullptr));
    // Env-driven knobs with save/restore.
    {
        const char* old = getenv("DVS_PTP_MAX_OFFSET_US");
        std::string keep = old ? old : "";
        bool had = old != nullptr;
        setenv("DVS_PTP_MAX_OFFSET_US", "500", 1);
        assert(ptp_max_offset_us() == 500);
        setenv("DVS_PTP_MAX_OFFSET_US", "abc", 1);
        assert(ptp_max_offset_us() == 2000);
        setenv("DVS_PTP_MAX_OFFSET_US", "", 1);
        assert(ptp_max_offset_us() == 2000);
        restore_env("DVS_PTP_MAX_OFFSET_US", keep, had);
    }
    {
        const char* old = getenv("DVS_FANOUT_PARALLEL");
        std::string keep = old ? old : "";
        bool had = old != nullptr;
        setenv("DVS_FANOUT_PARALLEL", "0", 1);
        assert(fanout_parallelism() == 1);
        setenv("DVS_FANOUT_PARALLEL", "999", 1);
        assert(fanout_parallelism() == 16);
        setenv("DVS_FANOUT_PARALLEL", "", 1);
        assert(fanout_parallelism() == 8);
        restore_env("DVS_FANOUT_PARALLEL", keep, had);
    }
    {
        const char* old = getenv("DVS_MAX_WALL_MEMBERS");
        std::string keep = old ? old : "";
        bool had = old != nullptr;
        setenv("DVS_MAX_WALL_MEMBERS", "64", 1);
        assert(max_wall_members() == 64);
        setenv("DVS_MAX_WALL_MEMBERS", "0", 1);
        assert(max_wall_members() == 128);
        setenv("DVS_MAX_WALL_MEMBERS", "", 1);
        assert(max_wall_members() == 128);
        restore_env("DVS_MAX_WALL_MEMBERS", keep, had);
    }
    // Clock gate branches.
    {
        std::string reason;
        assert(!node_clock_ok(nullptr, nullptr));
        json_object* n = json_object_new_object();
        assert(!node_clock_ok(n, &reason));
        json_object_object_add(n, "online", json_object_new_boolean(1));
        json_object_object_add(n, "role", json_object_new_string("input"));
        assert(!node_clock_ok(n, &reason));
        json_object_object_add(n, "role", json_object_new_string("output"));
        json_object* clk = json_object_new_object();
        json_object_object_add(clk, "state", json_object_new_string("synced"));
        json_object_object_add(n, "ptp", clk);
        assert(node_clock_ok(n, &reason));
        json_object_put(n);
    }
    {
        std::string reason;
        json_object* n = json_object_new_object();
        json_object_object_add(n, "online", json_object_new_boolean(1));
        json_object* p = json_object_new_object();
        json_object_object_add(p, "state", json_object_new_string("synced"));
        json_object_object_add(n, "ptp", p);
        assert(node_clock_ok(n, &reason));
        json_object_put(n);
    }
    {
        const char* old = getenv("DVS_PTP_MAX_OFFSET_US");
        std::string keep = old ? old : "";
        bool had = old != nullptr;
        unsetenv("DVS_PTP_MAX_OFFSET_US");
        std::string reason;
        json_object* n = json_object_new_object();
        json_object_object_add(n, "online", json_object_new_boolean(1));
        json_object* ptp = json_object_new_object();
        json_object_object_add(ptp, "state", json_object_new_string("synced"));
        json_object_object_add(ptp, "offsetUs", json_object_new_int(5000));
        json_object_object_add(n, "ptp", ptp);
        assert(!node_clock_ok(n, &reason));
        json_object* ptp2 = json_object_new_object();
        json_object_object_add(ptp2, "state", json_object_new_string("holdover"));
        json_object_object_add(n, "ptp", ptp2);
        assert(!node_clock_ok(n, &reason));
        json_object* bad = json_object_new_string("x");
        json_object_object_add(n, "ptp", bad);
        assert(!node_clock_ok(n, &reason));
        json_object_put(n);
        restore_env("DVS_PTP_MAX_OFFSET_US", keep, had);
    }
    // Barrier helpers: remaining input shapes.
    {
        json_object* o = json_object_new_object();
        assert(barrier_lanes_ready(o));
        json_object_put(o);
    }
    {
        json_object* lanes = json_object_new_array();
        json_object* e = json_object_new_object();
        json_object_array_add(lanes, e);
        assert(barrier_lanes_ready(lanes));
        json_object_array_add(lanes, json_object_new_int(1));
        assert(barrier_lanes_ready(lanes));
        json_object_put(lanes);
    }
    {
        int64_t seq = 0;
        assert(barrier_lanes_frame_consistent(nullptr, &seq) && seq == 0);
        json_object* o = json_object_new_object();
        assert(barrier_lanes_frame_consistent(o, nullptr));
        json_object_put(o);
    }
    {
        json_object* lanes = json_object_new_array();
        json_object* a = json_object_new_object();
        json_object_object_add(a, "frame_seq", json_object_new_int(4));
        json_object_array_add(lanes, a);
        json_object* b = json_object_new_object();
        json_object_object_add(b, "frame", json_object_new_int(4));
        json_object_array_add(lanes, b);
        int64_t seq = 0;
        assert(barrier_lanes_frame_consistent(lanes, &seq) && seq == 4);
        json_object* c = json_object_new_object();
        json_object_array_add(lanes, c);
        assert(barrier_lanes_frame_consistent(lanes, nullptr));
        json_object_array_add(lanes, json_object_new_int(9));
        assert(barrier_lanes_frame_consistent(lanes, nullptr));
        json_object_put(lanes);
    }
    {
        json_object* lanes = json_object_new_array();
        json_object* a = json_object_new_object();
        json_object_object_add(a, "frameSeq", json_object_new_int(11));
        json_object_array_add(lanes, a);
        int64_t seq = 0;
        assert(barrier_lanes_frame_consistent(lanes, &seq) && seq == 11);
        json_object_put(lanes);
    }
}

static void test_p2_business_rules() {
    // Binding output dimension + signal reference (P2-1).
    {
        json_object* b = json_object_new_object();
        json_object_object_add(b, "wallId", json_object_new_string("w1"));
        json_object_object_add(b, "nodeId", json_object_new_string("n1"));
        assert(validate_binding_shape(b).empty());
        assert(binding_output_of(b).empty());
        assert(binding_sig_of(b).empty());
        json_object_object_add(b, "outputPort", json_object_new_string("HDMI-1"));
        assert(validate_binding_shape(b).empty());
        assert(binding_output_of(b) == "HDMI-1");
        json_object_object_add(b, "outputPort", json_object_new_string("other"));
        assert(validate_binding_shape(b).empty());
        json_object_object_add(b, "outputPort", json_object_new_int(3));
        assert(validate_binding_shape(b).empty());
        assert(binding_output_of(b) == "3");
        json_object_object_add(b, "output", json_object_new_string("other"));
        assert(validate_binding_shape(b).empty());
        assert(binding_output_of(b) == "3");
        json_object_object_del(b, "output");
        json_object_object_add(b, "outputPort", json_object_new_int(70000));
        assert(!validate_binding_shape(b).empty());
        json_object_object_add(b, "outputPort", json_object_new_int(3));
        json_object_object_add(b, "sig", json_object_new_string("s1"));
        assert(validate_binding_shape(b).empty());
        assert(binding_sig_of(b) == "s1");
        json_object_put(b);
        json_object* bad = json_object_new_object();
        json_object_object_add(bad, "wallId", json_object_new_string("w1"));
        json_object_object_add(bad, "nodeId", json_object_new_string("n1"));
        json_object_object_add(bad, "outputPort", json_object_new_int(-2));
        assert(!validate_binding_shape(bad).empty());
        json_object_put(bad);
    }
    // Whitelist encoding ceiling (P2-3).
    {
        json_object* w = json_object_new_object();
        json_object_object_add(w, "sig", json_object_new_string("s1"));
        json_object_object_add(w, "address", json_object_new_string("239.0.0.1"));
        assert(validate_whitelist_shape(w).empty());
        json_object_object_add(w, "fps", json_object_new_int(60));
        json_object_object_add(w, "bitrateKbps", json_object_new_int(8000));
        json_object_object_add(w, "keyFrameInterval", json_object_new_int(60));
        assert(validate_whitelist_shape(w).empty());
        json_object_object_add(w, "fps", json_object_new_int(240));
        assert(!validate_whitelist_shape(w).empty());
        json_object_object_add(w, "fps", json_object_new_int(60));
        json_object_object_add(w, "bitrateKbps", json_object_new_int(0));
        assert(!validate_whitelist_shape(w).empty());
        json_object_object_add(w, "bitrateKbps", json_object_new_int(8000));
        json_object_object_add(w, "gop", json_object_new_int(0));
        assert(!validate_whitelist_shape(w).empty());
        json_object_put(w);
    }
    // Multicast pool allocation (P2-2).
    {
        std::set<std::pair<std::string, int>> used;
        std::string addr;
        int port = 0;
        assert(multicast_pool_next(used, 0, &addr, &port));
        assert(addr == "239.100.0.1" && port == 50010);
        used.insert({addr, port});
        assert(multicast_pool_next(used, 0, &addr, &port));
        assert(addr == "239.100.0.2" && port == 50010);
        assert(multicast_pool_next(used, 6000, &addr, &port));
        assert(addr == "239.100.0.1" && port == 6000);
        assert(!multicast_pool_next(used, 99999, &addr, &port));
        assert(multicast_pool_next(used, 0, nullptr, nullptr));
        std::set<std::pair<std::string, int>> full;
        for (int i = 1; i <= 254; i++) {
            char a[32];
            snprintf(a, sizeof(a), "239.100.0.%d", i);
            full.insert({a, 50010});
        }
        assert(!multicast_pool_next(full, 0, &addr, &port));
    }
}

static void test_wall_ops() {
    OpCoordinator c;
    c.begin_with_deadline("op-w1", 3, {"a"}, 100, 200);
    if (Operation* m = c.mutable_find("op-w1")) {
        m->wall_id = "wall-1";
        m->scene_id = "scene-1";
        m->layout_hash = "abc";
    }
    assert(c.wall_has_preparing("wall-1", ""));
    assert(!c.wall_has_preparing("wall-1", "op-w1"));
    assert(!c.wall_has_preparing("wall-2", ""));
    assert(c.on_reply("op-w1", "a", true, "ready") == "committed");
    assert(!c.wall_has_preparing("wall-1", ""));
    assert(c.wall_committed_revision("wall-1") == 3);
    assert(c.wall_committed_revision("wall-2") == 0);
    json_object* rec = operation_to_json(*c.find("op-w1"));
    Operation back;
    assert(operation_from_json(rec, &back));
    assert(back.wall_id == "wall-1" && back.scene_id == "scene-1");
    assert(back.layout_hash == "abc");
    json_object_put(rec);
}

static void test_ops_gaps() {
    // find_committed skips unsettled and snapshot-less ops.
    {
        OpCoordinator c;
        assert(c.find_committed(9) == nullptr);
        c.begin("op-ns", 9, {"a"}, 0);
        assert(c.find_committed(9) == nullptr);
        c.on_reply("op-ns", "a", true, "ready");
        assert(c.find_committed(9) == nullptr);
        if (Operation* m = c.mutable_find("op-ns")) {
            m->windows_json = "[]";
            m->wall_w = 0;
            m->wall_h = 0;
        }
        assert(c.find_committed(9) == nullptr);
    }
    // gc keeps preparing ops and ageless terminal ops.
    {
        OpCoordinator c;
        c.begin("op-prep", 1, {"a"}, 0);
        c.begin_with_deadline("op-old", 2, {"a"}, 0, 0);
        c.on_reply("op-old", "a", true, "ready");
        if (Operation* m = c.mutable_find("op-old")) m->created_ns = 100;
        assert(c.gc(100000000000ULL, 1000ULL).size() == 1);
        assert(c.find("op-old") == nullptr);
        assert(c.find("op-prep") != nullptr);
        assert(c.find("op-prep")->state == "preparing");
    }
    // set_deadline on a missing op is a no-op; on a live op it sticks.
    {
        OpCoordinator c;
        c.set_deadline("missing", 5);
        c.begin("op-dl", 1, {"a"}, 0);
        c.set_deadline("op-dl", 77);
        assert(c.find("op-dl")->deadline_ns == 77);
        assert(c.mutable_find("missing") == nullptr);
    }
    // Empty wall id never reports preparing; unknown wall has no revision.
    {
        OpCoordinator c;
        c.begin("op-e", 1, {"a"}, 0);
        assert(!c.wall_has_preparing("", ""));
        assert(c.wall_committed_revision("nope") == 0);
    }
}

static json_object* mk_win(const char* id, const char* sig, int x, int y,
                          int w, int h) {
    json_object* o = json_object_new_object();
    json_object_object_add(o, "id", json_object_new_string(id));
    json_object_object_add(o, "sig", json_object_new_string(sig));
    json_object_object_add(o, "x", json_object_new_int(x));
    json_object_object_add(o, "y", json_object_new_int(y));
    json_object_object_add(o, "w", json_object_new_int(w));
    json_object_object_add(o, "h", json_object_new_int(h));
    return o;
}
static int jget(json_object* o, const char* k) {
    json_object* v = nullptr;
    if (o && json_object_object_get_ex(o, k, &v) && v)
        return json_object_get_int(v);
    return -9999;
}
static std::string jgets(json_object* o, const char* k) {
    json_object* v = nullptr;
    if (o && json_object_object_get_ex(o, k, &v) && v &&
        json_object_is_type(v, json_type_string))
        return json_object_get_string(v);
    return "";
}
static void restore_env(const char* k, const std::string& keep, bool had) {
    if (had)
        setenv(k, keep.c_str(), 1);
    else
        unsetenv(k);
}

static void test_fragment_for_node() {
    // Null and non-array inputs yield an empty fragment.
    assert(json_object_array_length(fragment_for_node(nullptr, Rect{0, 0, 1920, 1080})) == 0);
    {
        json_object* notarr = json_object_new_object();
        json_object* f = fragment_for_node(notarr, Rect{0, 0, 1920, 1080});
        assert(json_object_array_length(f) == 0);
        json_object_put(f);
        json_object_put(notarr);
    }
    // Single full window on the matching node: no crop, not partial.
    {
        json_object* wins = json_object_new_array();
        json_object* w = mk_win("w1", "s1", 0, 0, 1920, 1080);
        json_object_object_add(w, "z", json_object_new_int(3));
        json_object_array_add(wins, w);
        json_object* f = fragment_for_node(wins, Rect{0, 0, 1920, 1080});
        assert(json_object_array_length(f) == 1);
        json_object* e = json_object_array_get_idx(f, 0);
        assert(jgets(e, "windowId") == "w1" && jgets(e, "sig") == "s1");
        json_object* dst = nullptr;
        assert(json_object_object_get_ex(e, "dst", &dst) && dst);
        assert(jget(dst, "x") == 0 && jget(dst, "y") == 0);
        assert(jget(dst, "w") == 1920 && jget(dst, "h") == 1080);
        json_object* src = nullptr;
        assert(json_object_object_get_ex(e, "src", &src) && src);
        assert(jget(src, "x") == 0 && jget(src, "y") == 0);
        assert(jget(src, "w") == 1920 && jget(src, "h") == 1080);
        assert(jget(e, "fullW") == 1920 && jget(e, "fullH") == 1080);
        json_object* pv = nullptr;
        assert(json_object_object_get_ex(e, "partial", &pv) && !json_object_get_boolean(pv));
        assert(jget(e, "z") == 3);
        json_object* mv = nullptr;
        assert(!json_object_object_get_ex(e, "muted", &mv));
        json_object_put(f);
        json_object_put(wins);
    }
    // Cross-node window splits into per-node crops with full-frame dims.
    {
        json_object* wins = json_object_new_array();
        json_object* w = mk_win("w2", "s9", 0, 0, 3840, 1080);
        json_object_object_add(w, "muted", json_object_new_boolean(1));
        json_object_array_add(wins, w);
        json_object* left = fragment_for_node(wins, Rect{0, 0, 1920, 1080});
        json_object* right = fragment_for_node(wins, Rect{1920, 0, 1920, 1080});
        assert(json_object_array_length(left) == 1);
        assert(json_object_array_length(right) == 1);
        json_object* l = json_object_array_get_idx(left, 0);
        json_object* r = json_object_array_get_idx(right, 0);
        json_object* pv = nullptr;
        assert(json_object_object_get_ex(l, "partial", &pv) && json_object_get_boolean(pv));
        assert(json_object_object_get_ex(r, "partial", &pv) && json_object_get_boolean(pv));
        assert(jget(l, "fullW") == 3840 && jget(l, "fullH") == 1080);
        json_object* ld = nullptr;
        assert(json_object_object_get_ex(l, "dst", &ld) && ld);
        assert(jget(ld, "x") == 0 && jget(ld, "w") == 1920);
        json_object* ls = nullptr;
        assert(json_object_object_get_ex(l, "src", &ls) && ls);
        assert(jget(ls, "x") == 0 && jget(ls, "w") == 1920);
        json_object* rd = nullptr;
        assert(json_object_object_get_ex(r, "dst", &rd) && rd);
        assert(jget(rd, "x") == 0 && jget(rd, "w") == 1920);
        json_object* rs = nullptr;
        assert(json_object_object_get_ex(r, "src", &rs) && rs);
        assert(jget(rs, "x") == 1920 && jget(rs, "w") == 1920);
        json_object* mv = nullptr;
        assert(json_object_object_get_ex(r, "muted", &mv) && json_object_get_boolean(mv));
        json_object_put(left);
        json_object_put(right);
        json_object_put(wins);
    }
    // No overlap, degenerate rects and non-object elements are skipped.
    {
        json_object* wins = json_object_new_array();
        json_object_array_add(wins, mk_win("far", "s1", 5000, 0, 100, 100));
        json_object_array_add(wins, mk_win("flat", "s1", 0, 0, 0, 100));
        json_object_array_add(wins, json_object_new_int(7));
        json_object* f = fragment_for_node(wins, Rect{0, 0, 1920, 1080});
        assert(json_object_array_length(f) == 0);
        json_object_put(f);
        json_object_put(wins);
    }
}

static void test_per_node_budget() {
    assert(max_windows_per_node() == 4);
    // No members or no windows: nothing to budget, passes.
    {
        std::vector<std::pair<std::string, Rect>> none;
        assert(validate_per_node_budget(nullptr, none).empty());
        json_object* empty = json_object_new_array();
        assert(validate_per_node_budget(empty, none).empty());
        json_object_put(empty);
    }
    // Four overlapping windows on one node pass; the fifth rejects naming it.
    {
        json_object* wins = json_object_new_array();
        for (int i = 0; i < 5; i++) {
            char id[16];
            snprintf(id, sizeof(id), "w%d", i);
            json_object_array_add(wins, mk_win(id, "s", 0, 0, 960, 540));
        }
        std::vector<std::pair<std::string, Rect>> one = {{"n1", Rect{0, 0, 1920, 1080}}};
        json_object* four = json_object_new_array();
        for (int i = 0; i < 4; i++)
            json_object_array_add(four,
                json_object_get(json_object_array_get_idx(wins, i)));
        assert(validate_per_node_budget(four, one).empty());
        std::string err = validate_per_node_budget(wins, one);
        assert(!err.empty() && err.find("n1") != std::string::npos);
        json_object_put(four);
        json_object_put(wins);
    }
    // Sixteen windows spread over sixteen nodes: one plane each, passes.
    {
        json_object* wins = json_object_new_array();
        std::vector<std::pair<std::string, Rect>> members;
        for (int i = 0; i < 16; i++) {
            char id[16], nid[16];
            snprintf(id, sizeof(id), "w%d", i);
            snprintf(nid, sizeof(nid), "n%d", i);
            int nx = (i % 4) * 1920, ny = (i / 4) * 1080;
            json_object_array_add(wins, mk_win(id, "s", nx, ny, 1920, 1080));
            members.emplace_back(nid, Rect{nx, ny, 1920, 1080});
        }
        assert(validate_per_node_budget(wins, members).empty());
        json_object_put(wins);
    }
}

static void test_p3_barrier_and_scale() {
    assert(barrier_lanes_ready(nullptr));
    {
        json_object* lanes = json_object_new_array();
        assert(barrier_lanes_ready(lanes));
        json_object* e = json_object_new_object();
        json_object_object_add(e, "ready", json_object_new_boolean(0));
        json_object_array_add(lanes, e);
        assert(!barrier_lanes_ready(lanes));
        json_object_put(lanes);
    }
    {
        json_object* lanes = json_object_new_array();
        json_object* a = json_object_new_object();
        json_object_object_add(a, "ready", json_object_new_boolean(1));
        json_object_object_add(a, "frameSeq", json_object_new_int(7));
        json_object_array_add(lanes, a);
        json_object* b = json_object_new_object();
        json_object_object_add(b, "ready", json_object_new_boolean(1));
        json_object_object_add(b, "frameSeq", json_object_new_int(7));
        json_object_array_add(lanes, b);
        int64_t seq = 0;
        assert(barrier_lanes_frame_consistent(lanes, &seq) && seq == 7);
        json_object* c = json_object_new_object();
        json_object_object_add(c, "frameSeq", json_object_new_int(8));
        json_object_array_add(lanes, c);
        assert(!barrier_lanes_frame_consistent(lanes, nullptr));
        json_object_put(lanes);
    }
    {
        json_object* lanes = json_object_new_array();
        assert(barrier_lanes_frame_consistent(lanes, nullptr));
        json_object_put(lanes);
    }
    assert(fanout_parallelism() >= 1 && fanout_parallelism() <= 16);
    assert(max_wall_members() >= 1);
    assert(validate_volume(0).empty());
    assert(validate_volume(80).empty());
    assert(validate_volume(100).empty());
    assert(!validate_volume(-1).empty());
    assert(!validate_volume(101).empty());
    {
        OpCoordinator c;
        c.begin_with_deadline("op-vol", 5, {"a"}, 100, 200);
        if (Operation* m = c.mutable_find("op-vol")) {
            m->volume = 42;
            m->wall_id = "w";
        }
        json_object* rec = operation_to_json(*c.find("op-vol"));
        Operation back;
        assert(operation_from_json(rec, &back));
        assert(back.volume == 42);
        json_object_put(rec);
        json_object* legacy = json_object_new_object();
        json_object_object_add(legacy, "opId", json_object_new_string("op-old"));
        json_object_object_add(legacy, "state", json_object_new_string("preparing"));
        Operation old;
        assert(operation_from_json(legacy, &old));
        assert(old.volume == 80);
        json_object_put(legacy);
    }
}

static void test_upgrade_entry_guards() {
    PlatformStore store(":memory:");
    PlatformContext ctx;
    ctx.store = &store;
    ctx.ops.set_store(&store);
    HttpRequest req;
    HttpResponse res;
    req.method = "POST";
    req.path = "/api/v1/nodes/upgrade";
    req.body = "{}";
    assert(route_upgrade(ctx, req, res));
    json_object* o = json_tokener_parse(res.body.c_str());
    json_object* okv = nullptr;
    assert(o && json_object_object_get_ex(o, "ok", &okv) && !json_object_get_boolean(okv));
    json_object_put(o);
    req.body = "{\"targetNodeIds\":[\"n1\"]}";
    res = HttpResponse();
    assert(route_upgrade(ctx, req, res));
    o = json_tokener_parse(res.body.c_str());
    assert(o && json_object_object_get_ex(o, "ok", &okv) && !json_object_get_boolean(okv));
    json_object_put(o);
    req.body = "{\"targetNodeIds\":[\"n1\"],\"filename\":\"fw.bin\",\"sha256\":\"abc\"}";
    res = HttpResponse();
    assert(route_upgrade(ctx, req, res));
    o = json_tokener_parse(res.body.c_str());
    assert(o && json_object_object_get_ex(o, "ok", &okv) && json_object_get_boolean(okv));
    json_object_put(o);
}

int main() {
    test_layout_cell_grid();
    test_layout_apply_at();
    test_ops();
    test_ops_deadline();
    test_op_serialization();
    test_ops_persistence();
    test_store();
    test_contract();
    test_contract_gaps();
    test_ws_codec();
    test_business_rules();
    test_p2_business_rules();
    test_per_node_budget();
    test_wall_ops();
    test_ops_gaps();
    test_fragment_for_node();
    test_validation_gaps();
    test_p3_barrier_and_scale();
    test_common();
    test_frame_anchor();
    test_upgrade_entry_guards();

    printf("dvs_platform_basic OK\n");
    return 0;
}
