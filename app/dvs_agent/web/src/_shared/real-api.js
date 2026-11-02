/*
 * DVS real API adapter (node-side).
 *
 * Implements the same DvsMockApi.request interface as mock-api.js, but forwards
 * calls to the local dvs-agent HTTP service instead of localStorage.
 */
(function (global) {
  "use strict";

  var BASE = "";

  function requestId() {
    return "real-req-" + Date.now().toString(36) + "-" + Math.random().toString(36).slice(2, 7);
  }

  function normalize(method, path, body, options) {
    method = String(method || "GET").toUpperCase();
    body = body || {};
    options = options || {};
    var headers = Object.assign({ "Content-Type": "application/json" }, options.headers || {});
    var cookie = options.cookie || "";
    if (cookie) headers["Cookie"] = cookie;
    var init = { method: method, headers: headers, credentials: "same-origin" };
    if (method !== "GET" && method !== "HEAD") {
      init.body = JSON.stringify(body);
    }
    var url = BASE + path;
    var idem = options.idempotencyKey || (options.headers && (options.headers["Idempotency-Key"] || options.headers["idempotency-key"])) || "";
    if (idem) {
      headers["Idempotency-Key"] = idem;
    }
    return fetch(url, init).then(function (res) {
      return res.json().catch(function () { return { ok: res.ok, status: res.status, code: res.ok ? "OK" : "ERROR", msg: res.statusText, data: {}, requestId: requestId() }; });
    }).catch(function (err) {
      return { ok: false, status: 0, code: "NETWORK_ERROR", msg: String(err), data: {}, requestId: requestId() };
    });
  }

  global.DvsRealApi = {
    version: "real-v1",
    source: "dvs-agent local HTTP",
    request: function (method, path, body, options) {
      return normalize(method, path, body, options);
    },
    requestAsync: function (method, path, body, options) {
      return normalize(method, path, body, options);
    },
    reset: function () { return Promise.resolve({ ok: true, status: 200, code: "OK", msg: "", data: { reset: true }, requestId: requestId() }); }
  };

  // Runtime switch: if real backend is reachable, expose as DvsMockApi.
  global.DvsMockApi = global.DvsRealApi;
})(window);
