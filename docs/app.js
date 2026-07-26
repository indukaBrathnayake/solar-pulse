/* ============================================================
   Solar Pulse dashboard
   1) Paste your Firebase web config below (Console > Project
      settings > Your apps > Web app > Config).
   2) Nothing else needs editing.
   ============================================================ */
const FIREBASE_CONFIG = {
  apiKey: "AIzaSyD7_JShDQoGrB0V6A_6Zvd7O3DbpueUhWg",
  authDomain: "solar-pulse-8ba33.firebaseapp.com",
  databaseURL: "https://solar-pulse-8ba33-default-rtdb.asia-southeast1.firebasedatabase.app",
  projectId: "solar-pulse-8ba33",
};

const STALE_MS = 90 * 1000;          // monitor counted offline after 90 s silence
const AMP_DEADBAND = 0.25;           // below this = idle
const PV_HOURS = [6, 19];            // charging inside this window = PV

firebase.initializeApp(FIREBASE_CONFIG);
const db = firebase.database();

const $ = (id) => document.getElementById(id);
let live = null;

/* ---------------- number count-up ---------------- */
function tween(el, target, decimals = 2) {
  const from = parseFloat(el.dataset.v || "0");
  const t0 = performance.now(), dur = 700;
  function step(t) {
    const k = Math.min(1, (t - t0) / dur);
    const v = from + (target - from) * (1 - Math.pow(1 - k, 3));
    el.textContent = v.toFixed(decimals);
    if (k < 1) requestAnimationFrame(step);
    else el.dataset.v = target;
  }
  requestAnimationFrame(step);
}

/* ---------------- pill helper ---------------- */
function setPill(id, state, label) {
  const p = $(id);
  p.classList.remove("on", "gold", "warn");
  if (state) p.classList.add(state);
  p.childNodes[1].textContent = label;
}

/* ---------------- main render ---------------- */
function render() {
  if (!live) return;
  const now = Date.now();
  const ageMs = now - live.ts * 1000;
  const fresh = ageMs < STALE_MS;
  const hour = new Date().getHours();

  /* -- top status -- */
  document.body.classList.toggle("live", fresh);
  $("river").classList.toggle("stale", !fresh);
  $("offline-banner").classList.toggle("hidden", fresh);

  const seen = new Date(live.ts * 1000);
  const seenTxt = seen.toLocaleString("en-GB", { hour12: false });
  $("last-seen").textContent = seenTxt + (fresh ? " (live)" : "");
  $("last-seen-banner").textContent = seenTxt;

  setPill("pill-esp", fresh ? "on" : "warn", fresh ? "Monitor online" : "Monitor offline");
  setPill("pill-bms", fresh && live.bmsLink ? "on" : "warn",
          live.bmsLink ? "BMS linked" : "BMS lost");

  /* -- flow states (from net battery current) -- */
  const i = live.i ?? 0;
  const charging = i > AMP_DEADBAND;
  const discharging = i < -AMP_DEADBAND;
  const daylight = hour >= PV_HOURS[0] && hour < PV_HOURS[1];
  const pvActive = fresh && charging && daylight;
  const gridSuspect = fresh && charging && !daylight;
  const loadActive = fresh && discharging;

  $("river").classList.toggle("pv-on", pvActive || gridSuspect);
  $("river").classList.toggle("load-on", loadActive);

  setPill("pill-pv",
    pvActive ? "gold" : "",
    pvActive ? "PV generating" : (gridSuspect ? "Charging (grid?)" : (daylight ? "PV idle" : "Night")));
  setPill("pill-load", loadActive ? "on" : "", loadActive ? "Inverter feeding load" : "Load idle");
  setPill("pill-bal", live.bal ? "gold" : "", live.bal ? "Balancing" : "Balancer idle");

  const alarm = (live.err ?? 0) !== 0;
  $("pill-alarm").classList.toggle("hidden", !alarm);
  if (alarm) setPill("pill-alarm", "warn", "BMS alarm 0x" + live.err.toString(16));

  /* -- river numbers -- */
  const w = Math.abs(live.p ?? 0);
  $("pv-watts").textContent = (charging ? w.toFixed(0) : "0") + " W";
  $("pv-state").textContent = pvActive ? "generation running" :
      gridSuspect ? "charging after dark" : daylight ? "generation stopped" : "sun is down";
  $("soc-big").textContent = (live.soc ?? "--") + "%";
  $("pack-vi").textContent = `${(live.v ?? 0).toFixed(2)} V · ${i.toFixed(2)} A`;
  $("batt-state").textContent = charging ? "charging" : discharging ? "discharging" : "resting";
  $("load-state").textContent = loadActive ? `drawing ${w.toFixed(0)} W` : "standby";

  /* -- battery liquid -- */
  const soc = Math.max(0, Math.min(100, live.soc ?? 0));
  const h = 84 * soc / 100;
  const fill = $("batt-fill");
  fill.setAttribute("y", 42 - h);
  fill.setAttribute("height", h);
  $("batt-wave").setAttribute("transform", `translate(0 ${42 - h})`);

  /* -- cards -- */
  tween($("today-chg"), (live.todayChg ?? 0) / 1000, 2);
  tween($("today-dis"), (live.todayDis ?? 0) / 1000, 2);
  tween($("peak-w"), live.peakW ?? 0, 0);
  tween($("life-chg"), (live.lifeChg ?? 0) / 1000, 1);
  tween($("life-dis"), (live.lifeDis ?? 0) / 1000, 1);
  tween($("rem-ah"), live.remAh ?? 0, 1);
  $("soh").textContent = live.soh ?? "--";
  $("cycles").textContent = live.cyc ?? "--";
  $("mos-line").textContent =
    `MOS chg ${live.chgMos ? "ON" : "OFF"} / dis ${live.disMos ? "ON" : "OFF"}`;

  /* -- cells -- */
  const cells = [live.c1, live.c2, live.c3, live.c4];
  const vMin = 2.9, vMax = 3.55;
  cells.forEach((v, k) => {
    if (v == null) return;
    $("cv" + (k + 1)).textContent = v.toFixed(3) + " V";
    const pct = Math.max(4, Math.min(100, (v - vMin) / (vMax - vMin) * 100));
    $("cf" + (k + 1)).style.height = pct + "%";
  });
  const delta = (Math.max(...cells) - Math.min(...cells)) * 1000;
  $("delta-mv").textContent = isFinite(delta) ? delta.toFixed(0) : "--";
  $("t1").textContent = live.t1 ?? "--";
  $("t2").textContent = live.t2 ?? "--";
  $("mos-t").textContent = live.mosT ?? "--";
  $("bal-state").textContent = live.bal ? `ON ${(live.balI ?? 0).toFixed(2)} A` : "idle";

  /* -- ESP buffer note -- */
  const buffered = live.buffered ?? 0;
  $("buffered-note").classList.toggle("hidden", buffered === 0);
  $("buffered-n").textContent = buffered;
}

/* ---------------- charts ---------------- */
Chart.defaults.color = "#7C90AB";
Chart.defaults.borderColor = "rgba(36,54,92,0.6)";
Chart.defaults.font.family = "'IBM Plex Mono', monospace";

const todayChart = new Chart($("chart-today"), {
  type: "line",
  data: { labels: [], datasets: [{
    data: [], borderColor: "#F4B942", borderWidth: 2, pointRadius: 0,
    fill: { target: "origin", above: "rgba(244,185,66,0.10)", below: "rgba(70,181,173,0.12)" },
    tension: 0.3,
  }]},
  options: {
    animation: false, plugins: { legend: { display: false } },
    scales: {
      x: { ticks: { maxTicksLimit: 8 } },
      y: { title: { display: true, text: "W" } },
    },
  },
});

const monthChart = new Chart($("chart-month"), {
  type: "bar",
  data: { labels: [], datasets: [{
    data: [], backgroundColor: "rgba(244,185,66,0.75)", borderRadius: 5,
  }]},
  options: {
    animation: false, plugins: { legend: { display: false } },
    scales: { y: { title: { display: true, text: "kWh" } } },
  },
});

function localDate(d = new Date()) {
  return d.toLocaleDateString("en-CA");            // YYYY-MM-DD
}

/* today power curve: last 400 samples of /history/<today> */
function watchToday() {
  db.ref("history/" + localDate()).limitToLast(400).on("value", (snap) => {
    const labels = [], data = [];
    snap.forEach((c) => {
      const s = c.val();
      labels.push(new Date(s.t * 1000).toLocaleTimeString("en-GB",
        { hour: "2-digit", minute: "2-digit" }));
      data.push(s.p);
    });
    todayChart.data.labels = labels;
    todayChart.data.datasets[0].data = data;
    todayChart.update();
  });
}

/* 30-day bars + running year total from /daily */
function watchDaily() {
  db.ref("daily").limitToLast(370).on("value", (snap) => {
    const year = String(new Date().getFullYear());
    const days = [];
    let yearWh = 0;
    snap.forEach((c) => {
      const v = c.val() || {};
      days.push({ d: c.key, wh: v.chgWh || 0 });
      if (c.key.startsWith(year)) yearWh += v.chgWh || 0;
    });
    const last30 = days.slice(-30);
    monthChart.data.labels = last30.map((x) => x.d.slice(5));
    monthChart.data.datasets[0].data = last30.map((x) => x.wh / 1000);
    monthChart.update();
    $("year-total").textContent = (yearWh / 1000).toFixed(1);
  });
}

/* ---------------- wire it up ---------------- */
db.ref("live").on("value", (snap) => { live = snap.val(); render(); });
setInterval(render, 10000);            // staleness watchdog, re-renders even if silent
watchToday();
watchDaily();
setInterval(() => { watchToday(); }, 3600 * 1000);  // re-bind after midnight
