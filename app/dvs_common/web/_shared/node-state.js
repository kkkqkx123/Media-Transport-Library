/* DvsNodeState: node catalog backed by the platform truth.
 *
 * Single source is GET /api/v1/nodes (envelope {data:{items:[...]}}); this
 * module only normalizes field spellings, polls, and notifies subscribers.
 * Pages must not keep their own node tables.
 */
(function (global) {
  "use strict";

  var cache = {};
  var order = [];
  var listeners = [];

  function norm(raw) {
    raw = raw || {};
    var net = raw.network || {};
    var id = String(raw.id || raw.nodeId || "");
    var role = String(raw.role || raw.type || "output");
    return {
      id: id,
      key: id.toLowerCase(),
      name: raw.name || id,
      ip: raw.ip || net.ip || "",
      role: role,
      type: role,
      lifecycle: raw.lifecycle || (raw.retired ? "retired" : "active"),
      online: !!raw.online,
      registerState: raw.registerState || "",
      raw: raw
    };
  }

  function copy(node) { return JSON.parse(JSON.stringify(node)); }

  function notify() {
    var snapshot = list();
    listeners.slice().forEach(function (fn) {
      try { fn(snapshot); } catch (e) { /* keep other subscribers running */ }
    });
  }

  function itemsOf(payload) {
    if (!payload || !payload.ok) return null;
    var d = payload.data;
    if (Array.isArray(d)) return d;
    if (d && Array.isArray(d.items)) return d.items;
    return null;
  }

  function refresh() {
    if (!global.DvsApi || typeof global.DvsApi.request !== "function") {
      return Promise.resolve(false);
    }
    return global.DvsApi.request("GET", "/api/v1/nodes?pageSize=100").then(function (r) {
      var items = itemsOf(r);
      if (!items) return false;
      var next = {};
      var nextOrder = [];
      items.forEach(function (raw) {
        var n = norm(raw);
        if (!n.key) return;
        next[n.key] = n;
        nextOrder.push(n.key);
      });
      cache = next;
      order = nextOrder;
      notify();
      return true;
    }).catch(function () { return false; });
  }

  function list() {
    return order.map(function (k) { return copy(cache[k]); });
  }

  function get(id) {
    var n = cache[String(id == null ? "" : id).toLowerCase()];
    return n ? copy(n) : null;
  }

  function subscribe(fn) {
    if (typeof fn !== "function") return function () {};
    listeners.push(fn);
    return function () { listeners = listeners.filter(function (item) { return item !== fn; }); };
  }

  global.DvsNodeState = {
    list: list,
    get: get,
    refresh: refresh,
    subscribe: subscribe,
    runtimeStates: {
      online: "在线",
      offline: "离线",
      registering: "注册中",
      register_failed: "注册失败",
      network_reconfiguring: "网络切换中",
      rebooting: "重启中",
      factory_resetting: "恢复出厂中",
      upgrading: "升级中",
      upgrade_failed: "升级失败"
    }
  };

  refresh();
  if (global.setInterval) global.setInterval(refresh, 5000);
})(window);
