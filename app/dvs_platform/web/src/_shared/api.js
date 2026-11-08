/*
 * DVS real API adapter (same-origin fetch wrapper).
 *
 * Forwards calls to the co-served DVS HTTP service and normalizes every
 * reply into the uniform envelope {ok,status,code,msg,data,requestId}.
 * Carries Idempotency-Key through when given, and an Authorization bearer
 * token persisted as localStorage["dvs_api_token"] when the operator set one
 * (absent by default: the token gate stays open until DVS_API_TOKEN is set).
 */
(function (global) {
  "use strict";

  var BASE = "";

  function token() {
    try { return global.localStorage.getItem("dvs_api_token") || ""; }
    catch (e) { return ""; }
  }

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
    var tok = token();
    if (tok && !headers["Authorization"]) headers["Authorization"] = "Bearer " + tok;
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

  global.DvsApi = {
    version: "real-v1",
    source: "dvs HTTP (same-origin)",
    request: function (method, path, body, options) {
      return normalize(method, path, body || {}, options || {});
    },
    requestAsync: function (method, path, body, options) {
      return normalize(method, path, body || {}, options || {});
    },
    reset: function () { return Promise.resolve({ ok: true, status: 200, code: "OK", msg: "", data: { reset: true }, requestId: requestId() }); }
  };

})(window);
