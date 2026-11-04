#include "maintenance.h"

#include "node_store.h"
#include "media_ipc.h"
#include "sha256.h"

#include <json-c/json.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <unistd.h>

static std::string slurp_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    return std::string((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
}

static std::string file_sha256(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::string data((std::istreambuf_iterator<char>(f)),
                     std::istreambuf_iterator<char>());
    uint8_t out[32];
    dvs::sha256((const uint8_t*)data.data(), data.size(), out);
    return dvs::to_hex(out, 32);
}

static std::string derive_version(const std::string& filename) {
    /* Look for a d.d.d token in the filename; else suffix "-upgraded". */
    size_t i = 0;
    while (i < filename.size()) {
        if (isdigit((unsigned char)filename[i])) {
            size_t j = i;
            int dots = 0;
            while (j < filename.size() &&
                   (isdigit((unsigned char)filename[j]) || filename[j] == '.')) {
                if (filename[j] == '.') dots++;
                j++;
            }
            if (dots >= 2) return filename.substr(i, j - i);
            i = j;
        } else {
            i++;
        }
    }
    return "upgraded";
}

Maintenance::Maintenance(NodeStore* store, MediaIpcClient* media)
    : store_(store), media_(media) {}

json_object* Maintenance::reboot() {
    bool switched = store_->apply_pending_role();
    if (switched) store_->mark_applied("role", 0);
    std::string role;
    {
        json_object* node = store_->node();
        json_object* r = nullptr;
        if (node && json_object_object_get_ex(node, "role", &r) && r)
            role = json_object_get_string(r);
    }
    if (media_ && !role.empty()) {
        /* Sync the engine role so mutual exclusion holds (input stops RX,
         * output stops TX) even when the reboot itself is gated. */
        json_object* rv = json_object_new_object();
        json_object_object_add(rv, "scope", json_object_new_string("role"));
        json_object* rval = json_object_new_object();
        json_object_object_add(rval, "role", json_object_new_string(role.c_str()));
        json_object_object_add(rv, "value", rval);
        media_->request("configure", rv);
        json_object_put(rv);
        if (role == "input") media_->request("clear", nullptr);
    }
    bool allowed = (std::getenv("DVS_ALLOW_REBOOT") != nullptr);
    json_object* o = json_object_new_object();
    json_object_object_add(o, "ok", json_object_new_boolean(1));
    if (allowed) {
        json_object* patch = json_object_new_object();
        json_object_object_add(patch, "lifecycle", json_object_new_string("rebooting"));
        store_->update_node(patch);
        json_object_put(patch);
        json_object_object_add(o, "status", json_object_new_string("rebooting"));
        json_object_object_add(o, "gated", json_object_new_boolean(0));
        sync();
        system("reboot");
    } else {
        json_object_object_add(o, "status", json_object_new_string("reboot_deferred"));
        json_object_object_add(o, "gated", json_object_new_boolean(1));
        json_object_object_add(o, "reason", json_object_new_string("reboot gated for safety; role promotion and media clear already applied"));
    }
    return o;
}

json_object* Maintenance::factory_reset() {
    json_object* node = store_->node();
    json_object* net = nullptr;
    json_object_object_get_ex(node, "network", &net);
    json_object* patch = json_object_new_object();
    json_object_object_add(patch, "role", json_object_new_string("output"));
    json_object_object_add(patch, "pendingRole", json_object_new_null());
    json_object_object_add(patch, "input", json_object_new_object());
    json_object_object_add(patch, "output", json_object_new_null());
    json_object* sip = json_object_new_object();
    json_object_object_add(sip, "enabled", json_object_new_boolean(0));
    json_object_object_add(sip, "state", json_object_new_string("disabled"));
    json_object_object_add(patch, "sip", sip);
    if (net) json_object_object_add(patch, "network", json_object_get(net));
    json_object* rs = nullptr;
    if (node && json_object_object_get_ex(node, "registerState", &rs) && rs)
        json_object_object_add(patch, "registerState", json_object_get(rs));
    json_object* rm = nullptr;
    if (node && json_object_object_get_ex(node, "registerMode", &rm) && rm)
        json_object_object_add(patch, "registerMode", json_object_get(rm));
    store_->set_password("admin123");
    json_object_object_add(patch, "password_change_required", json_object_new_boolean(1));
    store_->update_node(patch);
    json_object_put(patch);
    if (media_) media_->request("clear", nullptr);
    for (int i = 0; i < 6; i++) store_->mark_applied(NodeStore::SCOPES[i], 0);

    json_object* o = json_object_new_object();
    json_object_object_add(o, "ok", json_object_new_boolean(1));
    json_object_object_add(o, "status", json_object_new_string("factory_reset"));
    json_object_object_add(o, "preserved", json_object_new_string("network,registration"));
    return o;
}

json_object* Maintenance::upgrade(const std::string& filename,
                                  const std::string& sha256_expected) {
    json_object* o = json_object_new_object();
    if (filename.empty()) {
        json_object_object_add(o, "ok", json_object_new_boolean(0));
        json_object_object_add(o, "status", json_object_new_string("failed"));
        json_object_object_add(o, "reason", json_object_new_string("missing filename"));
        return o;
    }
    /* Accept a bare basename staged by /upgrade/upload (in /tmp).
     * Absolute or tmp-relative paths are used as-is; anything else is first
     * resolved against /tmp so the frontend upload->upgrade link works. */
    std::string path = filename;
    bool has_slash = (path.find('/') != std::string::npos);
    if (!has_slash) {
        std::string base = path;
        size_t bs = base.find_last_of("\\");
        if (bs != std::string::npos) base = base.substr(bs + 1);
        if (base.empty() || base.size() > 128 || base.find("..") != std::string::npos) {
            json_object_object_add(o, "ok", json_object_new_boolean(0));
            json_object_object_add(o, "status", json_object_new_string("failed"));
            json_object_object_add(o, "reason", json_object_new_string("bad filename"));
            return o;
        }
        std::string cand = std::string("/tmp/") + base;
        std::ifstream probe(cand, std::ios::binary);
        if (probe) path = cand;
    }
    std::string actual = file_sha256(path);
    if (actual.empty()) {
        json_object_object_add(o, "ok", json_object_new_boolean(0));
        json_object_object_add(o, "status", json_object_new_string("failed"));
        json_object_object_add(o, "reason", json_object_new_string("cannot read file"));
        return o;
    }
    /* case-insensitive compare */
    bool match = (actual.size() == sha256_expected.size());
    if (match) {
        for (size_t i = 0; i < actual.size(); i++) {
            char a = actual[i], b = sha256_expected[i];
            if (a >= 'a' && a <= 'f') a = (char)(a - 'a' + 'A');
            if (b >= 'a' && b <= 'f') b = (char)(b - 'a' + 'A');
            if (a != b) { match = false; break; }
        }
    }
    if (!match) {
        /* A/B: keep the existing version, report failure (retryable). */
        json_object_object_add(o, "ok", json_object_new_boolean(0));
        json_object_object_add(o, "status", json_object_new_string("failed"));
        json_object_object_add(o, "reason", json_object_new_string("sha256 mismatch"));
        json_object_object_add(o, "expected", json_object_new_string(sha256_expected.c_str()));
        json_object_object_add(o, "actual", json_object_new_string(actual.c_str()));
        return o;
    }

    /* Verified: stage the package (A/B: previous version path is untouched).
     * The active switch stays gated. Without DVS_ALLOW_UPGRADE_SWITCH
     * the package is staged and reported as "staged" with the version kept
     * on the old value; with the env set the fw version is bumped. */
    std::string ver = derive_version(path);
    std::string staged = std::string("/var/lib/dvs_agent/upgrade/") + ver + "/fw.bin";
    std::string content = slurp_file(path);
    /* Best-effort staging write; failures still keep the old version. */
    {
        size_t pos = staged.find_last_of('/');
        if (pos != std::string::npos) {
            std::string dir = staged.substr(0, pos);
            std::string cmd = "mkdir -p " + dir;
            system(cmd.c_str());
        }
        std::ofstream out(staged, std::ios::binary);
        if (out) out.write(content.data(), (std::streamsize)content.size());
    }

    if (!std::getenv("DVS_ALLOW_UPGRADE_SWITCH")) {
        fprintf(stderr, "dvs-agent: firmware staged (switch gated): %s version %s\n",
                staged.c_str(), ver.c_str());
        json_object_object_add(o, "ok", json_object_new_boolean(1));
        json_object_object_add(o, "status", json_object_new_string("staged"));
        json_object_object_add(o, "filename", json_object_new_string(path.c_str()));
        json_object_object_add(o, "version", json_object_new_string(ver.c_str()));
        json_object_object_add(o, "sha256", json_object_new_string(actual.c_str()));
        json_object_object_add(o, "staged", json_object_new_string(staged.c_str()));
        json_object_object_add(o, "reason", json_object_new_string("switch gated (set DVS_ALLOW_UPGRADE_SWITCH=1 to apply)"));
        return o;
    }

    json_object* patch = json_object_new_object();
    json_object_object_add(patch, "fw", json_object_new_string(ver.c_str()));
    store_->update_node(patch);
    json_object_put(patch);

    json_object_object_add(o, "ok", json_object_new_boolean(1));
    json_object_object_add(o, "status", json_object_new_string("applied"));
    json_object_object_add(o, "filename", json_object_new_string(path.c_str()));
    json_object_object_add(o, "version", json_object_new_string(ver.c_str()));
    json_object_object_add(o, "sha256", json_object_new_string(actual.c_str()));
    json_object_object_add(o, "staged", json_object_new_string(staged.c_str()));
    return o;
}
