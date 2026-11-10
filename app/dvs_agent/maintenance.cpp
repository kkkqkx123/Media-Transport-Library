#include "maintenance.h"

#include "node_store.h"
#include "media_ipc.h"
#include "sha256.h"

#include <json-c/json.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include "dvs_http_json.h"

namespace {
// 512MB per-file cap shared with the upload route.
const size_t kMaxFwBytes = 512u * 1024u * 1024u;

std::string upgrade_dir() {
  const char* v = std::getenv("DVS_UPGRADE_DIR");
  return (v && *v) ? std::string(v) : "/var/lib/dvs_agent/upgrade";
}

std::string staging_dir() {
  const char* v = std::getenv("DVS_FW_STAGING_DIR");
  return (v && *v) ? std::string(v) : "/var/lib/dvs_agent/staging";
}

void mkdir_p(const std::string& dir) {
  std::string cur;
  for (size_t i = 0; i < dir.size(); i++) {
    cur.push_back(dir[i]);
    if (dir[i] == '/' && cur.size() > 1) mkdir(cur.c_str(), 0755);
  }
  if (!cur.empty()) mkdir(cur.c_str(), 0755);
}

bool valid_basename(const std::string& b) {
  if (b.empty() || b.size() > 128) return false;
  if (b.find("..") != std::string::npos) return false;
  for (char c : b) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
      continue;
    return false;
  }
  return true;
}

bool valid_version(const std::string& v) {
  if (v.empty() || v.size() > 64) return false;
  for (char c : v) {
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-')
      continue;
    return false;
  }
  return true;
}

std::string file_sha256(const std::string& path) {
  struct stat st;
  if (stat(path.c_str(), &st) != 0) return "";
  if (st.st_size <= 0 || (size_t)st.st_size > kMaxFwBytes) return "";
  std::ifstream f(path, std::ios::binary);
  if (!f) return "";
  std::string data((std::istreambuf_iterator<char>(f)),
                   std::istreambuf_iterator<char>());
  if (data.size() > kMaxFwBytes) return "";
  uint8_t out[32];
  dvs::sha256((const uint8_t*)data.data(), data.size(), out);
  return dvs::to_hex(out, 32);
}

bool copy_file(const std::string& src, const std::string& dst) {
  std::ifstream in(src, std::ios::binary);
  if (!in) return false;
  std::ofstream out(dst, std::ios::binary | std::ios::trunc);
  if (!out) return false;
  char buf[65536];
  while (in) {
    in.read(buf, sizeof(buf));
    std::streamsize n = in.gcount();
    if (n > 0) {
      out.write(buf, n);
      if (!out) return false;
    }
  }
  out.close();
  return (bool)out;
}

// Detached reboot without a shell: double-fork + exec systemctl/reboot.
// Never blocks the HTTP worker; failures are reported via the lifecycle doc.
void spawn_reboot_detached() {
  pid_t pid = fork();
  if (pid < 0) return;
  if (pid > 0) {
    waitpid(pid, nullptr, 0);
    return;
  }
  if (fork() != 0) _exit(0);
  execlp("systemctl", "systemctl", "reboot", (char*)nullptr);
  execlp("reboot", "reboot", (char*)nullptr);
  _exit(127);
}
}  // namespace

Maintenance::Maintenance(NodeStore* store, MediaIpcClient* media)
    : store_(store), media_(media) {}

json_object* Maintenance::reboot(bool confirm) {
  json_object* o = json_object_new_object();
  if (!confirm) {
    json_object_object_add(o, "ok", json_object_new_boolean(0));
    json_object_object_add(o, "status", json_object_new_string("needs_confirm"));
    json_object_object_add(o, "gated", json_object_new_boolean(1));
    json_object_object_add(
        o, "reason",
        json_object_new_string("reboot requires {confirm:true} in the request body"));
    return o;
  }
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
    // Sync the engine role so mutual exclusion holds (input stops RX,
    // output stops TX) before the machine goes down.
    json_object* rv = json_object_new_object();
    json_object_object_add(rv, "scope", json_object_new_string("role"));
    json_object* rval = json_object_new_object();
    json_object_object_add(rval, "role", json_object_new_string(role.c_str()));
    json_object_object_add(rv, "value", rval);
    media_->request("configure", rv);
    json_object_put(rv);
    if (role == "input") media_->request("clear", nullptr);
  }
  json_object* patch = json_object_new_object();
  json_object_object_add(patch, "lifecycle", json_object_new_string("rebooting"));
  store_->update_node(patch);
  json_object_put(patch);
  json_object_object_add(o, "ok", json_object_new_boolean(1));
  json_object_object_add(o, "status", json_object_new_string("rebooting"));
  json_object_object_add(o, "gated", json_object_new_boolean(0));
  sync();
  spawn_reboot_detached();
  return o;
}

json_object* Maintenance::factory_reset() {
  json_object* node = store_->node();
  json_object* net = nullptr;
  json_object_object_get_ex(node, "network", &net);
  json_object* patch = json_object_new_object();
  json_object_object_add(patch, "role", json_object_new_string("output"));
  json_object_object_add(patch, "pendingRole", json_object_new_null());
  json_object_object_add(patch, "roleCleanup", json_object_new_null());
  json_object_object_add(patch, "input", json_object_new_object());
  json_object_object_add(patch, "output", json_object_new_null());
  json_object* sip = json_object_new_object();
  json_object_object_add(sip, "enabled", json_object_new_boolean(0));
  json_object_object_add(sip, "state", json_object_new_string("disabled"));
  json_object_object_add(sip, "mode", json_object_new_string("connectivity-probe"));
  json_object_object_add(patch, "sip", sip);
  json_object_object_add(patch, "currentPlayback", json_object_new_null());
  json_object_object_add(patch, "exception", json_object_new_null());
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
                                  const std::string& sha256_expected,
                                  const std::string& version,
                                  bool confirm) {
  json_object* o = json_object_new_object();
  if (filename.empty()) {
    json_object_object_add(o, "ok", json_object_new_boolean(0));
    json_object_object_add(o, "status", json_object_new_string("failed"));
    json_object_object_add(o, "reason", json_object_new_string("missing filename"));
    return o;
  }
  // Accept a bare basename staged by /upgrade/upload (in the staging dir).
  // Absolute paths are used as-is; anything else resolves against staging so
  // the frontend upload->upgrade link works without /tmp prediction.
  std::string path = filename;
  bool has_slash = (path.find('/') != std::string::npos);
  if (!has_slash) {
    std::string base = path;
    size_t bs = base.find_last_of("\\");
    if (bs != std::string::npos) base = base.substr(bs + 1);
    if (!valid_basename(base)) {
      json_object_object_add(o, "ok", json_object_new_boolean(0));
      json_object_object_add(o, "status", json_object_new_string("failed"));
      json_object_object_add(o, "reason", json_object_new_string("bad filename"));
      return o;
    }
    std::string cand = staging_dir() + "/" + base;
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
  bool match = (actual.size() == sha256_expected.size());
  if (match) {
    for (size_t i = 0; i < actual.size(); i++) {
      char a = actual[i], b = sha256_expected[i];
      if (a >= 'a' && a <= 'f') a = (char)(a - 'a' + 'A');
      if (b >= 'a' && b <= 'f') b = (char)(b - 'a' + 'A');
      if (a != b) {
        match = false;
        break;
      }
    }
  }
  if (!match) {
    // A/B: keep the existing version, report failure (retryable).
    json_object_object_add(o, "ok", json_object_new_boolean(0));
    json_object_object_add(o, "status", json_object_new_string("failed"));
    json_object_object_add(o, "reason", json_object_new_string("sha256 mismatch"));
    json_object_object_add(o, "expected", json_object_new_string(sha256_expected.c_str()));
    json_object_object_add(o, "actual", json_object_new_string(actual.c_str()));
    return o;
  }

  // Version comes from the request package metadata, never from the filename.
  // Unknown stays unknown instead of a fabricated token.
  std::string ver = version;
  if (!ver.empty() && !valid_version(ver)) {
    json_object_object_add(o, "ok", json_object_new_boolean(0));
    json_object_object_add(o, "status", json_object_new_string("failed"));
    json_object_object_add(o, "reason", json_object_new_string("bad version"));
    return o;
  }
  if (ver.empty()) ver = "unknown";

  // Content-addressed staging (no filename traversal, no version collision).
  std::string dir = upgrade_dir() + "/" + actual.substr(0, 16);
  std::string staged = dir + "/fw.bin";
  mkdir_p(dir);
  if (!copy_file(path, staged)) {
    json_object_object_add(o, "ok", json_object_new_boolean(0));
    json_object_object_add(o, "status", json_object_new_string("failed"));
    json_object_object_add(o, "reason", json_object_new_string("cannot stage package"));
    return o;
  }

  if (!confirm) {
    fprintf(stderr, "dvs-agent: firmware staged (switch needs confirm): %s version %s\n",
            staged.c_str(), ver.c_str());
    json_object* lp = json_object_new_object();
    json_object* lu = json_object_new_object();
    json_object_object_add(lu, "filename", json_object_new_string(path.c_str()));
    json_object_object_add(lu, "version", json_object_new_string(ver.c_str()));
    json_object_object_add(lu, "sha256", json_object_new_string(actual.c_str()));
    json_object_object_add(lu, "status", json_object_new_string("staged"));
    json_object_object_add(lu, "at", json_object_new_string(dvs::iso_now().c_str()));
    json_object_object_add(lp, "lastUpgrade", lu);
    store_->update_node(lp);
    json_object_put(lp);
    json_object_object_add(o, "ok", json_object_new_boolean(1));
    json_object_object_add(o, "status", json_object_new_string("staged"));
    json_object_object_add(o, "filename", json_object_new_string(path.c_str()));
    json_object_object_add(o, "version", json_object_new_string(ver.c_str()));
    json_object_object_add(o, "sha256", json_object_new_string(actual.c_str()));
    json_object_object_add(o, "staged", json_object_new_string(staged.c_str()));
    json_object_object_add(o, "reason",
                           json_object_new_string("staged, switch requires {confirm:true}"));
    return o;
  }

  json_object* patch = json_object_new_object();
  json_object_object_add(patch, "fw", json_object_new_string(ver.c_str()));
  json_object* lu2 = json_object_new_object();
  json_object_object_add(lu2, "filename", json_object_new_string(path.c_str()));
  json_object_object_add(lu2, "version", json_object_new_string(ver.c_str()));
  json_object_object_add(lu2, "sha256", json_object_new_string(actual.c_str()));
  json_object_object_add(lu2, "status", json_object_new_string("applied"));
  json_object_object_add(lu2, "at", json_object_new_string(dvs::iso_now().c_str()));
  json_object_object_add(patch, "lastUpgrade", lu2);
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
