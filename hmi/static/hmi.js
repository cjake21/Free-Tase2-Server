"use strict";

// Tie-line power flow above this (MW) is a high-limit alarm condition. The lab
// server reports the authoritative value in state.lab.threshold.
let THRESH_MW = 100;

let state = null;
const events = [];                 // {t, k, text}, newest first
const acked = new Set();           // acknowledged alarm ids
let lastReportSeen = null;         // for event-log report detection
let prevAlarmIds = new Set();      // for new-alarm event detection

// ---- helpers --------------------------------------------------------------

function setText(id, txt) { const e = document.getElementById(id); if (e) e.textContent = txt; }
function setV(id, txt, cls) { const e = document.getElementById(id); if (e) { e.textContent = txt; e.className = "v" + (cls ? " " + cls : ""); } }
function fmt(v, d) { return (v === null || v === undefined) ? "--" : Number(v).toFixed(d); }
function nowUTC() { return new Date().toISOString().slice(11, 19) + "Z"; }
function fmtReport(s) {
  if (!s || s.length < 14) return "--";
  return s.slice(8, 10) + ":" + s.slice(10, 12) + ":" + s.slice(12, 14) + "Z";
}
function reason(c) {
  if (c === null || c === undefined) return "--";
  const p = [];
  if (c & 0x02) p.push("INTEGRITY");
  if (c & 0x04) p.push("CHANGE");
  if (c & 0x01) p.push("INTERVAL");
  return p.length ? p.join("+") : String(c);
}

// ---- one-line mimic (hero) ------------------------------------------------

function buildMimic() {
  return `
  <svg viewBox="0 0 920 220" role="img" aria-label="tie-line one-line diagram">
    <rect class="bound" x="14" y="24" width="420" height="176"></rect>
    <text class="bound-lbl" x="24" y="42">LOCAL STATION A</text>
    <rect class="bound" x="486" y="24" width="420" height="176"></rect>
    <text class="bound-lbl" x="896" y="42" text-anchor="end">REMOTE STATION B</text>

    <!-- generator -->
    <circle class="gen en" id="m-gen" cx="66" cy="120" r="20"></circle>
    <path class="gen en" id="m-genw" d="M56 120 q5 -8 10 0 q5 8 10 0"></path>
    <text class="eqlbl" x="66" y="154" text-anchor="middle">GEN</text>
    <line class="eq en" id="e-gw" x1="86" y1="120" x2="150" y2="120"></line>

    <!-- local bus -->
    <line class="busbar en" id="e-busA" x1="150" y1="84" x2="150" y2="156"></line>
    <text class="eqlbl" x="150" y="76" text-anchor="middle">BUS A</text>
    <text class="eqval" id="m-voltA" x="160" y="100">-- kV</text>
    <line class="eq en" id="e-w1" x1="150" y1="120" x2="250" y2="120"></line>

    <!-- breaker -->
    <rect class="brk-sym" id="m-brk" x="250" y="107" width="26" height="26"></rect>
    <text class="eqlbl" x="263" y="150" text-anchor="middle">DEV1</text>
    <text class="eqval" id="m-brkstate" x="263" y="100" text-anchor="middle" font-size="10">--</text>

    <!-- tie-line conductor with integrated flow readout -->
    <line class="eq en" id="m-tie" x1="276" y1="120" x2="660" y2="120"></line>
    <rect class="flowbox en" id="m-flowbox" x="408" y="104" width="98" height="32"></rect>
    <text class="eqlbl" x="457" y="116" text-anchor="middle">TIE FLOW</text>
    <text class="eqval" id="m-flowval" x="457" y="130" text-anchor="middle">-- MW</text>
    <polygon class="flowarrow en" id="m-flowarrow" points="514,114 528,120 514,126"></polygon>

    <!-- remote bus + demand -->
    <line class="busbar rx" id="e-busB" x1="660" y1="84" x2="660" y2="156"></line>
    <text class="eqlbl" x="660" y="76" text-anchor="middle">BUS B</text>
    <text class="eqval rx" id="m-voltB" x="670" y="100">-- kV</text>
    <line class="eq rx" id="e-rw" x1="660" y1="120" x2="770" y2="120"></line>
    <polygon class="load-sym rx" id="m-load" points="762,120 778,120 770,138"></polygon>
    <text class="eqlbl" x="770" y="156" text-anchor="middle">LOAD</text>
  </svg>`;
}

function setMimicClass(id, base, mod) {
  const e = document.getElementById(id);
  if (e) e.setAttribute("class", base + (mod ? " " + mod : ""));
}

// ---- state -> view --------------------------------------------------------

function currentA(p) {
  const pt = state.points[p];
  if (pt && pt.mode === "manual" && pt.setpoint !== null) return pt.setpoint;
  return state.stationB[p];
}

function render() {
  if (!state) return;
  if (state.lab && typeof state.lab.threshold === "number") THRESH_MW = state.lab.threshold;

  const A = !!(state.online && state.online.A), B = !!(state.online && state.online.B);
  const up = A && B;
  const b = state.stationB;
  const upd = fmtReport(b.last_report_time);

  // header status block
  setText("sys-sub", `DOMAIN ${state.server.domain}  |  BLT ${state.meta.blt || "--"}  |  SES ${state.server.host}:${state.server.port}`);
  setV("st-link", up ? "NORMAL" : (A || B ? "DEGRADED" : "OFFLINE"), up ? "st-ok" : (A || B ? "st-warn" : "st-bad"));
  const scanning = up && b.report_count > 0;
  setV("st-scan", scanning ? "RUN" : "HOLD", scanning ? "st-ok" : "st-warn");
  setV("st-rem", B ? "ONLINE" : "OFFLINE", B ? "st-rx" : "st-bad");
  setV("st-upd", upd, null);

  // local truth (operator setpoint if manual, else current server value)
  const lflow = currentA("tm1"), lvolt = currentA("tm2"), lclosed = currentA("ts1") === 1;
  const over = (lflow !== null && lflow !== undefined && Math.abs(lflow) > THRESH_MW);
  const flowManual = state.points.tm1 && state.points.tm1.mode === "manual";
  const flowMod = over ? "alarm" : (flowManual ? "manual" : (lclosed ? "en" : ""));

  // mimic: local side energization
  ["m-gen", "m-genw"].forEach(id => setMimicClass(id, "gen", lclosed ? "en" : ""));
  ["e-gw", "e-w1", "m-tie"].forEach(id => setMimicClass(id, "eq", lclosed ? "en" : ""));
  setMimicClass("e-busA", "busbar", lclosed ? "en" : "");
  setMimicClass("m-brk", "brk-sym", lclosed ? "closed" : "open");
  setText("m-brkstate", lclosed ? "CLOSED" : "OPEN");
  setText("m-voltA", fmt(lvolt, 1) + " kV");
  document.getElementById("m-voltA").setAttribute("class", "eqval" + (state.points.tm2 && state.points.tm2.mode === "manual" ? " manual" : ""));
  document.getElementById("m-flowval").setAttribute("class", "eqval" + (flowMod && flowMod !== "en" ? " " + flowMod : ""));
  setText("m-flowval", fmt(lflow, 1) + " MW");
  setMimicClass("m-flowbox", "flowbox", flowMod);
  const arr = document.getElementById("m-flowarrow");
  if (arr) {
    const pos = (lflow || 0) >= 0;
    arr.setAttribute("points", pos ? "514,114 528,120 514,126" : "528,114 514,120 528,126");
    arr.setAttribute("class", "flowarrow" + (over ? " alarm" : (lclosed ? " en" : "")));
  }

  // mimic: remote side reflects received link/values
  ["e-busB"].forEach(id => setMimicClass(id, "busbar", B ? "rx" : ""));
  ["e-rw"].forEach(id => setMimicClass(id, "eq", B ? "rx" : ""));
  setMimicClass("m-load", "load-sym", B ? "rx" : "");
  setText("m-voltB", fmt(b.tm2, 1) + " kV");
  document.getElementById("m-voltB").setAttribute("class", "eqval" + (B ? " rx" : ""));

  // local control cells
  cell("tm1", "MW"); cell("tm2", "kV");
  setText("c-ts1-mode", (state.points.ts1 && state.points.ts1.mode === "manual") ? "MANUAL" : "AUTO");
  setV("c-ts1-state", lclosed ? "CLOSED" : "OPEN", lclosed ? "en" : "alarm");
  setV("c-ts1-fb", b.ts1 === 1 ? "CLOSED" : (b.ts1 === 0 ? "OPEN" : "--"), b.ts1 === 1 ? "en" : "");
  setText("local-mode", anyManual() ? "MANUAL" : "AUTO");
  document.getElementById("local-mode").className = "badge" + (anyManual() ? " " : "");

  // remote received panel
  const disc = (lflow != null && b.tm1 != null && Math.abs(lflow - b.tm1) > 0.5);
  setV("r-flow", fmt(b.tm1, 1) + " MW", over ? "alarm" : "cyan");
  setV("r-volt", fmt(b.tm2, 1) + " kV", "cyan");
  setV("r-brk", b.ts1 === 1 ? "CLOSED" : (b.ts1 === 0 ? "OPEN" : "--"), b.ts1 === 1 ? "en" : "");
  setV("r-rptstate", b.report_count > 0 ? "ENABLED" : "IDLE", b.report_count > 0 ? "en" : "off");
  setV("r-last", upd, null);
  setV("r-count", String(b.report_count), null);
  setV("r-reason", reason(b.cond), null);
  setV("r-qual", up ? "GOOD" : "STALE", up ? "en" : "alarm");
  setV("r-link", B ? "ONLINE" : "OFFLINE", B ? "cyan" : "alarm");
  setV("r-disc", disc ? "FLOW DELTA" : "NONE", disc ? "manual" : "off");
  setV("remote-link", B ? "ONLINE" : "OFFLINE", null);
  document.getElementById("remote-link").className = "badge" + (B ? " cyan" : "");

  // reference drawer object model
  setText("i-version", state.meta.version || "--");
  setText("i-features", state.meta.features || "--");
  setText("i-blt", state.meta.blt || "--");
  setText("i-next", state.meta.next_ts || "--");
  setText("i-dataset", state.meta.dataset || "--");
  setText("i-ts", state.meta.transferset || "--");

  // lab validation marker (present only in the lab build)
  const m2 = state.lab && state.lab.marker2;
  const mEl = document.getElementById("marker2");
  if (mEl) {
    const revealed = !!(m2 && m2.revealed && m2.value);
    mEl.classList.toggle("hidden", !revealed);
    if (revealed) setText("marker2-flag", m2.value);
  }

  // alarms + events derived from the new state
  const alarms = buildAlarms(b, over, up, upd);
  renderAlarms(alarms);
  detectEvents(b, alarms);
}

function cell(item, unit) {
  const pt = state.points[item] || { mode: "auto" };
  setV(`c-${item}-mode`, pt.mode === "manual" ? "MANUAL" : "AUTO", pt.mode === "manual" ? "manual" : "");
  const local = currentA(item);
  setV(`c-${item}-fb`, fmt(local, 1) + " " + unit, pt.mode === "manual" ? "manual" : "");
  setText(`c-${item}-rcv`, fmt(state.stationB[item], 1) + " " + unit);
  const inp = document.getElementById(`in-${item}`);
  if (inp && document.activeElement !== inp) {
    inp.placeholder = (local === null || local === undefined) ? `SETPOINT  ${unit}` : Number(local).toFixed(1) + " " + unit;
  }
}

function anyManual() {
  return ["tm1", "tm2", "ts1"].some(p => state.points[p] && state.points[p].mode === "manual");
}

// ---- alarms ---------------------------------------------------------------

function buildAlarms(b, over, up, upd) {
  const a = [];
  if (over) a.push({ id: "ALM-102", cond: "TIE FLOW HIGH", val: fmt(b.tm1, 1) + " MW", lim: "LIMIT " + THRESH_MW + " MW" });
  if (!up) a.push({ id: "ALM-207", cond: "REMOTE LINK OFFLINE", val: "", lim: "LAST " + upd });
  return a;
}

function renderAlarms(alarms) {
  const ids = new Set(alarms.map(a => a.id));
  [...acked].forEach(id => { if (!ids.has(id)) acked.delete(id); });   // re-arm cleared alarms

  const host = document.getElementById("alarm-rows");
  if (!alarms.length) {
    host.innerHTML = '<div class="alarm-none">NO ACTIVE ALARMS</div>';
  } else {
    host.innerHTML = alarms.map(a => {
      const ackd = acked.has(a.id);
      return `<div class="alarm-row active">
        <span class="id">${a.id}</span>
        <span class="cond">${a.cond}</span>
        <span>${a.val}</span>
        <span>${a.lim}</span>
        <span class="ack ${ackd ? "ackd" : ""}">${ackd ? "ACK" : "UNACK"}</span>
      </div>`;
    }).join("");
  }
  const unack = alarms.filter(a => !acked.has(a.id)).length;
  setV("st-alm", String(alarms.length), unack ? "st-bad" : (alarms.length ? "st-warn" : null));
}

// ---- event log ------------------------------------------------------------

function pushEvent(kind, text) {
  events.unshift({ t: nowUTC(), k: kind, text });
  if (events.length > 16) events.pop();
  renderEvents();
}

function renderEvents() {
  const host = document.getElementById("event-rows");
  if (!events.length) { host.innerHTML = '<div class="event-none">NO EVENTS</div>'; return; }
  host.innerHTML = events.map(e =>
    `<div class="event-row"><span class="et">${e.t}</span><span class="ek ${e.k.toLowerCase()}">${e.k}</span><span>${e.text}</span></div>`
  ).join("");
}

function detectEvents(b, alarms) {
  if (b.last_report_time && b.last_report_time !== lastReportSeen) {
    lastReportSeen = b.last_report_time;
    pushEvent("RX", `REPORT ${reason(b.cond)}  TM1 ${fmt(b.tm1, 1)} MW  TM2 ${fmt(b.tm2, 1)} kV`);
  }
  const ids = new Set(alarms.map(a => a.id));
  alarms.forEach(a => { if (!prevAlarmIds.has(a.id)) pushEvent("ALM", `${a.id} ${a.cond} ${a.val}`); });
  prevAlarmIds = ids;
}

// ---- controls -------------------------------------------------------------

async function control(body) {
  try {
    await fetch("/api/control", {
      method: "POST", headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
    });
  } catch (e) { /* feed will resync */ }
}

function wireControls() {
  document.querySelectorAll("button[data-act]").forEach(btn => {
    btn.addEventListener("click", () => {
      const act = btn.dataset.act, item = btn.dataset.item;
      if (act === "set") {
        const inp = document.getElementById(`in-${item}`);
        if (inp.value !== "") {
          control({ action: "set", item, value: parseFloat(inp.value) });
          pushEvent("CMD", `SET ${item.toUpperCase()} = ${parseFloat(inp.value)}`);
        }
        inp.value = "";
      } else if (act === "release") {
        control({ action: "release", item });
        pushEvent("CMD", `RETURN ${item.toUpperCase()} AUTO`);
      } else if (act === "close") {
        control({ action: "breaker", closed: true });
        pushEvent("CMD", "CLOSE DEV1 (TIE BREAKER)");
      } else if (act === "open") {
        control({ action: "breaker", closed: false });
        pushEvent("CMD", "OPEN DEV1 (TIE BREAKER)");
      }
    });
  });
  document.getElementById("ackbtn").addEventListener("click", () => {
    document.querySelectorAll(".alarm-row .id").forEach(e => acked.add(e.textContent));
    render();
  });
}

// ---- boot -----------------------------------------------------------------

function init() {
  document.getElementById("mimic-svg").innerHTML = buildMimic();
  renderEvents();
  wireControls();
  setText("st-clock", nowUTC());
  setInterval(() => setText("st-clock", nowUTC()), 1000);

  fetch("/api/state").then(r => r.json()).then(s => { state = s; render(); });

  const es = new EventSource("/api/events");
  es.onmessage = ev => { try { state = JSON.parse(ev.data); render(); } catch (e) {} };
  es.onerror = () => { setV("st-link", "OFFLINE", "st-bad"); };
}

document.addEventListener("DOMContentLoaded", init);
