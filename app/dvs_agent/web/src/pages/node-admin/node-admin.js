      (function () {
        var nodeId = null;
        var nodeSnapshot = {};
        var currentRole = "output";
        var pendingRole = null;
        var opsRunning = false;
        var fwReady = false;
        var pendingFwFile = null;
        var pendingFwSha = "";
        var registrationBusy = false;
        var configJobs = {};
        var nodeCapabilities = DvsHardwareCapabilities.resolve({});
        var mediaState = {};

        function apiAsync(method, path, body, opts) {
          return DvsApi.requestAsync(method, path, body || {}, opts || {});
        }

        function loadNodeInfo() {
          return apiAsync("GET", "/api/v1/node/info").then(function (r) {
            if (!r || !r.ok) return r;
            nodeSnapshot = r.data || {};
            nodeId = nodeSnapshot.id || nodeId;
            currentRole = nodeSnapshot.role || "output";
            pendingRole = nodeSnapshot.pendingRole || null;
            nodeCapabilities = DvsHardwareCapabilities.resolve(nodeSnapshot);
            return r;
          });
        }
        function loadMediaState() {
          return apiAsync("GET", "/api/v1/node/media/status").then(function (r) {
            if (r && r.ok) mediaState = r.data || {};
            return r;
          });
        }

        function esc(s) { return String(s == null ? "" : s).replace(/[&<>\"]/g, function (c) { return { "&": "&amp;", "<": "&lt;", ">": "&gt;", "\"": "&quot;" }[c]; }); }
        function renderPlaybackTasks() {
          var cur = (nodeSnapshot && nodeSnapshot.currentPlayback) || null;
          var playing = cur && cur.status === "playing";
          $("playback-current-status").textContent = playing ? "播放中" : "暂无播放";
          $("playback-current-detail").textContent = playing
            ? ((cur.name || cur.taskId || "当前任务") + " · " + (cur.taskId || ""))
            : "当前节点没有正在播放的任务";
          $("playback-current").setAttribute("data-task-id", (cur && cur.taskId) || "");
          var hint = '<div class="task-empty">完整任务列表由平台服务管理，节点本机页仅投影当前播放。</div>';
          $("playback-task-list").innerHTML = hint;
          $("playback-record-list").innerHTML = hint;
        }
        function taskAction() { protoToast("任务操作请前往平台服务端执行"); }
        function $(id) { return document.getElementById(id); }
        function roleText(role) { return role === "input" ? "输入盒" : "输出盒"; }
        function nodeExceptionText(node) {
          return node && node.exception && node.exception.reason ? node.exception.reason : "—";
        }
        function runtimeState(node) {
          if (node && node.runtimeState) return node.runtimeState;
          if (node && node.online === false) return "offline";
          if (node && node.registerState === "registering") return "registering";
          if (node && node.registerState === "failed") return "register_failed";
          return "online";
        }
        var RUNTIME_STATES = { online: "在线", offline: "离线", registering: "注册中", register_failed: "注册失败", network_reconfiguring: "网络切换中", rebooting: "重启中", factory_resetting: "恢复出厂中", upgrading: "升级中", upgrade_failed: "升级失败" };
        function runtimeStateText(state) {
          return RUNTIME_STATES[state] || state || "在线";
        }
        function runtimeStateClass(state) {
          if (state === "online") return "state-online";
          if (state === "rebooting" || state === "factory_resetting" || state === "upgrading" || state === "network_reconfiguring" || state === "registering") return "state-working";
          return "state-danger";
        }
        function portCapabilityText(port) {
          if (!port) return "未读取端口能力";
          return port.kind + " · 上限 " + port.maxWidth + "×" + port.maxHeight + " @" + port.maxRefreshHz + "Hz";
        }
        function findSafeMode(output) {
          if (!output) return null;
          return nodeCapabilities.safeModes.filter(function (mode) {
            return mode.width === Number(output.width) && mode.height === Number(output.height) && mode.refreshHz === Number(output.refreshHz);
          })[0] || null;
        }
        function renderOutputModes(preferredValue) {
          var outputSelect = $("sel-outputport");
          var resSelect = $("sel-res");
          var outputId = outputSelect.value || (nodeCapabilities.selectedOutput && nodeCapabilities.selectedOutput.id);
          var selectedPort = nodeCapabilities.outputPorts.filter(function (port) { return port.id === outputId; })[0] || nodeCapabilities.outputPorts[0];
          if (!selectedPort) return;
          nodeCapabilities.selectedOutput = selectedPort;
          var limit = {
            maxWidth: Number(selectedPort.maxWidth) || nodeCapabilities.customRange.maxWidth,
            maxHeight: Number(selectedPort.maxHeight) || nodeCapabilities.customRange.maxHeight,
            maxRefreshHz: Number(selectedPort.maxRefreshHz) || nodeCapabilities.customRange.maxRefreshHz
          };
          nodeCapabilities.customRange = {
            minWidth: nodeCapabilities.profile.customRange.minWidth,
            maxWidth: limit.maxWidth,
            minHeight: nodeCapabilities.profile.customRange.minHeight,
            maxHeight: limit.maxHeight,
            minRefreshHz: nodeCapabilities.profile.customRange.minRefreshHz,
            maxRefreshHz: limit.maxRefreshHz
          };
          var modes = nodeCapabilities.profile.safeModes.filter(function (mode) {
            return mode.width <= limit.maxWidth && mode.height <= limit.maxHeight && mode.refreshHz <= limit.maxRefreshHz;
          });
          nodeCapabilities.safeModes = modes;
          resSelect.innerHTML = modes.map(function (mode) { return '<option value="' + mode.id + '">' + mode.label + '</option>'; }).join("") + '<option value="custom">自定义（' + nodeCapabilities.profile.customRange.minWidth + '~' + limit.maxWidth + ' × ' + nodeCapabilities.profile.customRange.minHeight + '~' + limit.maxHeight + '，' + nodeCapabilities.profile.customRange.minRefreshHz + '~' + limit.maxRefreshHz + 'Hz）</option>';
          var output = nodeSnapshot.output || {};
          var safe = findSafeMode(output);
          resSelect.value = preferredValue || (safe ? safe.id : "custom");
          if (!resSelect.value) resSelect.value = modes.length ? modes[0].id : "custom";
          $("custom-res").style.display = resSelect.value === "custom" ? "" : "none";
          $("res-w").placeholder = nodeCapabilities.profile.customRange.minWidth + "~" + limit.maxWidth;
          $("res-h").placeholder = nodeCapabilities.profile.customRange.minHeight + "~" + limit.maxHeight;
          $("res-hz").placeholder = nodeCapabilities.profile.customRange.minRefreshHz + "~" + limit.maxRefreshHz;
          if (resSelect.value === "custom") {
            $("res-w").value = output.width || "";
            $("res-h").value = output.height || "";
            $("res-hz").value = output.refreshHz || "";
          }
          $("output-capability-note").textContent = portCapabilityText(selectedPort) + " · 以节点回读能力为准";
        }
        function renderHardwareCapabilities() {
          nodeCapabilities = DvsHardwareCapabilities.resolve(nodeSnapshot);
          $("sel-inputport").innerHTML = nodeCapabilities.inputPorts.map(function (port) {
            return '<option value="' + port.id + '">' + port.label + " · " + port.kind + " · 上限 " + port.maxWidth + "×" + port.maxHeight + " @" + port.maxRefreshHz + "Hz</option>";
          }).join("");
          $("sel-outputport").innerHTML = nodeCapabilities.outputPorts.map(function (port) {
            return '<option value="' + port.id + '">' + port.label + " · " + port.kind + "</option>";
          }).join("");
          $("sel-inputport").value = nodeSnapshot.input && nodeSnapshot.input.port && nodeCapabilities.inputPorts.some(function (port) { return port.id === nodeSnapshot.input.port; })
            ? nodeSnapshot.input.port : (nodeCapabilities.inputPorts[0] && nodeCapabilities.inputPorts[0].id);
          $("sel-outputport").value = nodeSnapshot.output && nodeSnapshot.output.connector && nodeCapabilities.outputPorts.some(function (port) { return port.id === nodeSnapshot.output.connector; })
            ? nodeSnapshot.output.connector : (nodeCapabilities.outputPorts[0] && nodeCapabilities.outputPorts[0].id);
          renderOutputModes();
          $("info-capability").textContent = nodeCapabilities.profile.name + "（官方芯片基线）";
          $("info-sync").textContent = nodeCapabilities.sync;
        }
        function playbackText(node) {
          return node && node.currentPlayback && node.currentPlayback.status === "playing"
            ? node.currentPlayback.name + " · " + node.currentPlayback.taskId : "未播放";
        }
        function mergeNode(patch) {
          patch = patch || {};
          Object.keys(patch).forEach(function (k) {
            if (patch[k] && typeof patch[k] === "object" && !Array.isArray(patch[k]) && nodeSnapshot[k] && typeof nodeSnapshot[k] === "object") {
              var merged = Object.assign({}, nodeSnapshot[k]);
              Object.keys(patch[k]).forEach(function (kk) { merged[kk] = patch[k][kk]; });
              nodeSnapshot[k] = merged;
            } else {
              nodeSnapshot[k] = patch[k];
            }
          });
          return nodeSnapshot;
        }
        function patchNode(patch) { return mergeNode(patch); }
        function patchNodeConfig(key, state, detail) {
          var states = {}, messages = {};
          states[key] = state;
          messages[key] = detail;
          patchNode({ configStates: states, configMessages: messages });
        }
        function refreshConfigStates() {
          var cs = (nodeSnapshot && nodeSnapshot.configStates) || {};
          var map = { "role-config-state": "role", "input-config-state": "input", "output-config-state": "output", "network-config-state": "network", "registration-config-state": "registration", "sip-config-state": "sip" };
          Object.keys(map).forEach(function (id) {
            var scope = map[id];
            var st = cs[scope];
            if (!st) return;
            setConfigState(id, backendStateToUi(st), st);
          });
        }
        function syncNodeView() {
          nodeSnapshot = nodeSnapshot || {};
          currentRole = nodeSnapshot.role || currentRole || "output";
          pendingRole = (nodeSnapshot.pendingRole === undefined) ? pendingRole : nodeSnapshot.pendingRole;
          var state = runtimeState(nodeSnapshot);
          var stateText = runtimeStateText(state);
          $("ops-state-label").textContent = stateText;
          $("ops-state-label").className = "tag-state " + runtimeStateClass(state);
          $("ops-state-detail").textContent = nodeSnapshot.runtimeMessage || (nodeSnapshot.exception && nodeSnapshot.exception.reason) || "运行正常";
          $("network-risk").hidden = state !== "network_reconfiguring";
          $("node-login-context").textContent = (nodeSnapshot.name || "节点") + "（" + (nodeSnapshot.ip || "—") + "）· 本机登录，不依赖平台在线";
          $("info-model").textContent = nodeSnapshot.model || "—";
          $("info-board").textContent = nodeSnapshot.board || "—";
          $("info-sn").textContent = nodeSnapshot.registerId || nodeSnapshot.sn || "—";
          $("fw-ver").textContent = nodeSnapshot.fw || "—";
          $("info-role").textContent = pendingRole ? roleText(currentRole) + " → " + roleText(pendingRole) + "（待重启）" : roleText(currentRole);
          $("info-ip").textContent = nodeSnapshot.ip || "—";
          $("info-mac").textContent = nodeSnapshot.mac || "—";
          $("info-uptime").textContent = nodeSnapshot.uptime || "—";
          $("info-regmode").textContent = nodeSnapshot.registerMode === "manual" ? "手动注册" : "自动注册";
          $("info-sipstate").textContent = nodeSnapshot.sip && nodeSnapshot.sip.enabled
            ? (nodeSnapshot.sip.state === "connected" ? "已连接" : "连接失败") : "未启用";
          $("info-playback").textContent = playbackText(nodeSnapshot);
          $("info-exception").textContent = nodeExceptionText(nodeSnapshot);
          $("info-temp").textContent = nodeSnapshot.metrics && nodeSnapshot.metrics.temperature || "—";
          $("info-mem").textContent = nodeSnapshot.metrics && nodeSnapshot.metrics.memory || "—";
          if (nodeSnapshot.output) {
            var output = nodeSnapshot.output;
            var outputPortId = output.connector && nodeCapabilities.outputPorts.some(function (port) { return port.id === output.connector; })
              ? output.connector : (nodeCapabilities.outputPorts[0] && nodeCapabilities.outputPorts[0].id);
            $("sel-outputport").value = outputPortId;
            if (output.connector !== outputPortId) renderOutputModes();
            var safe = findSafeMode(output);
            $("sel-res").value = safe ? safe.id : "custom";
            $("custom-res").style.display = $("sel-res").value === "custom" ? "" : "none";
            if ($("sel-res").value === "custom") {
              $("res-w").value = output.width || "";
              $("res-h").value = output.height || "";
              $("res-hz").value = output.refreshHz || "";
            }
            var outMode = document.querySelector('input[name="outmode"][value="' + (output.mode === "hotplug" ? "hotplug" : "force") + '"]');
            if (outMode) outMode.checked = true;
          }
          if (nodeSnapshot.input && nodeSnapshot.input.port) $("sel-inputport").value = nodeSnapshot.input.port;
          var regLabel = nodeSnapshot.registerState === "registered" ? "已注册 · " + roleText(nodeSnapshot.role)
            : nodeSnapshot.registerState === "registering" ? "注册中…" : "连接失败 · 请检查地址";
          setRegState(regLabel, nodeSnapshot.registerState === "registered");
          if ($("reg-state")) $("reg-state").title = nodeSnapshot.registerMessage || "";
        }
        function openModal(id) { $(id).hidden = false; $(id).classList.add("is-open"); }
        function closeModal(id) { $(id).classList.remove("is-open"); $(id).hidden = true; }
        function now() { var d = new Date(); return d.toLocaleString(); }
        function setLastOps(type, result) {
          $("last-ops").textContent = "最近动作：" + type + " · " + now() + " · " + result;
        }
        function setOpsRunning(flag, label) {
          opsRunning = flag;
          ["btn-reboot", "btn-factory", "btn-pick-fw", "btn-upgrade"].forEach(function (id) {
            if (id === "btn-upgrade" && fwReady && !flag) return;
            if (id === "btn-upgrade") { $(id).disabled = !fwReady || flag; return; }
            $(id).disabled = flag;
          });
          if (label) protoToast(label);
        }
        function setConfigState(id, state, detail) {
          var el = $(id), detailEl = $(id + "-detail");
          if (!el) return;
          var labels = { idle: "未保存", pending: "待生效", applying: "应用中", applied: "已生效", failed: "失败" };
          el.textContent = labels[state] || state;
          el.className = "tag-state state-" + state;
          if (detailEl && detail !== undefined) detailEl.textContent = detail;
        }
        function backendStateToUi(state) {
          if (state === "pending_apply") return "pending";
          if (state === "applied") return "applied";
          if (state === "failed") return "failed";
          return "idle";
        }
        function pollConfig(id, scope, token, opts) {
          var attempts = 0;
          function tick() {
            if (configJobs[id] !== token) return;
            attempts++;
            apiAsync("GET", "/api/v1/node/config/" + encodeURIComponent(scope)).then(function (r) {
              if (configJobs[id] !== token) return;
              if (!r || !r.ok) {
                setConfigState(id, "failed", (r && r.msg) || "读取配置状态失败");
                if (opts.button) opts.button.disabled = false;
                return;
              }
              var cfg = r.data || {};
              var ui = backendStateToUi(cfg.state);
              var detail = cfg.errorReason || cfg.state;
              if (ui === "pending") {
                setConfigState(id, "applying", opts.applyingText || "正在应用配置");
                if (opts.onApplying && attempts === 1) opts.onApplying();
              } else {
                setConfigState(id, ui, ui === "applied" ? (opts.appliedText || "配置已生效") : (cfg.errorReason || opts.failedText || "应用失败"));
              }
              // update nodeSnapshot configStates
              if (!nodeSnapshot.configStates) nodeSnapshot.configStates = {};
              nodeSnapshot.configStates[scope] = cfg.state;
              if (ui === "applied") {
                if (opts.onApplied) opts.onApplied(cfg);
                if (opts.button) opts.button.disabled = false;
                return;
              }
              if (ui === "failed") {
                if (opts.onFail) opts.onFail(cfg);
                if (opts.button) opts.button.disabled = false;
                return;
              }
              if (attempts < 20) {
                window.setTimeout(tick, 500);
              } else {
                setConfigState(id, "failed", "等待应用超时");
                if (opts.button) opts.button.disabled = false;
              }
            });
          }
          window.setTimeout(tick, 400);
        }
        function applyConfig(id, opts) {
          opts = opts || {};
          var scope = opts.scope;
          var token = (configJobs[id] || 0) + 1;
          configJobs[id] = token;
          if (opts.button) opts.button.disabled = true;
          setConfigState(id, "pending", opts.pendingText || "配置已保存，等待节点应用");
          if (opts.onPending) opts.onPending();
          apiAsync("POST", "/api/v1/node/config/" + encodeURIComponent(scope), {
            desiredValue: opts.desiredValue || {},
            operationId: opts.operationId || ("op-" + Date.now())
          }).then(function (r) {
            if (configJobs[id] !== token) return;
            if (!r || !r.ok) {
              setConfigState(id, "failed", (r && r.msg) || "提交失败");
              if (opts.onFail) opts.onFail(r);
              if (opts.button) opts.button.disabled = false;
              return;
            }
            var cfg = r.data || {};
            if (!nodeSnapshot.configStates) nodeSnapshot.configStates = {};
            nodeSnapshot.configStates[scope] = cfg.state || "pending_apply";
            pollConfig(id, scope, token, opts);
          });
        }

        /* ---------- 分组菜单 / 右侧功能详情切换 ---------- */
        function navItem(z) { return document.querySelector('.proto-side-sub-children a[data-zone="' + z + '"]'); }
        function showZone(id) {
          if (id === "playback" && currentRole !== "output") protoToast("播放任务仅输出盒适用");
          var pageTitle = {
            role: "节点角色",
            input: "输入口",
            output: "输出设置",
            net: "网络与注册",
            playback: "播放任务",
            info: "本机信息",
            ops: "维护操作"
          }[id] || "节点角色";
          document.querySelectorAll(".proto-side-sub-children a[data-zone]").forEach(function (a) {
            a.classList.toggle("is-active", a.getAttribute("data-zone") === id);
          });
          var activeItem = navItem(id);
          document.querySelectorAll(".proto-side-sub-group[data-nav-group]").forEach(function (group) {
            group.classList.toggle("is-active", group.contains(activeItem));
          });
          document.querySelectorAll(".proto-content .zone").forEach(function (s) {
            s.classList.toggle("is-active", s.id === "zone-" + id);
          });
          $("node-breadcrumb-zone").textContent = pageTitle;
        }

        /* ---------- 登录 ---------- */
        $("btn-login").onclick = function () {
          var u = $("login-user").value.trim(), p = $("login-pwd").value;
          $("btn-login").disabled = true;
          apiAsync("POST", "/api/v1/node/login", { username: u, password: p }, { idempotencyKey: "login-" + Date.now() }).then(function (result) {
            $("btn-login").disabled = false;
            if (!result || !result.ok) {
              $("field-pwd").classList.add("is-error");
              $("login-err").style.display = "block";
              $("login-err").textContent = (result && result.msg) ? result.msg : "账号或密码错误";
              return;
            }
            $("field-pwd").classList.remove("is-error");
            $("login-err").style.display = "none";
            $("login-mask").style.display = "none";
            var req = result.data && result.data.requirement;
            if (req && req.passwordChangeRequired) {
              protoToast("首次登录请修改密码");
              openModal("pwd-modal");
            } else {
              protoToast("登录成功");
            }
          });
        };
        $("login-pwd").addEventListener("keydown", function (e) { if (e.key === "Enter") $("btn-login").click(); });
        $("btn-logout").onclick = function () {
          apiAsync("POST", "/api/v1/node/logout", {}).then(function () {
            $("login-mask").style.display = "flex";
            $("login-pwd").value = "";
            protoToast("已退出登录");
          });
        };

        /* ---------- 修改密码 ---------- */
        $("btn-changepwd").onclick = function () { $("pwd-err").style.display = "none"; openModal("pwd-modal"); };
        $("pwd-cancel").onclick = function () { closeModal("pwd-modal"); };
        $("pwd-ok").onclick = function () {
          var o = $("pwd-old").value, n = $("pwd-new").value, n2 = $("pwd-new2").value;
          if (!o || !n) { $("pwd-err").style.display = "block"; $("pwd-err").textContent = "请填写完整"; return; }
          if (n !== n2) { $("pwd-err").style.display = "block"; $("pwd-err").textContent = "两次输入的新密码不一致"; return; }
          apiAsync("POST", "/api/v1/node/password", { oldPassword: o, newPassword: n }).then(function (r) {
            if (!r || !r.ok) { $("pwd-err").style.display = "block"; $("pwd-err").textContent = (r && r.msg) || "修改失败"; return; }
            closeModal("pwd-modal");
            protoToast("密码修改成功");
            setLastOps("修改密码", "成功");
          });
        };

        /* ---------- 角色区 ---------- */
        function renderRole() {
          var shown = pendingRole || currentRole;
          /* 角色专属区：清单项置灰 + 进入后给不适用提示（待生效切换时两区均显示） */
          $("na-input").hidden = shown === "input" || !!pendingRole;
          $("na-output").hidden = shown === "output" || !!pendingRole;
          navItem("input").classList.toggle("is-muted", !(shown === "input" || pendingRole));
          navItem("output").classList.toggle("is-muted", !(shown === "output" || pendingRole));
          $("nav-dot-role").hidden = !pendingRole;
          $("tag-pending").hidden = !pendingRole;
          $("tag-input-pending").hidden = !(pendingRole === "input");
          $("tag-output-pending").hidden = !(pendingRole === "output");
          $("btn-undo-role").hidden = !pendingRole;
          var radio = document.querySelector('input[name="role"][value="' + (pendingRole || currentRole) + '"]');
          if (radio) radio.checked = true;
          var roleText = { input: "输入盒", output: "输出盒" };
          $("info-role").textContent = pendingRole
            ? roleText[currentRole] + " → " + roleText[pendingRole] + "（待重启）"
            : roleText[currentRole];
          var inputSave = document.querySelector('.btn-save-zone[data-zone="input"]');
          var outputSave = document.querySelector('.btn-save-zone[data-zone="output"]');
          var playbackNav = navItem("playback");
          var playbackAvailable = currentRole === "output";
          if (inputSave) inputSave.disabled = !(shown === "input" || pendingRole);
          if (outputSave) outputSave.disabled = !(shown === "output" || pendingRole);
          if (playbackNav) {
            playbackNav.classList.toggle("is-muted", !playbackAvailable);
            playbackNav.setAttribute("aria-disabled", playbackAvailable ? "false" : "true");
          }
          $("na-playback").hidden = playbackAvailable;
          $("playback-body").hidden = !playbackAvailable;
          if (pendingRole) {
            setConfigState("role-config-state", "pending", "已保存切换意图，重启后应用：" + roleText[pendingRole]);
          } else {
            setConfigState("role-config-state", "applied", "当前角色：" + roleText[currentRole]);
          }
        }
        document.querySelectorAll('input[name="role"]').forEach(function (r) {
          r.addEventListener("change", function () {
            var target = r.value;
            if (target === currentRole && !pendingRole) return;
            $("role-target-name").textContent = target === "input" ? "输入盒" : "输出盒";
            openModal("role-modal");
          });
        });
        $("role-cancel").onclick = function () { closeModal("role-modal"); renderRole(); };
        $("role-ok").onclick = function () {
          var target = document.querySelector('input[name="role"]:checked').value;
          apiAsync("POST", "/api/v1/node/config/role", { desiredValue: { role: target }, operationId: "role-" + Date.now() }).then(function (r) {
            if (!r || !r.ok) { protoToast((r && r.msg) || "角色切换配置失败"); return; }
            pendingRole = target;
            patchNode({ pendingRole: pendingRole, configStates: { role: "pending_apply" }, configMessages: { role: "已保存切换意图，等待重启应用" } });
            closeModal("role-modal");
            renderRole();
            protoToast("配置已保存，等待重启应用");
            setLastOps("角色切换配置", "待生效");
          });
        };
        $("btn-undo-role").onclick = function () {
          apiAsync("POST", "/api/v1/node/config/role", { desiredValue: { role: currentRole }, operationId: "role-undo-" + Date.now() }).then(function () {
            pendingRole = null;
            patchNode({ pendingRole: null, configStates: { role: "applied" }, configMessages: { role: "已撤销切换，当前角色：" + roleText(currentRole) } });
            renderRole();
            protoToast("已撤销切换，节点保持原角色");
            setLastOps("角色切换撤销", "已取消");
          });
        };

        /* ---------- 输入口 / 输出区 ---------- */
        $("sel-outputport").addEventListener("change", function () {
          renderOutputModes();
        });
        $("sel-res").addEventListener("change", function () {
          $("custom-res").style.display = this.value === "custom" ? "" : "none";
        });
        document.querySelectorAll(".btn-save-zone").forEach(function (btn) {
          btn.addEventListener("click", function () {
            var zone = btn.getAttribute("data-zone");
            if (zone === "input") {
              var inputPort = $("sel-inputport").value;
              applyConfig("input-config-state", {
                scope: "input",
                desiredValue: { port: inputPort },
                button: btn,
                pendingText: "配置已保存，等待节点应用",
                applyingText: "正在应用采集口配置",
                appliedText: "已生效 · 当前采集口：" + inputPort,
                onPending: function () { patchNodeConfig("input", "pending", "配置已保存，等待节点应用"); },
                onApplying: function () { patchNodeConfig("input", "applying", "正在应用采集口配置"); },
                onApplied: function () {
                  patchNode({ input: { port: inputPort }, configStates: { input: "applied" }, configMessages: { input: "已生效 · 当前采集口：" + inputPort }, exception: null });
                  syncNodeView();
                  protoToast("输入口配置已生效：" + inputPort);
                  setLastOps("输入口配置", "已生效");
                }
              });
            } else {
              if ($("sel-res").value === "custom") {
                var w = Number($("res-w").value), h = Number($("res-h").value), hz = Number($("res-hz").value);
                var range = nodeCapabilities.customRange;
                if (!(w >= range.minWidth && w <= range.maxWidth) || !(h >= range.minHeight && h <= range.maxHeight) || !(hz >= range.minRefreshHz && hz <= range.maxRefreshHz)) {
                  setConfigState("output-config-state", "failed", "参数超出本机能力范围");
                  protoToast("超出当前输出口能力（宽 " + range.minWidth + "~" + range.maxWidth + " / 高 " + range.minHeight + "~" + range.maxHeight + " / 刷新率 " + range.minRefreshHz + "~" + range.maxRefreshHz + "Hz）");
                  return;
                }
                var customLabel = w + "×" + h + " @" + hz + "Hz";
              } else {
                var customLabel = $("sel-res").value;
              }
              var outputSelection = $("sel-res").value;
              var selectedMode = nodeCapabilities.profile.safeModes.filter(function (mode) { return mode.id === outputSelection; })[0];
              var selectedOutput = outputSelection === "custom"
                ? { width: w, height: h, refreshHz: hz }
                : selectedMode || nodeCapabilities.safeModes[0] || { width: 1920, height: 1080, refreshHz: 60 };
              var outMode = document.querySelector('input[name="outmode"]:checked').value === "force" ? "强推" : "热插拔";
              applyConfig("output-config-state", {
                scope: "output",
                desiredValue: (function () {
                  var outputMode = (document.querySelector('input[name="outmode"]:checked').value === "force") ? "force" : "hotplug";
                  return { width: selectedOutput.width, height: selectedOutput.height, refreshHz: selectedOutput.refreshHz, mode: outputMode, connector: $("sel-outputport").value };
                })(),
                button: btn,
                pendingText: "配置已保存，等待输出重新协商",
                applyingText: "应用中 · 输出重新协商",
                appliedText: "已生效 · " + customLabel + " · " + outMode,
                onPending: function () { patchNodeConfig("output", "pending", "配置已保存，等待输出重新协商"); },
                onApplying: function () { patchNodeConfig("output", "applying", "正在应用输出配置"); },
                onApplied: function () {
                  var outputMode = outMode === "强推" ? "force" : "hotplug";
                  var outputPatch = { width: selectedOutput.width, height: selectedOutput.height, refreshHz: selectedOutput.refreshHz };
                  outputPatch.mode = outputMode;
                  outputPatch.connector = $("sel-outputport").value;
                  patchNode({ output: outputPatch, configStates: { output: "applied" }, configMessages: { output: "已生效 · " + customLabel + " · " + outMode }, exception: null });
                  syncNodeView();
                  protoToast("输出配置已生效，重新协商完成");
                  setLastOps("输出配置", "已生效");
                }
              });
            }
          });
        });

        /* ---------- 网络与注册 ---------- */
        document.querySelectorAll('input[name="netmode"]').forEach(function (r) {
          r.addEventListener("change", function () {
            $("static-fields").style.display = this.value === "static" && this.checked ? "" : "none";
          });
        });
        function ipOk(v) { return /^(\d{1,3}\.){3}\d{1,3}$/.test(v) && v.split(".").every(function (x) { return Number(x) <= 255; }); }
        $("btn-save-net").onclick = function () {
          var mode = document.querySelector('input[name="netmode"]:checked').value;
          if (mode === "static") {
            var fields = [["IP", "in-ip"], ["掩码", "in-mask"], ["网关", "in-gw"], ["DNS", "in-dns"]];
            for (var i = 0; i < fields.length; i++) {
              if (!ipOk($(fields[i][1]).value)) {
                $("net-err").style.display = "block";
                $("net-err").textContent = fields[i][0] + " 格式不正确";
                setConfigState("network-config-state", "failed", fields[i][0] + " 格式不正确，未保存");
                return;
              }
            }
            $("net-err").style.display = "none";
            var newIp = $("in-ip").value;
            protoConfirm({
              title: "静态 IP 生效确认",
              content: "生效后请以新 IP 访问本机页（新地址：" + newIp + "）。确认保存？",
              onOk: function () {
                applyConfig("network-config-state", {
                  scope: "network",
                  desiredValue: { mode: "static", ip: newIp, mask: $("in-mask").value, gateway: $("in-gw").value, dns: $("in-dns").value, server: $("in-server").value },
                  button: $("btn-save-net"),
                  pendingText: "配置已保存，等待网络应用",
                  applyingText: "应用中 · 正在切换网络地址",
                  appliedText: "已生效 · 请使用新 IP 访问：" + newIp,
                  failedText: "IP 冲突，已回滚原配置",
                  onPending: function () { patchNodeConfig("network", "pending", "配置已保存，等待网络应用"); },
                  onApplying: function () {
                    patchNodeConfig("network", "applying", "正在切换网络地址");
                    patchNode({ online: false, runtimeState: "network_reconfiguring", runtimeMessage: "网络参数应用中，节点会短暂断连", exception: { code: "NETWORK_RECONFIGURING", reason: "网络改动应用中，节点短暂断连", at: new Date().toISOString() } });
                    syncNodeView();
                  },
                  onFail: function () {
                    patchNode({ online: true, runtimeState: "online", runtimeMessage: "网络配置失败，已回滚原配置", configStates: { network: "failed" }, configMessages: { network: "IP 冲突，已回滚原配置" }, exception: { code: "IP_CONFLICT", reason: "IP 冲突，已保持原配置", at: new Date().toISOString() } });
                    syncNodeView();
                    protoToast("IP 冲突，已保持原配置");
                    setLastOps("网络配置", "失败（回滚）");
                  },
                  onApplied: function () {
                    patchNode({ ip: newIp, online: true, runtimeState: "online", runtimeMessage: "网络配置已生效", network: { mode: "static", mask: $("in-mask").value, gateway: $("in-gw").value, dns: $("in-dns").value, server: $("in-server").value }, configStates: { network: "applied" }, configMessages: { network: "已生效 · 请使用新 IP 访问：" + newIp }, exception: null });
                    syncNodeView();
                    protoToast("网络配置已生效（" + newIp + "）");
                    setLastOps("网络配置", "已生效");
                  }
                });
              }
            });
            return;
          }
          protoConfirm({
            title: "切换 DHCP 确认",
            content: "应用 DHCP 会让节点短暂断连并重新获取地址。确认保存网络配置？",
            onOk: function () {
              applyConfig("network-config-state", {
                scope: "network",
                desiredValue: { mode: "dhcp", server: $("in-server").value },
                button: $("btn-save-net"),
                pendingText: "配置已保存，等待网络应用",
                applyingText: "应用中 · 正在获取 DHCP 地址",
                appliedText: "已生效 · 当前方式：DHCP",
                onPending: function () { patchNodeConfig("network", "pending", "配置已保存，等待网络应用"); },
                onApplying: function () {
                  patchNodeConfig("network", "applying", "正在获取 DHCP 地址");
                  patchNode({ online: false, runtimeState: "network_reconfiguring", runtimeMessage: "DHCP 应用中，节点会短暂断连", exception: { code: "NETWORK_RECONFIGURING", reason: "DHCP 应用中，节点短暂断连", at: new Date().toISOString() } });
                  syncNodeView();
                },
                onApplied: function () {
                  patchNode({ ip: nodeSnapshot.ip || "192.168.10.51", online: true, runtimeState: "online", runtimeMessage: "网络配置已生效", network: { mode: "dhcp", server: $("in-server").value }, configStates: { network: "applied" }, configMessages: { network: "已生效 · 当前方式：DHCP" }, exception: null });
                  syncNodeView();
                  protoToast("网络配置已生效（DHCP）");
                  setLastOps("网络配置", "已生效");
                }
              });
            }
          });
        };

        /* ---------- 设备注册 ---------- */
        function regMode() { return document.querySelector('input[name="regmode"]:checked').value; }
        function setRegState(text, ok) {
          $("reg-state").textContent = text;
          $("reg-state").className = ok ? "tag-ok" : "tag-pending";
        }
        function startRegistration() {
          if (registrationBusy) return;
          if (!$("in-server").value.trim()) {
            setConfigState("registration-config-state", "failed", "缺少注册服务器地址");
            protoToast("请先填写注册服务器地址");
            return;
          }
          registrationBusy = true;
          $("btn-register").disabled = true;
          setRegState("注册中…", false);
          setConfigState("registration-config-state", "applying", "正在提交注册请求");
          apiAsync("POST", "/api/v1/node/register", {}).then(function () { return loadNodeInfo(); }).then(function (r) {
            registrationBusy = false;
            $("btn-register").disabled = false;
            if (!r || !r.ok) {
              setRegState("连接失败 · 请检查地址", false);
              setConfigState("registration-config-state", "failed", "注册状态读取失败，可重试");
              syncNodeView();
              return;
            }
            syncNodeView();
            renderRole();
            var st = nodeSnapshot.registerState;
            if (st === "registered") {
              setRegState("已注册 · " + roleText(currentRole), true);
              setConfigState("registration-config-state", "applied", "已生效 · 设备已注册");
              protoToast("设备注册成功");
              setLastOps("设备注册", "已生效");
            } else if (st === "failed") {
              setRegState("连接失败 · 请检查地址", false);
              setConfigState("registration-config-state", "failed", "注册失败，可修改地址后重试");
              protoToast("注册失败，本机播放不受影响");
              setLastOps("设备注册", "失败");
            } else {
              setRegState("注册中…", false);
              setConfigState("registration-config-state", "applying", "注册请求已提交，等待平台回执");
              protoToast("注册请求已提交");
              setLastOps("设备注册", "已提交");
            }
          });
        }
        $("btn-save-registration").onclick = function () {
          $("info-regmode").textContent = regMode() === "auto" ? "自动注册" : "手动注册";
          if (!$("in-server").value.trim()) {
            setConfigState("registration-config-state", "failed", "缺少注册服务器地址，未保存");
            protoToast("请先填写注册服务器地址");
            return;
          }
          applyConfig("registration-config-state", {
            scope: "registration",
            desiredValue: { mode: regMode(), server: $("in-server").value },
            button: $("btn-save-registration"),
            pendingText: "配置已保存，等待注册应用",
            applyingText: "应用中 · 正在提交注册配置",
            appliedText: "已生效 · " + (regMode() === "auto" ? "自动注册" : "手动注册"),
            onPending: function () { patchNodeConfig("registration", "pending", "配置已保存，等待注册应用"); },
            onApplying: function () { patchNodeConfig("registration", "applying", "正在提交注册配置"); },
            onApplied: function () {
              patchNode({ registerMode: regMode(), network: { server: $("in-server").value }, configStates: { registration: "applied" }, configMessages: { registration: "已生效 · " + (regMode() === "auto" ? "自动注册" : "手动注册") } });
              syncNodeView();
              if (regMode() === "auto") {
                startRegistration();
              } else {
                protoToast("注册配置已生效，点击“立即注册”后执行");
                setLastOps("注册配置", "已生效");
              }
            }
          });
        };
        $("btn-register").onclick = function () {
          if (registrationBusy) return;
          setConfigState("registration-config-state", "pending", "已提交注册请求，等待节点应用");
          patchNodeConfig("registration", "pending", "已提交注册请求，等待节点应用");
          startRegistration();
        };

        /* ---------- SIP 接入 ---------- */
        $("sip-enabled").addEventListener("change", function () {
          $("sip-fields").hidden = !this.checked;
          setConfigState("sip-config-state", "pending", "有未保存改动");
        });
        $("btn-save-sip").onclick = function () {
          if (!$("sip-enabled").checked) {
            applyConfig("sip-config-state", {
              scope: "sip",
              desiredValue: { enabled: false },
              button: $("btn-save-sip"),
              pendingText: "配置已保存，等待关闭 SIP",
              applyingText: "应用中 · 正在关闭 SIP",
              appliedText: "已生效 · SIP 未启用",
              onPending: function () { patchNodeConfig("sip", "pending", "配置已保存，等待关闭 SIP"); },
              onApplying: function () { patchNodeConfig("sip", "applying", "正在关闭 SIP"); },
              onApplied: function () {
                $("sip-state").textContent = "未启用";
                $("sip-state").className = "tag-pending";
                $("info-sipstate").textContent = "未启用";
                patchNode({ sip: { enabled: false, state: "disabled" }, configStates: { sip: "applied" }, configMessages: { sip: "已生效 · SIP 未启用" }, exception: null });
                syncNodeView();
                protoToast("SIP 配置已生效：已关闭");
                setLastOps("SIP 配置", "已生效");
              }
            });
            return;
          }
          var server = $("sip-server").value.trim(), port = Number($("sip-port").value), device = $("sip-device").value.trim(), user = $("sip-user").value.trim();
          if (!server || !(port >= 1 && port <= 65535) || !device || !user || !$("sip-pwd").value) {
            setConfigState("sip-config-state", "failed", "参数不完整，未保存");
            protoToast("请填写完整 SIP 参数（端口 1~65535）");
            return;
          }
          $("sip-state").textContent = "连接中…";
          $("sip-state").className = "tag-pending";
          applyConfig("sip-config-state", {
            scope: "sip",
            desiredValue: { enabled: true, server: server, port: port, deviceId: device, user: user },
            button: $("btn-save-sip"),
            pendingText: "配置已保存，等待 SIP 应用",
            applyingText: "应用中 · 正在连接 SIP 服务",
            appliedText: "已生效 · SIP 已连接",
            failedText: "SIP 连接失败，可修改参数后重试",
            onPending: function () { patchNodeConfig("sip", "pending", "配置已保存，等待 SIP 应用"); },
            onApplying: function () { patchNodeConfig("sip", "applying", "正在连接 SIP 服务"); },
            onFail: function () {
              $("sip-state").textContent = "连接失败 · 可重试";
              $("sip-state").className = "tag-pending";
              $("info-sipstate").textContent = "连接失败";
              patchNode({ sip: { enabled: true, state: "failed", server: server, port: port, deviceId: device, user: user }, configStates: { sip: "failed" }, configMessages: { sip: "SIP 连接失败，可修改参数后重试" }, exception: { code: "SIP_UNREACHABLE", reason: "SIP 服务不可达", at: new Date().toISOString() } });
              syncNodeView();
              protoToast("SIP 服务不可达，本机播放不受影响");
              setLastOps("SIP 配置", "失败");
            },
            onApplied: function () {
              $("sip-state").textContent = "已连接";
              $("sip-state").className = "tag-ok";
              $("info-sipstate").textContent = "已连接";
              patchNode({ sip: { enabled: true, state: "connected", server: server, port: port, deviceId: device, user: user, lastAt: new Date().toISOString() }, configStates: { sip: "applied" }, configMessages: { sip: "已生效 · SIP 已连接" }, exception: null });
              syncNodeView();
              protoToast("SIP 配置已生效");
              setLastOps("SIP 配置", "已生效");
            }
          });
        };

        /* ---------- 运维 ---------- */
        $("btn-reboot").onclick = function () {
          protoConfirm({
            title: "重启确认",
            content: "重启期间画面中断约 1 分钟。确认重启？" + (pendingRole ? "（本次重启将使角色切换生效）" : ""),
            onOk: function () {
              setOpsRunning(true, "正在重启…");
              apiAsync("POST", "/api/v1/node/reboot").then(function (r) {
                protoToast("重启指令已下发");
                setLastOps("重启", "已提交");
                setTimeout(function () { window.location.reload(); }, 3000);
              }).catch(function () {
                setOpsRunning(false);
                protoToast("重启请求失败");
              });
            }
          });
        };
        $("btn-factory").onclick = function () {
          if (opsRunning) return;
          openModal("factory-modal");
        };
        $("factory-cancel").onclick = function () { closeModal("factory-modal"); };
        $("factory-ok").onclick = function () {
          closeModal("factory-modal");
          protoConfirm({
            title: "再次确认恢复出厂",
            content: "该操作会重置角色、输入口、输出参数和密码，节点将重启并暂时中断画面；网络设置与注册地址保留。仍要继续？",
            onOk: function () {
              setOpsRunning(true, "正在恢复出厂…");
              apiAsync("POST", "/api/v1/node/factory-reset").then(function (r) {
                protoToast("恢复出厂指令已下发");
                setLastOps("恢复出厂", "已提交");
                setTimeout(function () { window.location.reload(); }, 3000);
              }).catch(function () {
                setOpsRunning(false);
                protoToast("恢复出厂请求失败");
              });
            }
          });
        };
        $("btn-pick-fw").onclick = function () { $("fw-input").click(); };
        $("fw-input").addEventListener("change", function () {
          var f = this.files && this.files[0];
          if (!f) return;
          if (!/\.bin$/.test(f.name) || f.name.indexOf("V1.0.1") < 0) {
            fwReady = false;
            $("btn-upgrade").disabled = true;
            protoToast(/\.bin$/.test(f.name) ? "版本不符：固件包版本不匹配本机" : "文件损坏：固件包格式不正确");
            setLastOps("固件校验", "失败");
            return;
          }
          fwReady = true;
          pendingFwFile = f;
          pendingFwSha = "";
          $("btn-upgrade").disabled = false;
          if (window.crypto && window.crypto.subtle) {
            f.arrayBuffer().then(function (buf) {
              return window.crypto.subtle.digest("SHA-256", buf);
            }).then(function (hash) {
              pendingFwSha = Array.from(new Uint8Array(hash)).map(function (b) { return b.toString(16).padStart(2, "0"); }).join("");
            }).catch(function () { pendingFwSha = ""; });
          }
          protoToast("固件包校验通过（" + f.name + "），可开始升级");
          setLastOps("固件校验", "通过");
          this.value = "";
        });
        $("btn-upgrade").onclick = function () {
          if (!fwReady || opsRunning) return;
          protoConfirm({
            title: "开始升级确认",
            content: "升级期间节点会暂时离线，严禁断电或关闭页面。失败时保留当前旧版本并允许重试。确认开始升级？",
            onOk: function () {
              setOpsRunning(true, "升级开始");
              $("fw-prog").hidden = false;
              $("fw-status").hidden = false;
              $("fw-status").textContent = "正在上传固件包，请勿关闭页面…";
              var file = pendingFwFile;
              /* UPG-1:先上传二进制拿到暂存 path,再用该 path 调升级接口;后端对裸文件名也会回退到 /tmp 查找。 */
              function finishUpgrade(stagedPath, sha) {
                $("fw-status").textContent = "升级中，请勿关闭页面…";
                return apiAsync("POST", "/api/v1/node/upgrade", { filename: stagedPath, sha256: sha }).then(function (r) {
                  setOpsRunning(false);
                  var inner = r && r.data ? r.data : null;
                  var st = inner && inner.status ? inner.status : "";
                  if (!r || !r.ok || !inner || inner.ok === false) {
                    $("fw-status").textContent = "升级失败：节点已回到旧版本，可重新选择固件包重试。";
                    protoToast("升级失败");
                    setLastOps("固件升级", "失败");
                    return;
                  }
                  $("fw-status").hidden = true;
                  fwReady = false;
                  pendingFwFile = null;
                  pendingFwSha = "";
                  $("btn-upgrade").disabled = true;
                  if (st === "staged") {
                    protoToast("固件已暂存，未切换（需服务端开门）");
                    setLastOps("固件升级", "已暂存待切换");
                  } else {
                    protoToast("升级完成");
                    setLastOps("固件升级", "成功");
                  }
                  setTimeout(function () { window.location.reload(); }, 1500);
                });
              }
              if (!file) {
                finishUpgrade("fw.bin", pendingFwSha).catch(function () {
                  setOpsRunning(false);
                  protoToast("升级请求失败");
                });
                return;
              }
              fetch("/api/v1/node/upgrade/upload?filename=" + encodeURIComponent(file.name),
                { method: "POST", headers: { "X-Filename": file.name }, body: file, credentials: "same-origin" })
                .then(function (resp) { return resp.json().catch(function () { return null; }); })
                .then(function (ur) {
                  var stagedPath = (ur && ur.ok && ur.data && ur.data.path) ? ur.data.path : file.name;
                  return finishUpgrade(stagedPath, pendingFwSha);
                })
                .catch(function () {
                  setOpsRunning(false);
                  $("fw-status").textContent = "固件上传失败，可重试。";
                  protoToast("固件上传失败");
                  setLastOps("固件升级", "上传失败");
                });
            }
          });
        };

        [$("playback-task-list"), $("playback-record-list")].forEach(function (container) {
          container.addEventListener("click", function (e) {
            var btn = e.target.closest("button[data-task-action]");
            if (!btn) return;
            var row = btn.closest("[data-task-id]");
            if (row) taskAction(btn.getAttribute("data-task-action"), row.getAttribute("data-task-id"));
          });
        });

        document.querySelectorAll(".proto-side-sub-children a[data-zone]").forEach(function (a) {
          a.addEventListener("click", function (e) {
            e.preventDefault();
            showZone(a.getAttribute("data-zone"));
          });
        });
        function initFromBackend() {
          loadNodeInfo().then(function (r) {
            if (!r || !r.ok) { protoToast("加载节点信息失败"); return; }
            renderHardwareCapabilities();
            syncNodeView();
            refreshConfigStates();
            renderRole();
            renderPlaybackTasks();
            showZone("role");
            loadMediaState();
          });
        }
        initFromBackend();
        // periodic refresh
        setInterval(function () {
          if (opsRunning || registrationBusy) return;
          loadNodeInfo().then(function (r) {
            if (r && r.ok) { renderHardwareCapabilities(); syncNodeView(); renderRole(); }
          });
        }, 5000);
      })();
