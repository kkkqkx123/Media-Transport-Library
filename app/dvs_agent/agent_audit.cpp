#include "agent_audit.h"

#include <mutex>
#include <vector>

#include "agent_context.h"
#include "dvs_http_json.h"

namespace {
struct AuditEntry {
  std::string at;
  std::string scope;
  std::string action;
  std::string result;
  std::string detail;
};
std::mutex g_mu;
std::vector<AuditEntry> g_ring;
const size_t kCap = 200;

void append_locked(const AuditEntry& e) {
  g_ring.push_back(e);
  if (g_ring.size() > kCap)
    g_ring.erase(g_ring.begin(), g_ring.begin() + (g_ring.size() - kCap));
}

json_object* entry_json(const AuditEntry& e) {
  json_object* o = json_object_new_object();
  json_object_object_add(o, "at", json_object_new_string(e.at.c_str()));
  json_object_object_add(o, "scope", json_object_new_string(e.scope.c_str()));
  json_object_object_add(o, "action",
                         json_object_new_string(e.action.c_str()));
  json_object_object_add(o, "result",
                         json_object_new_string(e.result.c_str()));
  json_object_object_add(o, "detail",
                         json_object_new_string(e.detail.c_str()));
  return o;
}
}  // namespace

void agent_audit(const std::string& scope, const std::string& action,
                 const std::string& result, const std::string& detail) {
  AuditEntry e{dvs::iso_now(), scope, action, result, detail};
  std::lock_guard<std::mutex> lk(g_mu);
  append_locked(e);
}

json_object* agent_audit_snapshot(size_t limit) {
  std::lock_guard<std::mutex> lk(g_mu);
  json_object* arr = json_object_new_array();
  size_t n = g_ring.size();
  size_t start = (limit == 0 || limit >= n) ? 0 : n - limit;
  for (size_t i = start; i < n; i++)
    json_object_array_add(arr, entry_json(g_ring[i]));
  return arr;
}

json_object* agent_audit_page(int page, int page_size, int* total_out) {
  if (page < 1) page = 1;
  if (page_size < 1) page_size = 1;
  if (page_size > 200) page_size = 200;
  std::lock_guard<std::mutex> lk(g_mu);
  size_t total = g_ring.size();
  if (total_out) *total_out = (int)total;
  size_t end = total > (size_t)((page - 1) * page_size)
                   ? total - (size_t)((page - 1) * page_size)
                   : 0;
  size_t start = end > (size_t)page_size ? end - (size_t)page_size : 0;
  json_object* items = json_object_new_array();
  for (size_t i = start; i < end; i++)
    json_object_array_add(items, entry_json(g_ring[i]));
  return items;
}
