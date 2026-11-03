/**
 * html-proto：toast / 确认框 / 二级折叠 / 分组展开 / 高亮
 * 复制到 products/{id}/html-proto/_shared/proto-shell.js
 * 壳层能力仅服务后台型业务页或已确认的管理配置态；用户运行态不默认加载。
 */
(function () {
  function ensureToast() {
    var el = document.getElementById("proto-toast");
    if (!el) {
      el = document.createElement("div");
      el.id = "proto-toast";
      el.className = "proto-toast";
      document.body.appendChild(el);
    }
    return el;
  }

  window.protoToast = function (msg) {
    var el = ensureToast();
    el.textContent = msg;
    el.classList.add("is-open");
    clearTimeout(el._timer);
    el._timer = setTimeout(function () {
      el.classList.remove("is-open");
    }, 1800);
  };

  window.protoConfirm = function (opts) {
    var mask = document.getElementById("proto-confirm-mask");
    if (!mask) {
      mask = document.createElement("div");
      mask.id = "proto-confirm-mask";
      mask.className = "proto-mask";
      mask.innerHTML =
        '<div class="proto-modal" role="dialog">' +
        '<div class="proto-modal-header"><span id="proto-confirm-title">确认</span>' +
        '<button type="button" class="proto-btn" id="proto-confirm-x" style="padding:0 8px;height:28px">×</button></div>' +
        '<div class="proto-modal-body" id="proto-confirm-body"></div>' +
        '<div class="proto-modal-footer">' +
        '<button type="button" class="proto-btn" id="proto-confirm-cancel">取消</button>' +
        '<button type="button" class="proto-btn proto-btn-primary" id="proto-confirm-ok">确认</button>' +
        "</div></div>";
      document.body.appendChild(mask);
    }
    document.getElementById("proto-confirm-title").textContent = opts.title || "确认";
    document.getElementById("proto-confirm-body").textContent = opts.content || "";
    mask.classList.add("is-open");
    function close() {
      mask.classList.remove("is-open");
    }
    function bind(id, fn) {
      document.getElementById(id).onclick = function () {
        close();
        if (fn) fn();
      };
    }
    bind("proto-confirm-cancel", opts.onCancel);
    bind("proto-confirm-x", opts.onCancel);
    bind("proto-confirm-ok", opts.onOk);
  };

  window.protoNotifyUnbuilt = function () {
    protoToast("本批未出示意");
  };

  window.protoOpenDrawer = function (id) {
    var el = document.getElementById(id);
    if (!el) return;
    el.hidden = false;
    el.classList.add("is-open");
  };

  window.protoCloseDrawer = function (id) {
    var el = id ? document.getElementById(id) : document.querySelector(".proto-drawer-mask.is-open");
    if (!el) return;
    el.classList.remove("is-open");
    el.hidden = true;
  };

  var PROTO_COL_SHARES = [
    { cls: "col-check", share: 4 },
    { cls: "col-idx", share: 6 },
    { cls: "col-thumb", share: 8 },
    { cls: "col-num", share: 7 },
    { cls: "col-count", share: 8 },
    { cls: "col-status", share: 8 },
    { cls: "col-person", share: 10 },
    { cls: "col-name", share: 14 },
    { cls: "col-time", share: 14 },
    { cls: "col-text", share: 22 },
    { cls: "col-ops", share: 11 }
  ];

  function protoColShare(th) {
    if (th.classList.contains("col-ops") && th.classList.contains("is-ops-3")) return 14;
    var i;
    for (i = 0; i < PROTO_COL_SHARES.length; i++) {
      if (th.classList.contains(PROTO_COL_SHARES[i].cls)) return PROTO_COL_SHARES[i].share;
    }
    return 0;
  }

  /** 本表出现的列类型按份额归一化到 100%，避免某一列吞剩余宽度 */
  window.protoLayoutListTables = function () {
    document
      .querySelectorAll(".proto-table-wrap > .proto-table, .proto-picker-table-wrap > .proto-table")
      .forEach(function (table) {
        var heads = table.querySelectorAll("thead th");
        if (!heads.length) return;
        var shares = [];
        var total = 0;
        heads.forEach(function (th) {
          var s = protoColShare(th);
          shares.push(s);
          total += s;
        });
        if (!total) return;
        heads.forEach(function (th, idx) {
          if (!shares[idx]) return;
          th.style.width = ((shares[idx] / total) * 100).toFixed(2) + "%";
        });
      });
  };

  document.addEventListener("DOMContentLoaded", function () {
    // 仅使用 DVS 平台现有的 TDesign 图标字体；不使用自绘占位图形。
    document.querySelectorAll(".proto-side-item").forEach(function (item) {
      var icon = item.querySelector(".proto-side-icon");
      if (!icon || icon.querySelector(".t-icon")) return;
      var page = item.getAttribute("data-page");
      icon.innerHTML = '<i class="t-icon ' + (page === "schedule" ? "t-icon-control-platform-filled" : "t-icon-menu-filled") + '" aria-hidden="true"></i>';
    });
    document.querySelectorAll(".proto-side-sub-group").forEach(function (group) {
      var icon = group.querySelector(".proto-menu-ico");
      var label = group.querySelector(".proto-menu-label");
      if (!icon || !label || icon.querySelector(".t-icon")) return;
      var name = label.textContent.trim() === "设备管理" ? "t-icon-device-filled" : "t-icon-control-platform-filled";
      icon.innerHTML = '<i class="t-icon ' + name + '" aria-hidden="true"></i>';
    });

    var shell = document.querySelector(".proto-shell");
    var fold = document.getElementById("proto-fold-sub");
    if (shell && fold) {
      fold.addEventListener("click", function () {
        shell.classList.toggle("is-sub-collapsed");
        fold.setAttribute(
          "title",
          shell.classList.contains("is-sub-collapsed") ? "展开二级菜单" : "收起二级菜单"
        );
      });
    }

    document.querySelectorAll(".proto-side-sub-parent").forEach(function (btn) {
      btn.addEventListener("click", function () {
        var group = btn.closest(".proto-side-sub-group");
        if (group) group.classList.toggle("is-open");
      });
    });

    var page = document.body.getAttribute("data-proto-page");
    var l1 = document.body.getAttribute("data-proto-l1") || page;
    if (l1) {
      document.querySelectorAll(".proto-side-item[data-page]").forEach(function (item) {
        if (item.getAttribute("data-page") === l1) item.classList.add("is-active");
      });
    }
    if (page) {
      document.querySelectorAll(".proto-side-sub-children a[data-page]").forEach(function (item) {
        if (item.getAttribute("data-page") === page) {
          item.classList.add("is-active");
          var group = item.closest(".proto-side-sub-group");
          if (group) group.classList.add("is-open");
        }
      });
    }

    document.querySelectorAll(".proto-side-sub a.is-disabled, .proto-hub-card.is-disabled").forEach(function (a) {
      a.addEventListener("click", function (e) {
        e.preventDefault();
        protoNotifyUnbuilt();
      });
    });

    document.querySelectorAll("[data-drawer-close]").forEach(function (btn) {
      btn.addEventListener("click", function () {
        var mask = btn.closest(".proto-drawer-mask");
        if (mask) {
          mask.classList.remove("is-open");
          mask.hidden = true;
        }
      });
    });

    document.querySelectorAll(".proto-drawer-mask").forEach(function (mask) {
      mask.addEventListener("click", function (e) {
        if (e.target === mask) {
          mask.classList.remove("is-open");
          mask.hidden = true;
        }
      });
    });

    document.querySelectorAll("[data-picker-close]").forEach(function (btn) {
      btn.addEventListener("click", function () {
        var mask = btn.closest(".proto-mask");
        if (mask) mask.classList.remove("is-open");
      });
    });

    document.querySelectorAll(".proto-mask").forEach(function (mask) {
      mask.addEventListener("click", function (e) {
        if (e.target === mask) mask.classList.remove("is-open");
      });
    });

    window.protoLayoutListTables();

    document.addEventListener("click", function (e) {
      var row = e.target.closest(".proto-tree-row");
      if (!row) return;
      var caret = row.querySelector(".proto-tree-caret");
      if (!caret || caret.classList.contains("is-leaf")) return;
      if (e.target.closest("[data-pick-id]")) return;
      var node = row.closest(".proto-tree-node");
      if (node) node.classList.toggle("is-open");
    });
  });
})();
