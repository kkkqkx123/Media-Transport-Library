/* Shared platform state: localStorage is the offline cache, the platform
 * service is the truth. Group and wall basics (name/group/mode/type/
 * settings/volume/layoutMode/currentSceneId/imageAdjust) are written
 * through to the REST API; bindings/scenes/whitelist/runtime windows sync
 * with their server doc kinds (see the phase-2 sync section below).
 * Server failures reject loudly, never fork.
 */
(function (global) {
  "use strict";

  /* v2 drops the pre-unification device scheme: whitelist entries are keyed
   * by signal id (s*), the same ids scenes and windows reference. v1 state
   * is intentionally not migrated (no backward compatibility). */
  var STORAGE_KEY = "dvs.platform.state.v2";
  var listeners = [];

  function copy(value) { return value === undefined ? undefined : JSON.parse(JSON.stringify(value)); }
  function sameId(a, b) { return String(a) === String(b); }
  function nameKey(value) { return String(value || "").trim().toLocaleLowerCase(); }
  function stamp() {
    var d = new Date();
    function pad(n) { return String(n).padStart(2, "0"); }
    return d.getFullYear() + "-" + pad(d.getMonth() + 1) + "-" + pad(d.getDate()) + " " + pad(d.getHours()) + ":" + pad(d.getMinutes());
  }
  function uniqueId(prefix) { return prefix + "-" + Date.now().toString(36) + "-" + Math.random().toString(36).slice(2, 7); }
  function emptyBindings(rows, cols) {
    return Array.from({ length: rows }, function () { return Array.from({ length: cols }, function () { return null; }); });
  }
  function defaultSettings() {
    return {
      colorFormat: "4:2:0 转 4:4:4", quality: "中", buffer: 4,
      timeServerMode: "auto", timeServerAddress: "", syncMax: 120, syncMin: 60, syncStep: 5,
      image: { brightness: 50, contrast: 50, saturation: 50, hue: 180 }
    };
  }
  function baseWall(id, groupId, name, mode, type, rows, cols, res, createdAt) {
    return {
      id: id, groupId: groupId, name: name, mode: mode, type: type,
      rows: rows, cols: cols, res: res, createdAt: createdAt,
      firstRes: "1920×1080", rowPixels: Array.from({ length: rows }, function () { return 1080; }),
      colPixels: Array.from({ length: cols }, function () { return 1920; }),
      bindings: emptyBindings(rows, cols), whitelist: [], settings: defaultSettings(),
      backdrop: null, scenes: [{ id: "scene-" + id + "-default", name: "默认场景", isDefault: true, volume: 80, layout: [] }],
      currentSceneId: "scene-" + id + "-default", windows: [], operations: 0, layoutMode: "free", volume: 80,
      deletedAt: null
    };
  }
  /* Empty initial state: the browser holds no business truth. Real data
   * arrives via refreshFromServer; while the server is unreachable the
   * pages render empty instead of demo walls. Local edits stay drafts
   * until the owning page wires them to the matching REST call. */
  function seed() {
    return {
      version: 2,
      groups: [],
      walls: [],
      currentGroupId: null, currentWallId: null
    };
  }

  function fromStorage() {
    try {
      var raw = global.localStorage.getItem(STORAGE_KEY);
      if (raw) {
        var parsed = JSON.parse(raw);
        if (parsed && parsed.version === 2 && Array.isArray(parsed.groups) && Array.isArray(parsed.walls)) return parsed;
      }
    } catch (e) { /* Storage may be unavailable in a local file preview. */ }
    return null;
  }
  var state = fromStorage() || seed();
  function notify() {
    listeners.slice().forEach(function (listener) { try { listener(getState()); } catch (e) { /* keep other subscribers running */ } });
  }
  function persist() {
    try { global.localStorage.setItem(STORAGE_KEY, JSON.stringify(state)); } catch (e) { /* in-memory fallback */ }
    notify();
  }
  function mutate(fn) {
    state = fromStorage() || state;
    var result = fn(state);
    persist();
    return copy(result);
  }
  function activeWalls(source) { return source.walls.filter(function (wall) { return !wall.deletedAt; }); }
  function groupOrThrow(source, id) {
    var group = source.groups.find(function (item) { return sameId(item.id, id); });
    if (!group) throw new Error("分组不存在或已删除");
    return group;
  }
  function wallOrThrow(source, id) {
    var wall = activeWalls(source).find(function (item) { return sameId(item.id, id); });
    if (!wall) throw new Error("拼墙不存在或已删除");
    return wall;
  }
  function boundCount(wall) {
    if (!Array.isArray(wall.bindings)) return Number(wall.boxes) || 0;
    return wall.bindings.reduce(function (sum, row) { return sum + (Array.isArray(row) ? row.filter(Boolean).length : 0); }, 0);
  }
  function windowCount(wall) { return Array.isArray(wall.windows) ? wall.windows.length : (Number(wall.windows) || 0); }
  function sceneCount(wall) { return Array.isArray(wall.scenes) ? wall.scenes.length : (Number(wall.scenesCount) || 0); }
  function describeWall(source, wall) {
    var result = copy(wall);
    var group = source.groups.find(function (item) { return sameId(item.id, wall.groupId); });
    result.groupName = group ? group.name : "—";
    result.boundCount = boundCount(wall);
    result.totalCells = Number(wall.rows) * Number(wall.cols);
    result.bind = result.boundCount + "/" + result.totalCells;
    result.boxes = result.boundCount;
    result.windowCount = windowCount(wall);
    result.sceneCount = sceneCount(wall);
    return result;
  }
  function describeGroup(source, group) {
    var result = copy(group);
    result.wallCount = activeWalls(source).filter(function (wall) { return sameId(wall.groupId, group.id); }).length;
    return result;
  }
  function checkedName(value, label) {
    var name = String(value || "").trim();
    if (!name) throw new Error("请输入" + label);
    if (name.length > 20) throw new Error(label + "不能超过 20 个字符");
    return name;
  }
  function checkGroupName(source, name, exceptId) {
    if (source.groups.some(function (group) { return !sameId(group.id, exceptId) && nameKey(group.name) === nameKey(name); })) throw new Error("该分组名称已存在");
  }
  function checkWallName(source, groupId, name, exceptId) {
    if (activeWalls(source).some(function (wall) { return sameId(wall.groupId, groupId) && !sameId(wall.id, exceptId) && nameKey(wall.name) === nameKey(name); })) throw new Error("该分组下已存在同名拼墙");
  }
  function checkBindings(source, wall, bindings) {
    if (!Array.isArray(bindings) || bindings.length !== Number(wall.rows) || bindings.some(function (row) { return !Array.isArray(row) || row.length !== Number(wall.cols); })) {
      throw new Error("格位矩阵与拼墙行列不一致");
    }
    var seenPorts = {};
    bindings.forEach(function (row) { row.forEach(function (cell) {
      if (!cell) return;
      var id = String(cell.node || "").toLowerCase();
      var port = Number(cell.port);
      if (!id || !Number.isInteger(port) || port < 1) throw new Error("输出盒或输出口无效");
      if (seenPorts[id + ":" + port]) throw new Error("同一输出口不能绑定多个格位");
      seenPorts[id + ":" + port] = true;
      var other = activeWalls(source).find(function (item) {
        return item.id !== wall.id && (item.bindings || []).some(function (cells) {
          return Array.isArray(cells) && cells.some(function (binding) { return binding && String(binding.node).toLowerCase() === id; });
        });
      });
      if (other) throw new Error("输出盒已绑定拼墙「" + other.name + "」");
      if (global.DvsNodeState) {
        var node = global.DvsNodeState.get(id);
        if (!node || node.lifecycle !== "active" || node.type !== "output") throw new Error("输出盒已退役或未注册");
      }
    }); });
  }
  function getState() { return copy(state); }
  function listGroups() { return state.groups.map(function (group) { return describeGroup(state, group); }).sort(function (a, b) { return b.createdAt.localeCompare(a.createdAt); }); }
  function getGroup(id) {
    var group = state.groups.find(function (item) { return sameId(item.id, id == null ? state.currentGroupId : id); });
    return group ? describeGroup(state, group) : null;
  }
  function createGroup(name) {
    return Promise.resolve().then(function () {
      name = checkedName(name, "分组名称");
      checkGroupName(state, name, null);
      var id = uniqueId("g");
      return serverCall("POST", "/api/v1/groups", { id: id, name: name }).then(function (data) {
        return mutate(function (source) {
          checkGroupName(source, name, null);
          var group = { id: (data && data.id) || id, name: (data && data.name) || name, createdAt: (data && data.createdAt) || stamp() };
          if (!source.groups.some(function (item) { return sameId(item.id, group.id); })) source.groups.unshift(group);
          return describeGroup(source, group);
        });
      });
    });
  }
  function renameGroup(id, name) {
    return Promise.resolve().then(function () {
      name = checkedName(name, "分组名称");
      var cur = getGroup(id);
      if (!cur) throw new Error("分组不存在或已删除");
      checkGroupName(state, name, cur.id);
      return putMergedDoc("groups", cur.id, { name: name }).then(function () {
        return mutate(function (source) {
          var group = groupOrThrow(source, cur.id);
          checkGroupName(source, name, group.id);
          group.name = name;
          return describeGroup(source, group);
        });
      });
    });
  }
  function deleteGroup(id) {
    return Promise.resolve().then(function () {
      var cur = getGroup(id);
      if (!cur) throw new Error("分组不存在或已删除");
      if (listWalls(cur.id).length) throw new Error("该分组下还有拼墙，请先移走或删除");
      return serverCall("DELETE", "/api/v1/groups/" + encodeURIComponent(cur.id)).then(function () {
        return mutate(function (source) {
          groupOrThrow(source, cur.id);
          source.groups = source.groups.filter(function (item) { return !sameId(item.id, cur.id); });
          if (sameId(source.currentGroupId, cur.id)) {
            source.currentGroupId = source.groups.length ? source.groups[0].id : null;
            source.currentWallId = null;
          }
          return cur;
        });
      });
    });
  }
  function listWalls(groupId) {
    var walls = activeWalls(state);
    if (groupId != null) walls = walls.filter(function (wall) { return sameId(wall.groupId, groupId); });
    return walls.map(function (wall) { return describeWall(state, wall); });
  }
  function getWall(id) {
    var wall = activeWalls(state).find(function (item) { return sameId(item.id, id == null ? state.currentWallId : id); });
    return wall ? describeWall(state, wall) : null;
  }
  /* Wall basics (name/group/mode/type/settings/volume/layoutMode/
   * currentSceneId/imageAdjust) are server-mapped and written through.
   * bindings/scenes/whitelist/windows route to their server doc kinds
   * (see syncBindings/syncScenes/syncWhitelist/syncRuntimeWindows). */
  var WALL_SERVER_KEYS = ["name", "groupId", "mode", "type", "settings",
    "volume", "layoutMode", "currentSceneId", "imageAdjust"];
  function createWall(input) {
    return Promise.resolve().then(function () {
      input = input || {};
      var groupId = input.groupId == null ? state.currentGroupId : String(input.groupId);
      groupOrThrow(state, groupId);
      var name = checkedName(input.name, "拼墙名称");
      checkWallName(state, groupId, name, null);
      var rows = Number(input.rows), cols = Number(input.cols);
      if (!Number.isInteger(rows) || rows < 1 || rows > 6) throw new Error("行数取值 1~6");
      if (!Number.isInteger(cols) || cols < 1 || cols > 15) throw new Error("列数取值 1~15");
      var mode = input.mode === "LED" ? "LED" : "LCD";
      var type = mode === "LED" ? "" : (input.type === "非标准" ? "非标准" : "标准");
      var custom = mode === "LED" || type === "非标准";
      var rowPixels = custom ? copy(input.rowPixels || []) : [];
      var colPixels = custom ? copy(input.colPixels || []) : [];
      var firstRes = input.firstRes || "1920×1080";
      var width, height;
      if (custom) {
        if (rowPixels.length !== rows || colPixels.length !== cols || rowPixels.concat(colPixels).some(function (px) { return !Number.isInteger(Number(px)) || Number(px) < 64 || Number(px) > 7680; })) throw new Error("单格像素取值 64~7680");
        width = colPixels.reduce(function (sum, px) { return sum + Number(px); }, 0);
        height = rowPixels.reduce(function (sum, px) { return sum + Number(px); }, 0);
      } else {
        var parts = String(firstRes).split("×").map(Number);
        if (parts.length !== 2 || parts.some(function (px) { return !Number.isInteger(px) || px <= 0; })) throw new Error("请选择首格分辨率");
        width = parts[0] * cols;
        height = parts[1] * rows;
        rowPixels = Array.from({ length: rows }, function () { return parts[1]; });
        colPixels = Array.from({ length: cols }, function () { return parts[0]; });
      }
      var id = uniqueId("wall");
      var body = { id: id, name: name, groupId: groupId, rows: rows, cols: cols, mode: mode, type: type,
        rowPixels: rowPixels.map(Number), colPixels: colPixels.map(Number) };
      return serverCall("POST", "/api/v1/walls", body).then(function (data) {
        var sid = (data && data.id) || id;
        return mutate(function (source) {
          groupOrThrow(source, groupId);
          checkWallName(source, groupId, name, null);
          var wall = baseWall(sid, groupId, name, mode, type, rows, cols, width + "×" + height + (custom ? "（逐行逐列）" : ""), stamp());
          /* Server-side default scene id is <wallId>-default; adopt it so
           * later scene syncs match instead of duplicating. */
          wall.scenes = [{ id: sid + "-default", serverId: sid + "-default", name: "默认场景", isDefault: true, volume: 80, layout: [] }];
          wall.currentSceneId = sid + "-default";
          wall.firstRes = custom ? null : firstRes;
          wall.rowPixels = rowPixels.map(Number);
          wall.colPixels = colPixels.map(Number);
          if (!source.walls.some(function (item) { return sameId(item.id, sid); })) source.walls.unshift(wall);
          return describeWall(source, wall);
        });
      });
    });
  }
  /* updateWall routes each patch key to its owning server doc kind:
   * bindings -> bindings docs, scenes -> scenes docs (adopted ids flow
   * back into the local view), whitelist -> allocate/delete, windows ->
   * current scene doc, the rest -> wall doc. Server first, local after. */
  function updateWall(id, patch) {
    return Promise.resolve().then(function () {
      patch = patch || {};
      var wall = getWall(id);
      if (!wall) throw new Error("拼墙不存在或已删除");
      var nextGroup = patch.groupId == null ? wall.groupId : String(patch.groupId);
      groupOrThrow(state, nextGroup);
      var nextName = patch.name == null ? wall.name : checkedName(patch.name, "拼墙名称");
      checkWallName(state, nextGroup, nextName, wall.id);
      if (patch.bindings !== undefined) checkBindings(state, wall, patch.bindings);
      var idem = "update-wall-" + id + "-" + Date.now().toString(36);
      var idMap = {};
      var run = Promise.resolve();
      if (patch.bindings !== undefined) run = run.then(function () { return syncBindings(id, patch.bindings, idem); });
      if (patch.whitelist !== undefined) {
        var oldWl = (wall.whitelist || []).map(function (item) { return { id: item.id }; });
        run = run.then(function () {
          return syncWhitelist(id, patch.whitelist, oldWl, idem).then(function (enriched) { patch.whitelist = enriched; });
        });
      }
      if (patch.scenes !== undefined) {
        run = run.then(function () {
          return syncScenes(id, patch.scenes, idem).then(function (r) {
            idMap = r.idMap || {};
            patch.scenes = r.scenes;
            if (patch.currentSceneId != null && idMap[String(patch.currentSceneId)]) {
              patch.currentSceneId = idMap[String(patch.currentSceneId)];
            }
          });
        });
      }
      if (patch.windows !== undefined) {
        run = run.then(function () {
          var sid = patch.currentSceneId != null ? patch.currentSceneId : getWall(id).currentSceneId;
          if (sid != null && idMap[String(sid)]) sid = idMap[String(sid)];
          return syncRuntimeWindows(id, patch.windows, sid, idem);
        });
      }
      var mapped = {};
      WALL_SERVER_KEYS.forEach(function (k) { if (patch[k] !== undefined) mapped[k] = patch[k]; });
      if (Object.keys(mapped).length) {
        run = run.then(function () { return putMergedDoc("walls", id, mapped, { idempotencyKey: idem + "-wall" }); });
      }
      return run.then(applyLocal);
      function applyLocal() {
        return mutate(function (source) {
          var target = wallOrThrow(source, wall.id);
          var ng = patch.groupId == null ? target.groupId : String(patch.groupId);
          groupOrThrow(source, ng);
          var nn = patch.name == null ? target.name : checkedName(patch.name, "拼墙名称");
          checkWallName(source, ng, nn, target.id);
          if (patch.bindings !== undefined) checkBindings(source, target, patch.bindings);
          Object.keys(patch).forEach(function (key) {
            if (patch[key] !== undefined && key !== "id" && key !== "deletedAt" && key !== "createdAt" && key !== "groupName" && key !== "boundCount" && key !== "totalCells" && key !== "sceneCount" && key !== "windowCount" && key !== "bind" && key !== "boxes") target[key] = copy(patch[key]);
          });
          target.groupId = ng;
          target.name = nn;
          return describeWall(source, target);
        });
      }
    });
  }
  function deleteWall(id) {
    return Promise.resolve().then(function () {
      var wall = getWall(id);
      if (!wall) throw new Error("拼墙不存在或已删除");
      if (wall.boundCount || wall.windowCount || Number(wall.operations)) throw new Error("请先清屏、解绑并等待操作结束");
      return serverCall("DELETE", "/api/v1/walls/" + encodeURIComponent(wall.id)).then(function () {
        return mutate(function (source) {
          var target = wallOrThrow(source, wall.id);
          target.deletedAt = new Date().toISOString();
          if (sameId(source.currentWallId, wall.id)) {
            var replacement = activeWalls(source).find(function (item) { return sameId(item.groupId, target.groupId); });
            source.currentWallId = replacement ? replacement.id : null;
          }
          return describeWall(source, target);
        });
      });
    });
  }
  function restoreWall(id) {
    return Promise.resolve().then(function () {
      var found = state.walls.find(function (item) { return sameId(item.id, id); });
      if (!found || !found.deletedAt) throw new Error("未找到可恢复的拼墙");
      return serverCall("POST", "/api/v1/walls/" + encodeURIComponent(found.id) + "/restore", {}).then(function () {
        return mutate(function (source) {
          var wall = source.walls.find(function (item) { return sameId(item.id, found.id); });
          if (!wall || !wall.deletedAt) throw new Error("未找到可恢复的拼墙");
          groupOrThrow(source, wall.groupId);
          checkWallName(source, wall.groupId, wall.name, wall.id);
          wall.deletedAt = null;
          wall.bindings = emptyBindings(wall.rows, wall.cols);
          return describeWall(source, wall);
        });
      });
    });
  }
  function setCurrentGroup(id) {
    return mutate(function (source) {
      var group = groupOrThrow(source, id);
      source.currentGroupId = group.id;
      var first = activeWalls(source).find(function (wall) { return sameId(wall.groupId, group.id); });
      source.currentWallId = first ? first.id : null;
      return describeGroup(source, group);
    });
  }
  function setCurrentWall(id) {
    return mutate(function (source) {
      var wall = wallOrThrow(source, id);
      source.currentWallId = wall.id;
      source.currentGroupId = wall.groupId;
      return describeWall(source, wall);
    });
  }
  function reset() { state = seed(); persist(); return getState(); }
  function subscribe(fn) {
    if (typeof fn !== "function") return function () {};
    listeners.push(fn);
    return function () { listeners = listeners.filter(function (item) { return item !== fn; }); };
  }
  if (global.addEventListener) global.addEventListener("storage", function (event) {
    if (event.key === STORAGE_KEY) { state = fromStorage() || seed(); notify(); }
  });
  function serverItems(payload) {
    if (!payload || !payload.ok) return null;
    var d = payload.data;
    if (Array.isArray(d)) return d;
    if (d && Array.isArray(d.items)) return d.items;
    return null;
  }
  function serverCall(method, path, body, options) {
    if (!global.DvsApi || typeof global.DvsApi.request !== "function") {
      return Promise.reject(new Error("平台服务不可达"));
    }
    return global.DvsApi.request(method, path, body || {}, options || {}).then(function (r) {
      if (r && r.ok) return r.data == null ? {} : r.data;
      throw new Error((r && r.msg) || "平台服务请求失败");
    });
  }
  /* Full-document PUT without dropping server-owned keys: read the stored
   * doc, overlay the mapped patch, write it back. */
  function putMergedDoc(kind, id, patch, options) {
    return serverCall("GET", "/api/v1/" + kind + "/" + encodeURIComponent(id)).then(function (doc) {
      doc = (doc && typeof doc === "object") ? doc : { id: id };
      Object.keys(patch).forEach(function (k) { doc[k] = copy(patch[k]); });
      return serverCall("PUT", "/api/v1/" + kind + "/" + encodeURIComponent(id), doc, options);
    });
  }
  /* ---------- Phase 2: server doc sync ---------- */
  /* Canvas units stay logical on screen; the server counts wall pixels.
   * gridOf is the authoritative per-cell pixel table from the joined wall
   * doc (standard walls carry uniform tables via firstRes). */
  function numList(value) {
    if (!Array.isArray(value) || !value.length) return null;
    var out = value.map(Number);
    if (out.some(function (n) { return !isFinite(n) || n <= 0; })) return null;
    return out;
  }
  function gridOf(wall) {
    wall = wall || {};
    var rows = Number(wall.rows) || 1, cols = Number(wall.cols) || 1;
    var rowPx = numList(wall.rowPixels);
    var colPx = numList(wall.colPixels);
    var i;
    if (!rowPx || rowPx.length !== rows) {
      var rh = 1080;
      if (Number(wall.wall_px_h) > 0) rh = Math.max(64, Math.round(Number(wall.wall_px_h) / rows));
      else if (Number(wall.h) > 0) rh = Math.max(64, Math.round(Number(wall.h) / rows));
      rowPx = [];
      for (i = 0; i < rows; i++) rowPx.push(rh);
    }
    if (!colPx || colPx.length !== cols) {
      var cw0 = 1920;
      if (Number(wall.wall_px_w) > 0) cw0 = Math.max(64, Math.round(Number(wall.wall_px_w) / cols));
      else if (Number(wall.w) > 0) cw0 = Math.max(64, Math.round(Number(wall.w) / cols));
      colPx = [];
      for (i = 0; i < cols; i++) colPx.push(cw0);
    }
    return { rows: rows, cols: cols, rowPx: rowPx, colPx: colPx };
  }
  function cellIdOf(r, c) { return (r + 1) + "-" + (c + 1); }
  function parseCellId(id) {
    var m = /^(\d+)-(\d+)$/.exec(String(id || ""));
    if (!m) return null;
    return { r: Number(m[1]) - 1, c: Number(m[2]) - 1 };
  }
  function pxOffset(t, i) { var x = 0; for (var k = 0; k < i; k++) x += t[k] || 0; return x; }
  function pxSpan(t, i, n) { var s = 0; for (var k = i; k < i + n; k++) s += t[k] || 0; return s; }
  function serverItemsOf(payload) {
    if (!payload) return [];
    if (Array.isArray(payload)) return payload.slice();
    return (payload.items || []).slice();
  }
  /* Server scene -> local scene view. muted round-trips as a tolerated
   * extra key; z stays a local render concern. */
  function serverSceneToLocal(wallId, doc) {
    doc = doc || {};
    return {
      id: doc.id, serverId: doc.id, name: doc.name == null ? String(doc.id) : String(doc.name),
      isDefault: String(doc.id) === String(wallId) + "-default",
      volume: doc.volume == null ? 80 : Number(doc.volume),
      layout: ((doc.windows && doc.windows.slice()) || []).map(function (w) {
        return { sig: w.sig, x: Number(w.x) || 0, y: Number(w.y) || 0,
          w: Number(w.w) || 0, h: Number(w.h) || 0, muted: !!w.muted };
      })
    };
  }
  /* Local layout item -> server window. Legacy grid snapshots convert via
   * the wall pixel tables; pixel snapshots pass through. */
  function layoutToServerWindows(wall, sceneId, layout) {
    var g = gridOf(wall);
    return (layout || []).map(function (item, i) {
      var r = { id: "w-" + sceneId + "-" + i, sig: item.sig, x: 0, y: 0, w: 64, h: 64 };
      if (item.x != null) {
        r.x = Math.round(Number(item.x) || 0);
        r.y = Math.round(Number(item.y) || 0);
        r.w = Math.max(64, Math.round(Number(item.w) || 0));
        r.h = Math.max(64, Math.round(Number(item.h) || 0));
      } else {
        var c = Math.max(0, Number(item.c) || 0), rr = Math.max(0, Number(item.r) || 0);
        var cs = Math.max(1, Number(item.cs) || 1), rs = Math.max(1, Number(item.rs) || 1);
        r.x = pxOffset(g.colPx, c);
        r.y = pxOffset(g.rowPx, rr);
        r.w = Math.max(64, pxSpan(g.colPx, c, cs));
        r.h = Math.max(64, pxSpan(g.rowPx, rr, rs));
      }
      if (item.muted != null) r.muted = !!item.muted;
      return r;
    });
  }
  function localSceneToServer(wall, scene, serverId) {
    return {
      id: serverId, wallId: wall.id, name: scene.name, snapshotVersion: 1,
      volume: Math.max(0, Math.min(100, Number(scene.volume == null ? 80 : scene.volume))),
      layoutMode: wall.layoutMode === "fixed" ? "fixed" : "free",
      windows: layoutToServerWindows(wall, serverId, scene.layout)
    };
  }
  /* Matrix -> bindings docs: diff by cellId, POST new / PUT changed /
   * DELETE removed. Server conflict checks are authoritative. */
  function syncBindings(wallId, matrix, idemKey) {
    var key = idemKey || ("sync-bind-" + wallId + "-" + Date.now().toString(36));
    return serverCall("GET", "/api/v1/bindings").then(function (all) {
      var grid = gridOf(getWall(wallId) || {});
      var current = {};
      all.forEach(function (b) {
        if (b && String(b.wallId) === String(wallId) && b.id) current[String(b.cellId || "")] = b;
      });
      var desired = {};
      (matrix || []).forEach(function (row, r) {
        (row || []).forEach(function (cell, c) {
          if (!cell || !cell.node) return;
          if (r >= grid.rows || c >= grid.cols) return;
          desired[cellIdOf(r, c)] = { nodeId: String(cell.node), port: Math.max(1, Math.floor(Number(cell.port) || 1)) };
        });
      });
      var ops = [];
      Object.keys(desired).forEach(function (cellId) {
        var want = desired[cellId];
        var have = current[cellId];
        if (!have) {
          ops.push(serverCall("POST", "/api/v1/bindings",
            { id: "b-" + wallId + "-" + cellId, wallId: String(wallId), nodeId: want.nodeId, cellId: cellId, outputPort: want.port },
            { idempotencyKey: key + "-add-" + cellId }));
        } else if (String(have.nodeId) !== want.nodeId || Number(have.outputPort) !== want.port) {
          ops.push(putMergedDoc("bindings", have.id,
            { nodeId: want.nodeId, cellId: cellId, outputPort: want.port },
            { idempotencyKey: key + "-upd-" + cellId }));
        }
      });
      Object.keys(current).forEach(function (cellId) {
        if (!cellId) return; /* 无格位归属的孤儿条目不碰：映射不清不删。 */
        if (!desired[cellId] && current[cellId] && current[cellId].id) {
          ops.push(serverCall("DELETE", "/api/v1/bindings/" + encodeURIComponent(current[cellId].id),
            {}, { idempotencyKey: key + "-del-" + cellId }));
        }
      });
      return Promise.all(ops);
    });
  }
  /* Local scenes -> scenes docs. Drafts (no serverId) are POSTed and the
   * stored doc (snapped when fixed) is adopted back; the wall default
   * adopts the server-generated <wallId>-default doc. */
  function syncScenes(wallId, scenes, idemKey) {
    var key = idemKey || ("sync-scenes-" + wallId + "-" + Date.now().toString(36));
    var wall = getWall(wallId) || { id: wallId };
    return serverCall("GET", "/api/v1/scenes").then(function (all) {
      var current = {};
      all.forEach(function (s) {
        if (s && String(s.wallId) === String(wallId) && s.id) current[String(s.id)] = s;
      });
      var defaultId = String(wallId) + "-default";
      var adopted = [], idMap = {};
      var ops = (scenes || []).map(function (scene, i) {
        var sid = scene.serverId ||
          (typeof scene.id === "string" && scene.id.indexOf("draft-") !== 0 ? scene.id : null);
        var stamp = function (saved) {
          var view = serverSceneToLocal(wallId, saved);
          view.name = scene.name;
          view.isDefault = !!scene.isDefault;
          view.volume = scene.volume == null ? view.volume : Number(scene.volume);
          adopted[i] = view;
          idMap[String(scene.id)] = view.id;
          if (scene.serverId) idMap[String(scene.serverId)] = view.id;
        };
        if (sid && current[sid]) {
          return putMergedDoc("scenes", sid, localSceneToServer(wall, scene, sid),
            { idempotencyKey: key + "-upd-" + i }).then(stamp);
        }
        if (!sid && scene.isDefault && current[defaultId]) {
          return putMergedDoc("scenes", defaultId, localSceneToServer(wall, scene, defaultId),
            { idempotencyKey: key + "-adopt-" + i }).then(stamp);
        }
        var nid = sid || ("sc-" + String(wallId) + "-" + Date.now().toString(36) + "-" + i);
        return serverCall("POST", "/api/v1/scenes", localSceneToServer(wall, scene, nid),
          { idempotencyKey: key + "-add-" + i }).then(stamp);
      });
      return Promise.all(ops).then(function () { return { scenes: adopted, idMap: idMap }; });
    });
  }
  function wallListsSignal(sig, exceptWallId) {
    var k = String(sig).toLowerCase();
    return activeWalls(state).some(function (wall) {
      if (String(wall.id) === String(exceptWallId)) return false;
      return (wall.whitelist || []).some(function (item) { return String(item.id).toLowerCase() === k; });
    });
  }
  /* Whitelist membership stays local; every member gets a server allocation
   * via the idempotent allocate call. Removals delete the server entry
   * unless another wall still lists the signal. */
  function syncWhitelist(wallId, whitelist, oldWhitelist, idemKey) {
    var key = idemKey || ("sync-wl-" + wallId + "-" + Date.now().toString(36));
    return serverCall("GET", "/api/v1/whitelist").then(function (all) {
      var bySig = {};
      all.forEach(function (w) {
        if (!w) return;
        bySig[String(w.sig == null ? w.id : w.sig).toLowerCase()] = w;
      });
      var ops = [];
      var enriched = (whitelist || []).map(function (item) {
        var sig = String(item.id);
        var hit = bySig[sig.toLowerCase()];
        var out = copy(item);
        var wantPort = Math.max(1, Math.min(65535, Math.floor(Number(item.port) || 50010)));
        if (hit) {
          out.serverAlloc = { address: hit.address || "", port: hit.port == null ? wantPort : Number(hit.port), status: hit.status || "" };
          if (Number(hit.port) !== wantPort) {
            ops.push(putMergedDoc("whitelist", hit.id, { port: wantPort },
              { idempotencyKey: key + "-port-" + sig }));
            out.serverAlloc.port = wantPort;
          }
        } else {
          ops.push(serverCall("POST", "/api/v1/whitelist/allocate",
            { sig: sig, port: wantPort },
            { idempotencyKey: key + "-add-" + sig }).then(function (saved) {
              out.serverAlloc = {
                address: (saved && saved.address) || "", port: (saved && saved.port) || wantPort,
                status: (saved && saved.status) || ""
              };
            }));
        }
        return out;
      });
      return Promise.all(ops).then(function () {
        var keep = {};
        enriched.forEach(function (item) { keep[String(item.id).toLowerCase()] = true; });
        var dels = [];
        (oldWhitelist || []).forEach(function (item) {
          var k = String(item.id).toLowerCase();
          if (keep[k]) return;
          var hit = bySig[k];
          if (!hit || !hit.id) return;
          if (wallListsSignal(item.id, wallId)) return;
          dels.push(serverCall("DELETE", "/api/v1/whitelist/" + encodeURIComponent(hit.id),
            {}, { idempotencyKey: key + "-del-" + k }));
        });
        return Promise.all(dels).then(function () { return enriched; });
      });
    });
  }
  /* Runtime windows live on the current scene doc, not on the wall doc. */
  function syncRuntimeWindows(wallId, windows, sceneServerId, idemKey) {
    var wall = getWall(wallId);
    if (!wall) return Promise.reject(new Error("拼墙不存在或已删除"));
    var sid = sceneServerId || wall.currentSceneId;
    if (!sid) return Promise.reject(new Error("当前场景未同步，请先保存场景"));
    var key = idemKey || ("sync-rt-" + wallId + "-" + Date.now().toString(36));
    return putMergedDoc("scenes", sid,
      { windows: layoutToServerWindows(wall, sid, windows || []) },
      { idempotencyKey: key }).then(function () { return true; });
  }
  function deleteServerScene(id) {
    return serverCall("DELETE", "/api/v1/scenes/" + encodeURIComponent(id), {},
      { idempotencyKey: "del-scene-" + id });
  }
  function postBaseImageRecord(wallId, rec) {
    return serverCall("POST", "/api/v1/walls/" + encodeURIComponent(wallId) + "/base-image",
      { revision: String(rec.revision), width: Number(rec.width) || 0, height: Number(rec.height) || 0 },
      { idempotencyKey: "base-image-" + wallId + "-" + String(rec.revision) });
  }
  /* Wall prepare without members: the server derives members from the
   * active bindings and enforces revision = applied + 1. */
  function prepareWallOnce(op) {
    return serverCall("GET", "/api/v1/walls/" + encodeURIComponent(op.wallId)).then(function (wdoc) {
      var rev = Number((wdoc && wdoc.appliedRevision) || 0) + 1;
      var opId = op.operationId || ("op-" + String(op.wallId) + "-" + Date.now().toString(36));
      return serverCall("POST", "/api/v1/ops/prepare", {
        operationId: opId, revision: rev, wallId: String(op.wallId),
        sceneId: op.sceneId, windows: op.windows || [], volume: op.volume == null ? 80 : Number(op.volume)
      }, { idempotencyKey: opId }).then(function (data) {
        return { operationId: opId, revision: rev, data: data };
      });
    });
  }
  function prepareWall(op) {
    return prepareWallOnce(op).catch(function (err) {
      if (err && /revision not newer|EXPIRED/.test(err.message || "")) return prepareWallOnce(op);
      throw err;
    });
  }
  function markSceneCurrent(wallId, sceneId) {
    return mutate(function (source) {
      var target = wallOrThrow(source, wallId);
      target.currentSceneId = sceneId;
      return describeWall(source, target);
    });
  }
  /* Adapt a server wall doc to the local wall view: real geometry from
   * rows/cols/rowPixels/colPixels; bindings/scenes/whitelist come from
   * the detail join (refreshRefs/loadWallDetail), never from placeholders. */
  function adaptServerWall(w) {
    w = w || {};
    var g = gridOf({ rows: w.rows, cols: w.cols, rowPixels: w.rowPixels, colPixels: w.colPixels,
      wall_px_w: w.wall_px_w, wall_px_h: w.wall_px_h, w: w.w, h: w.h });
    return {
      id: w.id, groupId: w.groupId || w.group || null, name: w.name || w.id,
      mode: w.mode || "", type: w.type || "", rows: g.rows, cols: g.cols,
      res: pxSpan(g.colPx, 0, g.cols) + "×" + pxSpan(g.rowPx, 0, g.rows),
      rowPixels: g.rowPx, colPixels: g.colPx, firstRes: null,
      bindings: emptyBindings(g.rows, g.cols), whitelist: [], settings: w.settings || null,
      backdrop: w.backdrop || null, scenes: [],
      currentSceneId: w.currentSceneId || null, windows: [], operations: 0,
      layoutMode: w.layoutMode || "free", volume: w.volume == null ? 80 : Number(w.volume),
      appliedRevision: w.appliedRevision == null ? 0 : Number(w.appliedRevision),
      deletedAt: null, createdAt: w.createdAt || "", serverDoc: true
    };
  }
  /* Join server bindings into every local wall matrix (+whitelist alloc
   * annotations). Server wins; unreachable keeps the local cache. */
  function refreshRefs() {
    return Promise.all([
      serverCall("GET", "/api/v1/bindings").catch(function () { return null; }),
      serverCall("GET", "/api/v1/whitelist").catch(function () { return null; })
    ]).then(function (results) {
      var bindings = serverItemsOf(results[0]);
      var allocBySig = {};
      serverItemsOf(results[1]).forEach(function (w) {
        if (!w) return;
        allocBySig[String(w.sig == null ? w.id : w.sig).toLowerCase()] =
          { address: w.address || "", port: w.port == null ? 0 : Number(w.port), status: w.status || "" };
      });
      mutate(function (source) {
        activeWalls(source).forEach(function (wall) {
          var g = gridOf(wall);
          var matrix = emptyBindings(g.rows, g.cols);
          bindings.forEach(function (b) {
            if (!b || String(b.wallId) !== String(wall.id)) return;
            var pos = parseCellId(b.cellId);
            if (!pos || pos.r < 0 || pos.c < 0 || pos.r >= g.rows || pos.c >= g.cols) return;
            var port = Number(b.outputPort);
            matrix[pos.r][pos.c] = { node: String(b.nodeId || ""), port: Number.isInteger(port) && port >= 1 ? port : 1 };
          });
          wall.bindings = matrix;
          (wall.whitelist || []).forEach(function (item) {
            var hit = allocBySig[String(item.id).toLowerCase()];
            if (hit) item.serverAlloc = hit;
            else delete item.serverAlloc;
          });
        });
        return true;
      });
      return true;
    });
  }
  /* Per-wall deep join for detail pages: fresh geometry + scenes + runtime
   * windows from the server; local whitelist membership and unsynced scene
   * drafts are preserved. Rejects when offline (caller falls back to cache). */
  function loadWallDetail(wallId) {
    return refreshRefs().then(function () {
      return Promise.all([
        serverCall("GET", "/api/v1/walls/" + encodeURIComponent(wallId)),
        serverCall("GET", "/api/v1/scenes")
      ]);
    }).then(function (results) {
      var wdoc = results[0] || {};
      var scenes = serverItemsOf(results[1]).filter(function (s) { return s && String(s.wallId) === String(wallId); });
      return mutate(function (source) {
        var target = wallOrThrow(source, wallId);
        ["rows", "cols", "rowPixels", "colPixels", "mode", "type", "settings",
         "volume", "layoutMode", "appliedRevision", "currentSceneId"].forEach(function (k) {
          if (wdoc[k] !== undefined) target[k] = copy(wdoc[k]);
        });
        if (wdoc.groupId != null) target.groupId = wdoc.groupId;
        if (wdoc.name != null) target.name = wdoc.name;
        var total = gridOf(target);
        target.res = pxSpan(total.colPx, 0, total.cols) + "×" + pxSpan(total.rowPx, 0, total.rows);
        var views = scenes.map(function (s) { return serverSceneToLocal(wallId, s); });
        var drafts = (target.scenes || []).filter(function (s) { return !s.serverId; });
        target.scenes = views.concat(drafts);
        if (!target.currentSceneId || !target.scenes.some(function (s) { return String(s.id) === String(target.currentSceneId); })) {
          var def = target.scenes.filter(function (s) { return s.isDefault; })[0] || target.scenes[0];
          if (def) target.currentSceneId = def.id;
        }
        var cur = target.scenes.filter(function (s) { return String(s.id) === String(target.currentSceneId); })[0];
        target.windows = (cur ? cur.layout : []).map(function (item) {
          return { id: item.sig, sig: item.sig, x: item.x, y: item.y, w: item.w, h: item.h, z: 0, muted: !!item.muted };
        });
        return describeWall(source, target);
      });
    });
  }
  /* Pull groups/walls from the server and replace the local state. Server
   * wins on conflict; unreachable keeps the local drafts. Never throws. */
  function refreshFromServer() {
    if (!global.DvsApi || typeof global.DvsApi.request !== "function") {
      return Promise.resolve(false);
    }
    return Promise.all([
      global.DvsApi.request("GET", "/api/v1/groups"),
      global.DvsApi.request("GET", "/api/v1/walls")
    ]).then(function (results) {
      var groups = serverItems(results[0]);
      var walls = serverItems(results[1]);
      if (!groups && !walls) return false;
      state = {
        version: 2,
        groups: (groups || []).map(function (g) {
          return { id: g.id, name: g.name || g.id, createdAt: g.createdAt || "" };
        }),
        walls: (walls || []).map(adaptServerWall),
        currentGroupId: null, currentWallId: null
      };
      if (state.groups.length) state.currentGroupId = state.groups[0].id;
      var firstWall = state.walls.filter(function (w) { return sameId(w.groupId, state.currentGroupId); })[0] || state.walls[0];
      if (firstWall) {
        state.currentWallId = firstWall.id;
        state.currentGroupId = firstWall.groupId;
      }
      persist();
      return refreshRefs().then(function () { return true; }, function () { return true; });
    }).catch(function (err) {
      if (global.console && global.console.warn) global.console.warn("platform store refresh failed", err);
      return false;
    });
  }
  if (global.DvsApi && typeof global.DvsApi.request === "function") refreshFromServer();
  global.DvsPlatformStore = {
    storageKey: STORAGE_KEY, getState: getState, reset: reset, subscribe: subscribe,
    refreshFromServer: refreshFromServer, refreshRefs: refreshRefs, loadWallDetail: loadWallDetail,
    gridOf: gridOf,
    listGroups: listGroups, getGroup: getGroup, createGroup: createGroup, renameGroup: renameGroup, deleteGroup: deleteGroup,
    listWalls: listWalls, getWall: getWall, createWall: createWall, updateWall: updateWall, deleteWall: deleteWall, restoreWall: restoreWall,
    currentGroupId: function () { return state.currentGroupId; }, currentWallId: function () { return state.currentWallId; },
    syncBindings: syncBindings, syncScenes: syncScenes, syncWhitelist: syncWhitelist,
    syncRuntimeWindows: syncRuntimeWindows, deleteServerScene: deleteServerScene,
    postBaseImageRecord: postBaseImageRecord, prepareWall: prepareWall, markSceneCurrent: markSceneCurrent,
    setCurrentGroup: setCurrentGroup, setCurrentWall: setCurrentWall,
    getCurrentGroup: function () { return getGroup(); }, getCurrentWall: function () { return getWall(); }
  };
})(window);
