/* DvsPlaybackTasks: playback board backed by the platform truth.
 *
 * Single source is GET /api/v1/playback-tasks (envelope {data:{items:[...]}});
 * this module only normalizes, polls, and notifies subscribers.
 */
(function (global) {
  "use strict";

  var cache = {};
  var order = [];
  var listeners = [];

  var statuses = {
    pending_check: "待检查",
    pending_execution: "待执行",
    executing: "执行中",
    playing: "播放中",
    succeeded: "已完成",
    failed: "失败",
    cancelled: "已取消",
    expired: "已过期"
  };

  var terminal = {
    succeeded: true,
    failed: true,
    cancelled: true,
    expired: true
  };

  function copy(value) { return JSON.parse(JSON.stringify(value)); }

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
    return global.DvsApi.request("GET", "/api/v1/playback-tasks?pageSize=100").then(function (r) {
      var items = itemsOf(r);
      if (!items) return false;
      var next = {};
      var nextOrder = [];
      items.forEach(function (raw) {
        var id = String((raw && raw.id) || "");
        if (!id) return;
        next[id] = raw;
        nextOrder.push(id);
      });
      cache = next;
      order = nextOrder;
      notify();
      return true;
    }).catch(function () { return false; });
  }

  function list() {
    return order.map(function (id) { return copy(cache[id]); });
  }

  function get(id) {
    var t = cache[String(id == null ? "" : id)];
    return t ? copy(t) : null;
  }

  function subscribe(fn) {
    if (typeof fn !== "function") return function () {};
    listeners.push(fn);
    return function () { listeners = listeners.filter(function (item) { return item !== fn; }); };
  }

  global.DvsPlaybackTasks = {
    list: list,
    get: get,
    refresh: refresh,
    subscribe: subscribe,
    statuses: statuses,
    terminal: terminal
  };

  refresh();
  if (global.setInterval) global.setInterval(refresh, 5000);
})(window);
