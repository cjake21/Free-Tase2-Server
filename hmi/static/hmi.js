"use strict";

// Tie-line power flow above this (MW) is shown as an over-the-limit condition on
// the remote view. Synthetic training threshold only.
let THRESH_MW = 100;

let state = null;

// ---- single-line diagram --------------------------------------------------

// Build the station one-line diagram. p is the station prefix ("A" or "B");
// element ids are suffixed with it so render() can update each piece.
function buildSvg(p) {
  return `
  <svg viewBox="0 0 460 200" role="img" aria-label="single line diagram">
    <!-- generator -->
    <circle class="gen-ring" cx="44" cy="90" r="20"></circle>
    <path class="gen-wave" d="M34 90 q5 -9 10 0 q5 9 10 0"></path>
    <text class="cap" x="44" y="126" text-anchor="middle">Generator</text>

    <!-- generator to local bus (always energized: the generator feeds the bus) -->
    <line class="wire live" x1="64" y1="90" x2="120" y2="90"></line>

    <!-- local busbar (BUS A) -->
    <line class="bus live" id="busL-${p}" x1="120" y1="50" x2="120" y2="130"></line>
    <text class="busname" x="120" y="42" text-anchor="middle">BUS A</text>
    <text class="val" id="voltval-${p}" x="128" y="62" text-anchor="start">-- kV</text>

    <!-- bus to breaker -->
    <line class="wire live" x1="120" y1="90" x2="178" y2="90"></line>

    <!-- tie-line breaker -->
    <rect class="brk" id="brk-${p}" x="178" y="78" width="24" height="24" rx="3"></rect>
    <text class="cap" x="190" y="120" text-anchor="middle">Breaker</text>
    <text class="state" id="brkstate-${p}" x="190" y="70" text-anchor="middle">--</text>

    <!-- tie line to the neighbor (energized only when the breaker is closed) -->
    <line class="wire" id="tieR-${p}" x1="202" y1="90" x2="372" y2="90"></line>
    <line class="flowdash hidden" id="flowdash-${p}" x1="202" y1="90" x2="372" y2="90"></line>
    <polygon class="flow-arrow hidden" id="flowarrow-${p}" points="280,84 296,90 280,96"></polygon>
    <text class="val" id="flowval-${p}" x="287" y="74" text-anchor="middle">-- MW</text>
    <text class="cap" x="287" y="120" text-anchor="middle">Tie line</text>

    <!-- neighbor busbar (BUS B) and demand -->
    <line class="bus" id="busR-${p}" x1="372" y1="50" x2="372" y2="130"></line>
    <text class="busname" x="372" y="42" text-anchor="middle">BUS B</text>
    <line class="wire" id="neigh-${p}" x1="372" y1="90" x2="412" y2="90"></line>
    <polygon class="load" id="load-${p}" points="406,96 418,96 412,110"></polygon>
    <text class="cap" x="410" y="126" text-anchor="middle">Neighbor</text>
  </svg>`;
}

function setText(id, txt) { const e = document.getElementById(id); if (e) e.textContent = txt; }
function show(id, on) { const e = document.getElementById(id); if (e) e.classList.toggle("hidden", !on); }

function fmt(v, digits) {
  return (v === null || v === undefined) ? "--" : Number(v).toFixed(digits);
}

// vals: {tm1, tm2, ts1, ts2}; changed: {tmX: bool} or null
function renderStation(p, vals, changed) {
  const flow = vals.tm1, volt = vals.tm2;
  const closed = (vals.ts1 === 1);
  const over = (flow !== null && flow !== undefined && Math.abs(flow) > THRESH_MW);

  // power flow value + colour
  setText(`flowval-${p}`, (flow === null || flow === undefined ? "--" : fmt(flow, 1)) + " MW");
  const fv = document.getElementById(`flowval-${p}`);
  if (fv) fv.setAttribute("class", "val" + (over ? " over" : (changed && changed.tm1 ? " changed" : "")));

  // voltage value + colour
  setText(`voltval-${p}`, (volt === null || volt === undefined ? "--" : fmt(volt, 1)) + " kV");
  const vv = document.getElementById(`voltval-${p}`);
  if (vv) vv.setAttribute("class", "val" + (changed && changed.tm2 ? " changed" : ""));

  // breaker symbol + word
  const brk = document.getElementById(`brk-${p}`);
  if (brk) brk.setAttribute("class", "brk " + (closed ? "closed" : "open"));
  setText(`brkstate-${p}`, closed ? "CLOSED" : "OPEN");

  // energize the tie side only when the breaker is closed
  ["tieR", "neigh"].forEach(w => {
    const e = document.getElementById(`${w}-${p}`);
    if (e) e.setAttribute("class", "wire" + (closed ? " live" : ""));
  });
  const busR = document.getElementById(`busR-${p}`);
  if (busR) busR.setAttribute("class", "bus" + (closed ? " live" : ""));
  const load = document.getElementById(`load-${p}`);
  if (load) load.setAttribute("class", "load" + (closed ? " live" : ""));

  // moving dashes + direction arrow only when energized
  show(`flowdash-${p}`, closed);
  show(`flowarrow-${p}`, closed);
  const arr = document.getElementById(`flowarrow-${p}`);
  if (arr) {
    const positive = (flow || 0) >= 0;
    arr.setAttribute("points", positive ? "280,84 296,90 280,96" : "296,84 280,90 296,96");
    arr.setAttribute("class", "flow-arrow" + (over ? " over" : "") + (closed ? "" : " hidden"));
  }
  const dash = document.getElementById(`flowdash-${p}`);
  if (dash) dash.setAttribute("class", "flowdash" + (closed ? "" : " hidden") + ((flow || 0) < 0 ? " rev" : ""));
}

// ---- state -> view --------------------------------------------------------

function currentA(p) {
  // Station A shows the operator's local value: the manual setpoint if pinned,
  // otherwise the live server value (which is what Station B is receiving).
  const pt = state.points[p];
  if (pt && pt.mode === "manual" && pt.setpoint !== null) return pt.setpoint;
  return state.stationB[p];
}

function render() {
  if (!state) return;
  if (state.lab && typeof state.lab.threshold === "number") THRESH_MW = state.lab.threshold;

  // data-link status, in plain words
  const up = state.online && state.online.A && state.online.B;
  const conn = document.getElementById("conn");
  conn.textContent = up ? "Linked to Station B" : "Data link down";
  conn.className = "link-pill " + (up ? "up" : "down");

  // Station A (what the operator is sending)
  renderStation("A", {
    tm1: currentA("tm1"), tm2: currentA("tm2"),
    ts1: currentA("ts1"), ts2: currentA("ts2"),
  }, null);

  // mode badges + input hints
  ["tm1", "tm2", "ts1"].forEach(p => {
    const pt = state.points[p] || { mode: "auto" };
    const m = document.getElementById(`modeA-${p}`);
    if (m) { m.textContent = pt.mode === "manual" ? "MANUAL" : "AUTO"; m.className = "mode " + pt.mode; }
    const inp = document.getElementById(`inA-${p}`);
    if (inp && document.activeElement !== inp) {
      const v = currentA(p);
      inp.placeholder = (v === null || v === undefined) ? "" : Number(v).toFixed(1);
    }
  });

  // Station B (only what arrived over the link) + changed-from-normal highlight
  const b = state.stationB, base = b.baseline || {};
  const changed = {};
  ["tm1", "tm2"].forEach(p => {
    changed[p] = (base[p] !== undefined && b[p] !== null && Math.abs((b[p] || 0) - (base[p] || 0)) > 0.5);
  });
  renderStation("B", { tm1: b.tm1, tm2: b.tm2, ts1: b.ts1, ts2: b.ts2 }, changed);

  // data feed status
  setText("rep-link", b.report_count > 0 ? "receiving updates" : "waiting for first update");
  setText("rep-count", b.report_count);
  setText("rep-time", b.last_report_time || "none yet");
  setText("rep-cond", reason(b.cond));

  // over-the-limit warning
  const over = (b.tm1 !== null && b.tm1 !== undefined && Math.abs(b.tm1) > THRESH_MW);
  show("overflow", over);
  setText("thresh", THRESH_MW);

  // training validation marker, revealed once the server confirms the power-flow
  // condition (only present in the lab build)
  const m2 = state.lab && state.lab.marker2;
  const revealed = !!(m2 && m2.revealed && m2.value);
  show("marker2", revealed);
  if (revealed) setText("marker2-flag", m2.value);

  // behind-the-scenes object model
  setText("i-version", state.meta.version || "--");
  setText("i-features", state.meta.features || "--");
  setText("i-blt", state.meta.blt || "--");
  setText("i-next", state.meta.next_ts || "--");
  setText("i-dataset", state.meta.dataset);
  setText("i-ts", state.meta.transferset);
}

// Translate the protocol's update-trigger code into plain words.
function reason(c) {
  if (c === null || c === undefined) return "--";
  const parts = [];
  if (c & 0x02) parts.push("scheduled refresh");
  if (c & 0x04) parts.push("a value changed");
  if (c & 0x01) parts.push("timed update");
  return parts.length ? parts.join(", ") : "update";
}

// ---- control actions ------------------------------------------------------

async function control(body) {
  try {
    await fetch("/api/control", {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
    });
  } catch (e) { /* the live feed will resync */ }
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
    conn.textContent = "reconnecting..."; conn.className = "link-pill down";
  };
}

document.addEventListener("DOMContentLoaded", init);
