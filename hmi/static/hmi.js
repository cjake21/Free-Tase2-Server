"use strict";

// Tie-line flow above this (MW) is shown as an overflow / threshold-exceeded
// condition on the remote view. Synthetic lab threshold only.
const THRESH_MW = 100;

let state = null;

// ---- one-line diagram -----------------------------------------------------

function buildSvg(p) {
  // p is the station prefix ("A" or "B"). IDs are suffixed with it.
  return `
  <svg viewBox="0 0 360 175" role="img" aria-label="single line diagram">
    <!-- generator -->
    <circle class="gen" cx="38" cy="92" r="17"></circle>
    <text class="gen-label" x="38" y="97">G</text>
    <line class="wire" id="gw-${p}" x1="55" y1="92" x2="92" y2="92"></line>
    <!-- local bus -->
    <line class="bus" id="bus-${p}" x1="92" y1="52" x2="92" y2="132"></line>
    <text class="lbl" x="92" y="46" text-anchor="middle">BUS</text>
    <text class="val" id="volt-${p}" x="120" y="46" text-anchor="start">– kV</text>
    <!-- breaker on the tie -->
    <line class="wire" id="tw1-${p}" x1="92" y1="92" x2="146" y2="92"></line>
    <rect class="brk-box closed" id="brk-${p}" x="146" y="84" width="16" height="16" rx="2"></rect>
    <text class="tag" id="brklbl-${p}" x="154" y="118">CLOSED</text>
    <!-- tie line to remote bus -->
    <line class="wire" id="tw2-${p}" x1="162" y1="92" x2="300" y2="92"></line>
    <polygon class="flow-arrow" id="arr-${p}" points="224,86 240,92 224,98"></polygon>
    <text class="val" id="flow-${p}" x="231" y="78" text-anchor="middle">– MW</text>
    <text class="lbl" x="231" y="112" text-anchor="middle">TIE LINE</text>
    <!-- remote bus -->
    <line class="bus" id="rbus-${p}" x1="300" y1="52" x2="300" y2="132"></line>
    <circle id="rstat-${p}" cx="320" cy="92" r="6" fill="#4a5763"></circle>
    <text class="lbl" x="300" y="46" text-anchor="middle">${p === "A" ? "TIE" : "LOCAL"}</text>
  </svg>`;
}

function setText(id, txt) { const e = document.getElementById(id); if (e) e.textContent = txt; }
function setClass(id, cls) { const e = document.getElementById(id); if (e) e.setAttribute("class", cls); }

function fmt(v, digits) {
  return (v === null || v === undefined) ? "–" : Number(v).toFixed(digits);
}

// vals: {tm1, tm2, ts1, ts2} ; changed: {tmX: bool} ; live: bool (energized)
function renderStation(p, vals, changed) {
  const flow = vals.tm1, volt = vals.tm2, closed = (vals.ts1 === 1), rstat = (vals.ts2 === 1);
  const over = (flow !== null && flow !== undefined && Math.abs(flow) > THRESH_MW);
  const energized = closed;

  // flow value + direction arrow
  const flowCls = "val" + (over ? " over" : (changed && changed.tm1 ? " changed" : ""));
  setText(`flow-${p}`, (flow === null ? "–" : fmt(flow, 1)) + " MW");
  setClass(`flow-${p}`, flowCls);
  const positive = (flow || 0) >= 0;
  const arr = document.getElementById(`arr-${p}`);
  if (arr) {
    arr.setAttribute("points", positive ? "224,86 240,92 224,98" : "240,86 224,92 240,98");
    arr.setAttribute("class", "flow-arrow" + (over ? " over" : ""));
  }

  // voltage
  setText(`volt-${p}`, (volt === null ? "–" : fmt(volt, 1)) + " kV");
  setClass(`volt-${p}`, "val" + (changed && changed.tm2 ? " changed" : ""));

  // breaker + energization
  setClass(`brk-${p}`, "brk-box " + (closed ? "closed" : "open"));
  setText(`brklbl-${p}`, closed ? "CLOSED" : "OPEN");
  ["gw", "tw1", "tw2"].forEach(w => setClass(`${w}-${p}`, "wire" + (energized ? "" : " de")));
  setClass(`bus-${p}`, "bus" + (energized ? "" : " de"));
  const rs = document.getElementById(`rstat-${p}`);
  if (rs) rs.setAttribute("fill", rstat ? "#34c759" : "#4a5763");
}

// ---- state -> view --------------------------------------------------------

function currentA(p) {
  // Station A shows the operator's local value: the manual setpoint if pinned,
  // otherwise the current server value (which is what Station B is receiving).
  const pt = state.points[p];
  if (pt && pt.mode === "manual" && pt.setpoint !== null) return pt.setpoint;
  return state.stationB[p];
}

function render() {
  if (!state) return;

  // connection status
  const up = state.online && state.online.A && state.online.B;
  const conn = document.getElementById("conn");
  conn.textContent = up ? `ICCP linked · ${state.server.host}:${state.server.port}` : "ICCP link down";
  conn.className = "conn " + (up ? "up" : "down");

  // Station A (intent / local)
  renderStation("A", {
    tm1: currentA("tm1"), tm2: currentA("tm2"),
    ts1: currentA("ts1"), ts2: currentA("ts2"),
  }, null);

  // mode badges + input placeholders
  ["tm1", "tm2", "ts1"].forEach(p => {
    const pt = state.points[p] || { mode: "auto" };
    const m = document.getElementById(`modeA-${p}`);
    if (m) { m.textContent = pt.mode.toUpperCase(); m.className = "mode " + pt.mode; }
    const inp = document.getElementById(`inA-${p}`);
    if (inp && document.activeElement !== inp) {
      const v = currentA(p);
      inp.placeholder = (v === null || v === undefined) ? "" : Number(v).toFixed(1);
    }
  });

  // Station B (received over ICCP) + changed-from-baseline highlight
  const b = state.stationB, base = b.baseline || {};
  const changed = {};
  ["tm1", "tm2"].forEach(p => {
    changed[p] = (base[p] !== undefined && b[p] !== null && Math.abs((b[p] || 0) - (base[p] || 0)) > 0.5);
  });
  renderStation("B", { tm1: b.tm1, tm2: b.tm2, ts1: b.ts1, ts2: b.ts2 }, changed);

  // reporting status
  setText("rep-ts", state.meta.transferset + (b.report_count > 0 ? " (enabled)" : ""));
  setText("rep-count", b.report_count);
  setText("rep-time", b.last_report_time || "–");
  setText("rep-cond", condName(b.cond));

  // overflow banner (the visible power-flow condition)
  const over = (b.tm1 !== null && b.tm1 !== undefined && Math.abs(b.tm1) > THRESH_MW);
  document.getElementById("overflow").classList.toggle("hidden", !over);
  setText("thresh", THRESH_MW);

  // inspector
  setText("i-version", state.meta.version || "–");
  setText("i-features", state.meta.features || "–");
  setText("i-blt", state.meta.blt || "–");
  setText("i-next", state.meta.next_ts || "–");
  setText("i-dataset", state.meta.dataset);
  setText("i-ts", state.meta.transferset);
}

function condName(c) {
  if (c === null || c === undefined) return "–";
  const parts = [];
  if (c & 0x02) parts.push("integrity");
  if (c & 0x04) parts.push("change");
  if (c & 0x01) parts.push("interval");
  return parts.length ? parts.join("+") : String(c);
}

// ---- control actions ------------------------------------------------------

async function control(body) {
  try {
    await fetch("/api/control", {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
    });
  } catch (e) { /* SSE will resync */ }
}

function wireControls() {
  document.querySelectorAll("button[data-act]").forEach(btn => {
    btn.addEventListener("click", () => {
      const act = btn.dataset.act, item = btn.dataset.item;
      if (act === "set") {
        const inp = document.getElementById(`inA-${item}`);
        if (inp.value !== "") control({ action: "set", item, value: parseFloat(inp.value) });
        inp.value = "";
      } else if (act === "release") {
        control({ action: "release", item });
      } else if (act === "open") {
        control({ action: "breaker", closed: false });
      } else if (act === "close") {
        control({ action: "breaker", closed: true });
      }
    });
  });
}

// ---- boot -----------------------------------------------------------------

function init() {
  document.getElementById("oneA").innerHTML = buildSvg("A");
  document.getElementById("oneB").innerHTML = buildSvg("B");
  wireControls();

  fetch("/api/state").then(r => r.json()).then(s => { state = s; render(); });

  const es = new EventSource("/api/events");
  es.onmessage = ev => { try { state = JSON.parse(ev.data); render(); } catch (e) {} };
  es.onerror = () => {
    const conn = document.getElementById("conn");
    conn.textContent = "stream reconnecting…"; conn.className = "conn down";
  };
}

document.addEventListener("DOMContentLoaded", init);
