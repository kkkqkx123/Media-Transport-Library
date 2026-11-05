#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include "layout.h"
#include "ops.h"
#include "platform_store.h"

#include "dvs_contract.h"
#include "dvs_node_bundle.h"
#include "http_client.h"
#include "http_server.h"
#include "sha256.h"

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

static void test_layout_window_fragment() {
    Rect win{1000, 500, 2000, 1000};
    Rect left{0, 0, 1920, 1080};
    Rect dst, src;
    assert(layout_window_fragment(win, left, 1920, 1080, &dst, &src));
    assert(dst.x == 1000 && dst.y == 500 && dst.w == 920 && dst.h == 580);
    assert(src.x == 0 && src.y == 0);
    assert(src.w > 0 && src.h > 0);
    assert(src.x + src.w <= 1920 && src.y + src.h <= 1080);

    Rect right{1920, 0, 1920, 1080};
    assert(layout_window_fragment(win, right, 1920, 1080, &dst, &src));
    assert(dst.x == 0 && dst.w == 1080);
    assert(src.x + src.w <= 1920 && src.y + src.h <= 1080);

    Rect far{4000, 0, 1920, 1080};
    assert(!layout_window_fragment(win, far, 1920, 1080, &dst, &src));

    assert(layout_window_fragment(win, left, 1920, 1080, nullptr, nullptr));

    Rect zsrc{};
    assert(layout_window_fragment(win, left, 0, 1080, nullptr, &zsrc));
    assert(zsrc.w == 0 && zsrc.h == 0);

    Rect a{0, 0, 5, 5};
    Rect b{10, 10, 5, 5};
    assert(!layout_window_fragment(a, b, 10, 10, &dst, &src));
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

int main() {
    test_layout_cell_grid();
    test_layout_window_fragment();
    test_layout_apply_at();
    test_ops();
    test_ops_deadline();
    test_store();
    test_contract();
    test_ws_codec();
    test_common();

    printf("dvs_platform_basic OK\n");
    return 0;
}
