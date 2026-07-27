#pragma once
// source/services/web_ui.hpp
//
// The GarageNX web front-end, embedded in the binary as a single string.
//
// EMBEDDED, NOT SERVED FROM DISK, deliberately. The page must work when the SD
// card is being browsed, rewritten, or is the very thing an install is writing
// to — reading UI assets off the same card the app is mutating invites a UI that
// breaks exactly when it is needed. It also means no romfs dependency and no
// "asset missing" failure mode: if GarageNX runs, its web UI runs.
//
// One file, no external requests: no CDN fonts, no frameworks, no analytics. A
// homebrew console UI must work on a LAN with no internet, and a page that
// silently reaches out to the network is the wrong shape for a tool that browses
// a private device.
//
// ── The progress design, and why it is what it is ────────────────────────────
// HttpServer runs a STRICTLY SERIAL accept loop: one connection, handled inline,
// then the next. During an install the loop is inside http_install() for minutes,
// so a browser polling a server-side progress endpoint would have its connection
// sit unanswered in the listen backlog until the install finished — a progress
// bar that updates only once the thing it tracks is over.
//
// So live transfer progress comes from the BROWSER's own XMLHttpRequest upload
// events, which need no server round-trip. For a streaming install this is an
// honest measure rather than a convenient one: the console consumes bytes as it
// receives them, so TCP backpressure means "bytes the console has accepted" is
// very close to "bytes the console has installed". /api/status is polled while
// IDLE (before and after) for install state and the last result.
//
// Making the server concurrent would allow true mid-install server-side stats,
// but it would put a second thread near the install object whose destruction
// order took real device crashes to get right (~HttpServer{stop()}). That is not
// a trade worth making for a progress bar; it is noted as future work instead.

namespace Services {

inline constexpr const char* kWebUiHtml = R"HTMLDOC(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>GarageNX</title>
<style>
  :root {
    --bg:#12141a; --surface:#1b1e26; --surface2:#232733; --line:#2e333f;
    --fg:#e8eaf0; --fg2:#98a0b3; --accent:#4da3ff; --ok:#3ecf8e; --err:#ff6b6b;
    --warn:#ffc857;
  }
  * { box-sizing:border-box; }
  body {
    margin:0; background:var(--bg); color:var(--fg); font:15px/1.5 system-ui,
    -apple-system, "Segoe UI", Roboto, sans-serif;
  }
  header {
    display:flex; align-items:center; gap:12px; padding:14px 20px;
    background:var(--surface); border-bottom:1px solid var(--line);
    position:sticky; top:0; z-index:5;
  }
  header h1 { font-size:17px; margin:0; font-weight:650; letter-spacing:.2px; }
  header .sp { flex:1; }
  #conn { font-size:13px; color:var(--fg2); }
  main { max-width:1000px; margin:0 auto; padding:20px; }
  section {
    background:var(--surface); border:1px solid var(--line); border-radius:10px;
    margin-bottom:20px; overflow:hidden;
  }
  section > h2 {
    font-size:13px; text-transform:uppercase; letter-spacing:.8px; margin:0;
    padding:12px 16px; color:var(--fg2); background:var(--surface2);
    border-bottom:1px solid var(--line); font-weight:650;
  }
  .pad { padding:16px; }

  /* ── Uploader ── */
  #drop {
    border:2px dashed var(--line); border-radius:10px; padding:28px 16px;
    text-align:center; transition:border-color .15s, background .15s;
    cursor:pointer;
  }
  #drop.hot { border-color:var(--accent); background:rgba(77,163,255,.07); }
  #drop p { margin:6px 0; color:var(--fg2); }
  #drop strong { color:var(--fg); }
  .row { display:flex; gap:12px; align-items:center; flex-wrap:wrap; }
  .row + .row { margin-top:12px; }
  label.tgt {
    display:inline-flex; align-items:center; gap:7px; padding:7px 13px;
    border:1px solid var(--line); border-radius:7px; cursor:pointer;
    background:var(--surface2);
  }
  label.tgt input { accent-color:var(--accent); }
  button {
    font:inherit; font-weight:600; padding:9px 18px; border-radius:7px;
    border:1px solid var(--accent); background:var(--accent); color:#08111c;
    cursor:pointer;
  }
  button.ghost { background:transparent; color:var(--fg); border-color:var(--line); }
  button:disabled { opacity:.45; cursor:not-allowed; }

  /* ── Progress ── */
  #prog { display:none; }
  #prog.on { display:block; }
  .bar {
    height:12px; background:var(--surface2); border-radius:99px; overflow:hidden;
    border:1px solid var(--line);
  }
  .bar > div {
    height:100%; width:0%; background:var(--accent); transition:width .2s linear;
  }
  .stats {
    display:flex; gap:22px; flex-wrap:wrap; margin-top:10px;
    font-variant-numeric:tabular-nums; color:var(--fg2); font-size:13px;
  }
  .stats b { color:var(--fg); font-weight:600; }
  #result { margin-top:12px; font-weight:600; display:none; }
  #result.on { display:block; }
  #result.ok { color:var(--ok); }
  #result.err { color:var(--err); }

  /* ── Browser ── */
  #crumbs {
    padding:10px 16px; border-bottom:1px solid var(--line); font-size:13px;
    color:var(--fg2); word-break:break-all;
  }
  #crumbs a { color:var(--accent); text-decoration:none; cursor:pointer; }
  #crumbs a:hover { text-decoration:underline; }
  ul.files { list-style:none; margin:0; padding:0; }
  ul.files li {
    display:flex; align-items:center; gap:12px; padding:10px 16px;
    border-bottom:1px solid var(--line);
  }
  ul.files li:last-child { border-bottom:0; }
  ul.files li:hover { background:var(--surface2); }
  .ico { width:20px; text-align:center; opacity:.85; flex:none; }
  .nm { flex:1; word-break:break-all; }
  .nm a { color:var(--fg); text-decoration:none; cursor:pointer; }
  .nm a:hover { color:var(--accent); }
  .sz { color:var(--fg2); font-size:13px; font-variant-numeric:tabular-nums;
        flex:none; }
  .empty, .err { padding:18px 16px; color:var(--fg2); }
  .err { color:var(--err); }
  footer { text-align:center; color:var(--fg2); font-size:12px; padding:8px 0 28px; }
</style>
</head>
<body>
<header>
  <h1>GarageNX</h1>
  <span class="sp"></span>
  <span id="conn">&nbsp;</span>
</header>

<main>
  <section>
    <h2>Install</h2>
    <div class="pad">
      <div id="drop">
        <p><strong>Drop an NSP / NSZ / XCI / XCZ here</strong></p>
        <p>or click to choose a file</p>
        <input type="file" id="file" hidden
               accept=".nsp,.nsz,.xci,.xcz">
      </div>
      <div class="row" style="margin-top:14px">
        <span style="color:var(--fg2);font-size:13px">Install to:</span>
        <label class="tgt"><input type="radio" name="tgt" value="sd" checked> SD Card</label>
        <label class="tgt"><input type="radio" name="tgt" value="nand"> NAND</label>
        <span class="sp" style="flex:1"></span>
        <button id="go" disabled>Install</button>
        <button id="cancel" class="ghost" style="display:none">Cancel</button>
      </div>
      <div class="row"><span id="chosen" style="color:var(--fg2);font-size:13px"></span></div>

      <div id="prog" style="margin-top:16px">
        <div class="bar"><div id="fill"></div></div>
        <div class="stats">
          <span><b id="pct">0%</b></span>
          <span>Sent <b id="sent">0</b> / <b id="total">0</b></span>
          <span>Speed <b id="speed">—</b></span>
          <span>ETA <b id="eta">—</b></span>
        </div>
      </div>
      <div id="result"></div>
    </div>
  </section>

  <section>
    <h2>Storage</h2>
    <div id="crumbs">/</div>
    <div id="listing"><div class="empty">Loading…</div></div>
  </section>
</main>
<footer>GarageNX &middot; served from the console</footer>

<script>
"use strict";
var CWD = "/";
var picked = null;
var xhr = null;

function h(n) {
  if (n === null || n === undefined) return "—";
  var u = ["B","KB","MB","GB","TB"], i = 0, v = Number(n);
  while (v >= 1024 && i < u.length - 1) { v /= 1024; i++; }
  return (i === 0 ? v.toFixed(0) : v.toFixed(1)) + " " + u[i];
}
function hms(s) {
  if (!isFinite(s) || s < 0) return "—";
  s = Math.round(s);
  var m = Math.floor(s / 60), r = s % 60;
  if (m >= 60) { var hh = Math.floor(m / 60); m %= 60;
                 return hh + "h " + m + "m"; }
  return m > 0 ? (m + "m " + r + "s") : (r + "s");
}
function esc(t) {
  return String(t).replace(/[&<>"']/g, function (c) {
    return { "&":"&amp;", "<":"&lt;", ">":"&gt;", '"':"&quot;", "'":"&#39;" }[c];
  });
}
function join(dir, name) {
  return dir.endsWith("/") ? dir + name : dir + "/" + name;
}
function parent(p) {
  if (p === "/" || p === "") return "/";
  var t = p.replace(/\/+$/, "");
  var i = t.lastIndexOf("/");
  return i <= 0 ? "/" : t.slice(0, i);
}

/* ── Browser ─────────────────────────────────────────────────────────────── */
function crumbs(p) {
  var el = document.getElementById("crumbs");
  var parts = p.split("/").filter(Boolean);
  var html = '<a data-p="/">Storage</a>';
  var acc = "";
  for (var i = 0; i < parts.length; i++) {
    acc += "/" + parts[i];
    html += ' / <a data-p="' + esc(acc) + '">' + esc(parts[i]) + "</a>";
  }
  el.innerHTML = html;
  var links = el.querySelectorAll("a");
  for (var j = 0; j < links.length; j++) {
    links[j].onclick = function () { load(this.getAttribute("data-p")); };
  }
}

function load(p) {
  CWD = p || "/";
  crumbs(CWD);
  var out = document.getElementById("listing");
  out.innerHTML = '<div class="empty">Loading…</div>';

  fetch("/api/list?path=" + encodeURIComponent(CWD))
    .then(function (r) {
      if (!r.ok) throw new Error("HTTP " + r.status);
      return r.json();
    })
    .then(function (d) {
      var e = d.entries || [];
      e.sort(function (a, b) {
        if ((a.type === "dir") !== (b.type === "dir")) return a.type === "dir" ? -1 : 1;
        return a.name.localeCompare(b.name);
      });
      var html = "<ul class='files'>";
      if (CWD !== "/") {
        html += "<li><span class='ico'>&#8617;</span><span class='nm'>" +
                "<a data-d='" + esc(parent(CWD)) + "'>..</a></span><span class='sz'></span></li>";
      }
      if (!e.length && CWD === "/") {
        html += "";
      }
      for (var i = 0; i < e.length; i++) {
        var it = e[i], full = join(CWD, it.name);
        if (it.type === "dir") {
          html += "<li><span class='ico'>&#128193;</span><span class='nm'>" +
                  "<a data-d='" + esc(full) + "'>" + esc(it.name) + "</a></span>" +
                  "<span class='sz'></span></li>";
        } else {
          // encodeURI (not encodeURIComponent) so '/' survives as a separator.
          // Without this a filename containing '#' truncates the URL at the
          // fragment and '?' becomes a query — both silently fetch the wrong
          // thing, and both occur in real title names.
          html += "<li><span class='ico'>&#128196;</span><span class='nm'>" +
                  "<a href='" + esc(encodeURI(full)) + "' download>" +
                  esc(it.name) + "</a></span>" +
                  "<span class='sz'>" + h(it.size) + "</span></li>";
        }
      }
      html += "</ul>";
      if (!e.length) {
        html = CWD === "/"
          ? "<div class='empty'>No storage surfaces are enabled. Enable them in Settings &rsaquo; Storage Surfaces &rsaquo; HTTP.</div>"
          : "<div class='empty'>Empty.</div>";
      }
      out.innerHTML = html;

      var dirs = out.querySelectorAll("a[data-d]");
      for (var k = 0; k < dirs.length; k++) {
        dirs[k].onclick = function (ev) {
          ev.preventDefault();
          load(this.getAttribute("data-d"));
        };
      }
    })
    .catch(function (err) {
      out.innerHTML = "<div class='err'>Could not list this folder — " +
                      esc(err.message) + "</div>";
    });
}

/* ── Uploader ────────────────────────────────────────────────────────────── */
var drop = document.getElementById("drop");
var fileEl = document.getElementById("file");
var goEl = document.getElementById("go");
var cancelEl = document.getElementById("cancel");

drop.onclick = function () { fileEl.click(); };
["dragenter", "dragover"].forEach(function (n) {
  drop.addEventListener(n, function (e) {
    e.preventDefault(); e.stopPropagation(); drop.classList.add("hot");
  });
});
["dragleave", "drop"].forEach(function (n) {
  drop.addEventListener(n, function (e) {
    e.preventDefault(); e.stopPropagation(); drop.classList.remove("hot");
  });
});
drop.addEventListener("drop", function (e) {
  if (e.dataTransfer.files && e.dataTransfer.files.length) choose(e.dataTransfer.files[0]);
});
fileEl.onchange = function () { if (fileEl.files.length) choose(fileEl.files[0]); };

function choose(f) {
  picked = f;
  document.getElementById("chosen").textContent = f.name + "  (" + h(f.size) + ")";
  goEl.disabled = false;
  var r = document.getElementById("result");
  r.className = ""; r.textContent = "";
}

function target() {
  var rs = document.getElementsByName("tgt");
  for (var i = 0; i < rs.length; i++) if (rs[i].checked) return rs[i].value;
  return "sd";
}

goEl.onclick = function () {
  if (!picked) return;
  var t0 = Date.now(), lastT = t0, lastB = 0, speed = 0;

  var prog = document.getElementById("prog");
  var fill = document.getElementById("fill");
  var res = document.getElementById("result");
  prog.classList.add("on");
  res.className = ""; res.textContent = "";
  goEl.disabled = true; cancelEl.style.display = "";
  document.getElementById("total").textContent = h(picked.size);

  xhr = new XMLHttpRequest();
  xhr.open("PUT", "/install/" + target() + "/" + encodeURIComponent(picked.name));

  xhr.upload.onprogress = function (e) {
    if (!e.lengthComputable) return;
    var pct = e.loaded / e.total;
    fill.style.width = (pct * 100).toFixed(1) + "%";
    document.getElementById("pct").textContent = (pct * 100).toFixed(1) + "%";
    document.getElementById("sent").textContent = h(e.loaded);

    var now = Date.now(), dt = (now - lastT) / 1000;
    if (dt >= 0.5) {
      var inst = (e.loaded - lastB) / dt;
      speed = speed ? (speed * 0.7 + inst * 0.3) : inst;   // smooth the jitter
      lastT = now; lastB = e.loaded;
      document.getElementById("speed").textContent = h(speed) + "/s";
      document.getElementById("eta").textContent =
        speed > 0 ? hms((e.total - e.loaded) / speed) : "—";
    }
  };

  xhr.onload = function () {
    cancelEl.style.display = "none"; goEl.disabled = false; xhr = null;
    var ok = xhr2ok(this.status);
    res.className = "on " + (ok ? "ok" : "err");
    res.textContent = ok
      ? "Install complete — " + picked.name
      : "Install failed (HTTP " + this.status + "). " +
        (this.responseText || "").trim();
    if (ok) { fill.style.width = "100%";
              document.getElementById("pct").textContent = "100%"; }
    load(CWD);
    poll();
  };
  xhr.onerror = function () {
    cancelEl.style.display = "none"; goEl.disabled = false; xhr = null;
    res.className = "on err";
    res.textContent = "Connection lost during upload.";
  };
  xhr.onabort = function () {
    cancelEl.style.display = "none"; goEl.disabled = false; xhr = null;
    res.className = "on err";
    res.textContent = "Upload cancelled.";
  };

  xhr.send(picked);
};

function xhr2ok(s) { return s >= 200 && s < 300; }

cancelEl.onclick = function () { if (xhr) xhr.abort(); };

/* ── Idle status poll ────────────────────────────────────────────────────────
   Polled only when NOT uploading. The server handles one connection at a time,
   so during an install it cannot answer — the live numbers above come from the
   browser's own upload events instead. */
function poll() {
  if (xhr) return;
  fetch("/api/status")
    .then(function (r) { return r.json(); })
    .then(function (s) {
      document.getElementById("conn").textContent =
        s.installing ? "installing…" : "ready";
    })
    .catch(function () {
      document.getElementById("conn").textContent = "offline";
    });
}

load("/");
poll();
setInterval(poll, 5000);
</script>
</body>
</html>
)HTMLDOC";

} // namespace Services
