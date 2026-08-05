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

// The ESP pushes /live every 3 s, so 30 s of silence is ten missed
// pushes -- comfortably a real outage rather than one dropped packet.
const STALE_MS = 30 * 1000;
const AMP_DEADBAND = 0.25;           // below this = idle
const PV_HOURS = [6, 19];            // charging inside this window = PV
const CAPACITY_AH = 100;             // pack capacity, fixed system spec

firebase.initializeApp(FIREBASE_CONFIG);
const db = firebase.database();

const $ = (id) => document.getElementById(id);
let live = null;
// /daily rows are cached per year by daysForYear(); see the data
// layer further down.

/* ---------------- number count-up ---------------- */
function tween(el, target, decimals = 2) {
  const from = parseFloat(el.dataset.v || "0");
  const t0 = performance.now(), dur = 500;
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

/* ---------------- relay chip helper ---------------- */
function chip(id, on, tone) {
  const el = $(id);
  if (!el) return;
  el.classList.remove("on", "warn", "gold");
  if (on) el.classList.add(tone);
}

function fmtHM(hours) {
  if (!isFinite(hours) || hours <= 0) return "--";
  const totalMin = Math.round(hours * 60);
  const h = Math.floor(totalMin / 60), m = totalMin % 60;
  return h > 0 ? `${h}h ${String(m).padStart(2, "0")}m` : `${m}m`;
}

/* ---------------- main render ---------------- */
function render() {
  if (!live) return;
  const now = Date.now();
  const ageMs = now - live.ts * 1000;
  const fresh = ageMs < STALE_MS;
  const hour = new Date().getHours();

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

  const w = Math.abs(live.p ?? 0);
  $("pv-watts").textContent = (charging ? w.toFixed(0) : "0") + " W";
  $("pv-state").textContent = pvActive ? "generating" :
      gridSuspect ? "charging after dark" : daylight ? "stopped" : "sun is down";
  $("load-watts").textContent = (loadActive ? w.toFixed(0) : "0") + " W";
  $("load-state").textContent = loadActive ? "drawing power" : "standby";

  $("soc-big").textContent = (live.soc ?? "--") + "%";
  $("pack-vi").textContent = `${(live.v ?? 0).toFixed(2)} V · ${i.toFixed(2)} A`;
  $("batt-state").textContent = charging ? "charging" : discharging ? "discharging" : "resting";

  const soc = Math.max(0, Math.min(100, live.soc ?? 0));
  $("batt-fill").style.height = soc + "%";

  /* time remaining */
  const remAh = live.remAh ?? 0;
  let hrs = null, label = "resting", pct = 0, color = "var(--teal)";
  if (charging) {
    hrs = (CAPACITY_AH - remAh) / i;
    label = `${fmtHM(hrs)} to full`;
    pct = Math.min(100, (1 - hrs / 10) * 100);
    color = "var(--amber)";
  } else if (discharging) {
    hrs = remAh / Math.abs(i);
    label = `${fmtHM(hrs)} to empty at this rate`;
    pct = Math.min(100, (hrs / 24) * 100);
    color = hrs < 2 ? "var(--coral)" : "var(--teal)";
  } else {
    label = "resting, no load or charge";
    pct = 0;
  }
  $("time-fill").style.width = (fresh ? pct : 0) + "%";
  $("time-fill").style.background = color;
  $("time-label").textContent = fresh ? label : "no recent data";

  /* ---------- source relays, travel mode ----------
     Sent by firmware v3. Older firmware simply omits these keys,
     in which case the card stays on "--" and nothing breaks. */
  const src = live.src || "none";
  const srcTxt = src === "solar" ? "Solar + battery"
               : src === "utility" ? "Utility · CEB"
               : "not switched";
  $("src-name").textContent = fresh && live.src ? srcTxt : "--";
  $("src-why").textContent = live.why
    ? (live.manual ? "manual · " : "automatic · ") + live.why
    : "controller not reporting";
  chip("chip-solar", fresh && live.relayS, "on");
  chip("chip-util",  fresh && live.relayU, "warn");
  chip("chip-light", fresh && live.light,  "gold");
  $("util-min").textContent  = live.utilMin ?? 0;
  $("pv-now").textContent    = (live.pvW ?? 0).toFixed(0);
  $("load-now").textContent  = (live.loadW ?? 0).toFixed(0);

  /* ---------- tonight's plan ----------
     Decided by the ESP at 18:15: did the pack reach its 99% target
     today? If not it is a cloudy day, and the evening rule is allowed
     to run the pack down to the relaxed floor before touching CEB.
     Firmware older than this simply omits both keys, and the line
     hides itself rather than showing a wrong number. */
  const tn = $("tonight");
  if (live.nightFloor == null) {
    tn.classList.add("hidden");
  } else {
    tn.classList.remove("hidden");
    tn.classList.toggle("rainy", !!live.rainy);
    tn.textContent = live.rainy
      ? `tonight: 99% target missed, pack may run down to ${live.nightFloor}% before CEB`
      : `tonight: target reached, pack held above ${live.nightFloor}%`;
  }

  setPill("pill-src",
    !fresh || !live.src ? "" : src === "solar" ? "on" : src === "utility" ? "warn" : "",
    src === "solar" ? "On solar" : src === "utility" ? "On CEB" : "Source --");
  $("pill-travel").classList.toggle("hidden", !live.travel);
  if (live.travel) setPill("pill-travel", "gold", "Travel mode");

  tween($("today-chg"), (live.harvestWh ?? live.todayChg ?? 0) / 1000, 2);
  tween($("today-dis"), (live.todayDis ?? 0) / 1000, 2);
  tween($("peak-w"), live.peakW ?? 0, 0);
  tween($("life-chg"), (live.lifeChg ?? 0) / 1000, 1);
  tween($("life-dis"), (live.lifeDis ?? 0) / 1000, 1);
  tween($("rem-ah"), live.remAh ?? 0, 1);
  $("soh").textContent = live.soh ?? "--";
  $("cycles").textContent = live.cyc ?? "--";
  $("mos-line").textContent =
    `MOS chg ${live.chgMos ? "ON" : "OFF"} / dis ${live.disMos ? "ON" : "OFF"}`;

  const cells = [live.c1, live.c2, live.c3, live.c4];
  const vMin = 2.9, vMax = 3.55;
  cells.forEach((v, k) => {
    if (v == null) return;
    $("cv" + (k + 1)).textContent = v.toFixed(3) + " V";
    const pctC = Math.max(4, Math.min(100, (v - vMin) / (vMax - vMin) * 100));
    $("cf" + (k + 1)).style.height = pctC + "%";
  });
  const delta = (Math.max(...cells) - Math.min(...cells)) * 1000;
  $("delta-mv").textContent = isFinite(delta) ? delta.toFixed(0) : "--";
  $("t1").textContent = live.t1 ?? "--";
  $("t2").textContent = live.t2 ?? "--";
  $("mos-t").textContent = live.mosT ?? "--";
  $("bal-state").textContent = live.bal ? `ON ${(live.balI ?? 0).toFixed(2)} A` : "idle";

  const buffered = live.buffered ?? 0;
  $("buffered-note").classList.toggle("hidden", buffered === 0);
  $("buffered-n").textContent = buffered;
}

/* ---------------- chart ---------------- */
Chart.defaults.color = "#8B8175";
Chart.defaults.borderColor = "#ECE3D6";
Chart.defaults.font.family = "'IBM Plex Mono', monospace";

const chart = new Chart($("chart-main"), {
  type: "line",
  data: { labels: [], datasets: [{
    data: [], borderColor: "#F2A20C", borderWidth: 2, pointRadius: 0,
    fill: { target: "origin", above: "rgba(242,162,12,0.10)", below: "rgba(30,140,125,0.10)" },
    tension: 0.3,
  }]},
  options: {
    animation: { duration: 300 }, plugins: { legend: { display: false } },
    scales: { x: { ticks: { maxTicksLimit: 8 } }, y: { title: { display: true, text: "W" } } },
  },
});

function setChartType(type, labels, datasets) {
  chart.config.type = type;
  chart.data.labels = labels;
  chart.data.datasets = datasets;
  if (type === "line") {
    chart.options.scales.y.title.text = "W";
  } else {
    chart.options.scales.y.title.text = "kWh";
  }
  chart.update();
}

function localDate(d = new Date()) { return d.toLocaleDateString("en-CA"); }

let currentView = "day";
let currentRows = { labels: [], harvested: [], used: [] };  // for CSV export

/* ============================================================
   DAY VIEW - CIRCULAR SAMPLE BUFFER

   The old version read history once with .once("value") and only
   re-read it on a tab switch or the 30-minute timer, so the "live"
   graph was in fact a snapshot that sat still for half an hour.

   Now: a fixed-size ring keyed by unix timestamp.
     - seeded from /history/<date> on load
     - appended from the /live listener as values arrive (3 s)
     - appended from a child_added listener so the canonical
       one-minute samples and any backlog upload also land
     - deduplicated by timestamp, so nothing is ever counted twice
     - sorted only when a sample actually arrives out of order,
       which happens after an offline backlog is flushed
     - bounded at RING_MAX, oldest dropped: memory cannot grow and
       the window scrolls instead of accumulating forever
   ============================================================ */
const RING_MAX = 1200;

const ring = {
  rows: [],            // ascending by .t
  seen: new Set(),     // timestamps present, for O(1) dedupe
  date: null,          // which local date this ring holds
};

function ringReset(dateStr) {
  ring.rows.length = 0;
  ring.seen.clear();
  ring.date = dateStr;
}

function ringPush(s) {
  const ts = Number(s && s.t);
  if (!isFinite(ts) || ts <= 0 || ring.seen.has(ts)) return false;
  ring.seen.add(ts);
  const n = ring.rows.length;
  if (n && ts < ring.rows[n - 1].t) {
    ring.rows.push(s);
    ring.rows.sort((a, b) => a.t - b.t);      // rare: backlog arrived late
  } else {
    ring.rows.push(s);
  }
  while (ring.rows.length > RING_MAX) {
    ring.seen.delete(ring.rows.shift().t);
  }
  return true;
}

/* one rAF-coalesced redraw, so a burst of samples repaints once */
let drawQueued = false;
function queueDayDraw() {
  if (drawQueued || currentView !== "day") return;
  drawQueued = true;
  requestAnimationFrame(() => { drawQueued = false; drawDay(); });
}

function drawDay() {
  const labels = ring.rows.map((s) =>
    new Date(s.t * 1000).toLocaleTimeString("en-GB", { hour: "2-digit", minute: "2-digit" }));
  const gen = ring.rows.map((s) => (s.p > 0 ? s.p : 0));
  const use = ring.rows.map((s) => (s.p < 0 ? -s.p : 0));

  chart.config.type = "line";
  chart.options.scales.y.title.text = "W";
  chart.data.labels = labels;
  chart.data.datasets = [
    { label: "Generation (charging)", data: gen, borderWidth: 2, pointRadius: 0,
      fill: "origin", tension: 0.35 },
    { label: "Electricity use", data: use, borderWidth: 2, pointRadius: 0,
      fill: "origin", tension: 0.35 },
  ];
  chart.options.plugins.legend.display = true;
  chart.update("none");                       // no animation = no flicker

  currentRows = {
    kind: "day", date: ring.date,
    columns: ["time", "voltage", "current", "power", "soc"],
    rows: ring.rows.map((s) => ({
      time: new Date(s.t * 1000).toISOString(),
      voltage: s.v, current: s.i, power: s.p, soc: s.soc,
    })),
  };
  const live = ring.date === localDate();
  $("chart-caption").textContent = ring.rows.length
    ? `Power in/out of the battery on ${ring.date} · ${ring.rows.length} samples${live ? " · live" : ""}`
    : `No samples stored for ${ring.date} yet`;
}

/* child_added listener for the day currently on screen. Detached
   whenever the day changes so listeners cannot pile up -- that was
   the memory leak path.

   loadGen guards the await: spinning the date picker fires several
   loadDay() calls, and without it a slow earlier fetch would land
   its rows into the newer day's ring and attach a second listener. */
let dayRef = null;
let loadGen = 0;

function detachDayFeed() {
  if (dayRef) { dayRef.off(); dayRef = null; }
}

async function loadDay(dateStr) {
  const gen = ++loadGen;
  detachDayFeed();
  ringReset(dateStr);
  drawDay();                                   // paint empty immediately

  let snap;
  try {
    snap = await db.ref("history/" + dateStr).limitToLast(RING_MAX).once("value");
  } catch (e) {
    $("chart-caption").textContent = `Could not load ${dateStr}: ${e.message || e}`;
    return;
  }
  if (gen !== loadGen) return;                 // superseded while awaiting

  snap.forEach((c) => ringPush(c.val()));
  drawDay();

  // Only today keeps streaming; past days are static.
  if (dateStr === localDate()) {
    detachDayFeed();                           // belt and braces
    dayRef = db.ref("history/" + dateStr).orderByKey().limitToLast(1);
    dayRef.on("child_added", (c) => {
      if (gen !== loadGen || ring.date !== dateStr) return;
      if (ringPush(c.val())) queueDayDraw();
    });
  }
}

/* ============================================================
   DAILY TOTALS - QUERIED PER YEAR

   limitToLast(400) silently truncated anything older than ~13
   months, so a second year of history could never be totalled
   correctly. Key-range queries return exactly the year asked for,
   however far back it is.
   ============================================================ */
const dailyByYear = new Map();

function harvestKwh(r) { return (r.harvestWh ?? r.chgWh ?? 0) / 1000; }
function usedKwh(r)    { return (r.disWh ?? 0) / 1000; }

async function daysForYear(year) {
  const key = String(year);
  if (dailyByYear.has(key)) return dailyByYear.get(key);
  const snap = await db.ref("daily").orderByKey()
    .startAt(key + "-01-01").endAt(key + "-12-31").once("value");
  const rows = [];
  snap.forEach((c) => rows.push({ date: c.key, ...(c.val() || {}) }));
  dailyByYear.set(key, rows);
  return rows;
}

function invalidateDaily() { dailyByYear.clear(); }

async function loadMonth(monthStr) {           // "YYYY-MM"
  detachDayFeed();
  const rows = (await daysForYear(monthStr.slice(0, 4)))
    .filter((r) => r.date.startsWith(monthStr));
  const labels = rows.map((r) => r.date.slice(8));
  const harvested = rows.map(harvestKwh);
  const used = rows.map(usedKwh);
  setChartType("bar", labels, [
    { label: "Harvested", data: harvested, borderRadius: 4 },
    { label: "Used", data: used, borderRadius: 4 },
  ]);
  chart.options.plugins.legend.display = true;
  chart.update();
  currentRows = { kind: "month", date: monthStr,
    columns: ["date", "harvested_kWh", "used_kWh"],
    rows: rows.map((r) => ({ date: r.date,
      harvested_kWh: harvestKwh(r).toFixed(3), used_kWh: usedKwh(r).toFixed(3) })) };
  const total = harvested.reduce((a, b) => a + b, 0);
  $("chart-caption").textContent = `Daily harvest for ${monthStr} · ${total.toFixed(1)} kWh total`;
}

async function loadYear(yearStr) {              // "YYYY"
  detachDayFeed();
  const rows = await daysForYear(yearStr);
  // Always twelve buckets: a month with no data must read 0, not
  // vanish, or the bars silently shift along the axis.
  const chg = Array(12).fill(0), dis = Array(12).fill(0);
  rows.forEach((r) => {
    const m = parseInt(r.date.slice(5, 7), 10) - 1;
    if (m < 0 || m > 11) return;
    chg[m] += harvestKwh(r);
    dis[m] += usedKwh(r);
  });
  const labels = ["Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"];
  setChartType("bar", labels, [
    { label: "Harvested", data: chg, borderRadius: 4 },
    { label: "Used", data: dis, borderRadius: 4 },
  ]);
  chart.options.plugins.legend.display = true;
  chart.update();
  currentRows = { kind: "year", date: yearStr,
    columns: ["month", "harvested_kWh", "used_kWh"],
    rows: labels.map((name, i) => ({
      month: `${yearStr}-${String(i + 1).padStart(2, "0")}`,
      harvested_kWh: chg[i].toFixed(3), used_kWh: dis[i].toFixed(3) })) };
  const total = chg.reduce((a, b) => a + b, 0);
  $("chart-caption").textContent = `Monthly harvest for ${yearStr} · ${total.toFixed(1)} kWh total`;
}

function refreshChart() {
  if (currentView === "day") loadDay($("pick-day").value || localDate());
  else if (currentView === "month") loadMonth($("pick-month").value || localDate().slice(0, 7));
  else loadYear($("pick-year").value || String(new Date().getFullYear()));
}

/* ---------------- view switcher ---------------- */
document.querySelectorAll(".seg-btn").forEach((btn) => {
  btn.addEventListener("click", () => {
    document.querySelectorAll(".seg-btn").forEach((b) => b.classList.remove("active"));
    btn.classList.add("active");
    currentView = btn.dataset.view;
    $("pick-day").classList.toggle("hidden", currentView !== "day");
    $("pick-month").classList.toggle("hidden", currentView !== "month");
    $("pick-year").classList.toggle("hidden", currentView !== "year");
    chart.options.plugins.legend.display = currentView !== "day";
    refreshChart();
  });
});
$("pick-day").value = localDate();
$("pick-month").value = localDate().slice(0, 7);
$("pick-year").value = String(new Date().getFullYear());
$("pick-day").addEventListener("change", refreshChart);
$("pick-month").addEventListener("change", refreshChart);
$("pick-year").addEventListener("change", refreshChart);

/* ---------------- CSV export ---------------- */
function toCSV(columns, rows) {
  const esc = (v) => `"${String(v).replace(/"/g, '""')}"`;
  const head = columns.map(esc).join(",");
  const body = rows.map((r) => columns.map((c) => esc(r[c] ?? "")).join(",")).join("\n");
  return head + "\n" + body;
}
function downloadText(filename, text) {
  const blob = new Blob([text], { type: "text/csv;charset=utf-8" });
  const url = URL.createObjectURL(blob);
  const a = document.createElement("a");
  a.href = url; a.download = filename;
  document.body.appendChild(a); a.click(); a.remove();
  URL.revokeObjectURL(url);
}
$("btn-export").addEventListener("click", () => {
  if (!currentRows.rows || !currentRows.rows.length) return;
  const csv = toCSV(currentRows.columns, currentRows.rows);
  downloadText(`solar-pulse-${currentRows.kind}-${currentRows.date}.csv`, csv);
});

/* ============================================================
   MONTHLY TAB
   Built from /daily, the one-row-per-day log the ESP32 keeps in
   its own flash and mirrors to Firebase. Twelve bars, one per
   month of the chosen year, plus a table of the same numbers.
   ============================================================ */
const MONTH_NAMES = ["January","February","March","April","May","June",
                     "July","August","September","October","November","December"];
let monthlyChart = null;
let monthlyRows = null;

function themeColor(name) {
  return getComputedStyle(document.documentElement).getPropertyValue(name).trim();
}

async function loadMonthlyTab() {
  const year = String($("pick-myear").value || new Date().getFullYear());

  const rows = await daysForYear(year);
  const kwh = Array(12).fill(0), used = Array(12).fill(0), days = Array(12).fill(0);
  rows.forEach((r) => {
    const m = parseInt(r.date.slice(5, 7), 10) - 1;
    if (m < 0 || m > 11) return;
    // harvestWh is firmware v3; fall back to chgWh for older rows
    kwh[m]  += (r.harvestWh ?? r.chgWh ?? 0) / 1000;
    used[m] += (r.disWh || 0) / 1000;
    days[m] += 1;
  });

  const total = kwh.reduce((a, b) => a + b, 0);
  const totalUsed = used.reduce((a, b) => a + b, 0);
  const totalDays = days.reduce((a, b) => a + b, 0);
  let bestIdx = 0;
  kwh.forEach((v, i) => { if (v > kwh[bestIdx]) bestIdx = i; });

  /* ---- summary cards ---- */
  $("yr-kwh").textContent      = total.toFixed(1);
  $("yr-days").textContent     = totalDays;
  $("yr-best").textContent     = total > 0 ? MONTH_NAMES[bestIdx].slice(0, 3) : "--";
  $("yr-best-kwh").textContent = kwh[bestIdx].toFixed(1);
  $("yr-used").textContent     = totalUsed.toFixed(1);
  $("yr-days2").textContent    = totalDays;

  /* ---- bar chart ---- */
  const labels = MONTH_NAMES.map((m) => m.slice(0, 3));
  const solar = themeColor("--amber"), blue = themeColor("--use"), dim = themeColor("--muted");
  const grid = themeColor("--grid");
  const datasets = [
    { label: "Harvested", data: kwh,  backgroundColor: solar, borderRadius: 6 },
    { label: "Used",      data: used, backgroundColor: blue,  borderRadius: 6 },
  ];

  if (!monthlyChart) {
    monthlyChart = new Chart($("chart-monthly"), {
      type: "bar",
      data: { labels, datasets },
      options: {
        maintainAspectRatio: false,
        animation: { duration: 300 },
        plugins: { legend: { labels: { boxWidth: 14, boxHeight: 3, font: { size: 11 } } } },
        scales: { y: { beginAtZero: true, title: { display: true, text: "kWh" } } },
      },
    });
  } else {
    monthlyChart.data.labels = labels;
    monthlyChart.data.datasets = datasets;
  }
  const o = monthlyChart.options;
  o.scales.x.ticks = Object.assign(o.scales.x.ticks || {}, { color: dim });
  o.scales.y.ticks = Object.assign(o.scales.y.ticks || {}, { color: dim });
  o.scales.x.grid = Object.assign(o.scales.x.grid || {}, { color: grid });
  o.scales.y.grid = Object.assign(o.scales.y.grid || {}, { color: grid });
  o.scales.y.title.color = dim;
  o.plugins.legend.labels.color = dim;
  monthlyChart.update();

  /* ---- table ---- */
  $("mtbody").innerHTML =
    kwh.map((v, i) =>
      `<tr><td>${MONTH_NAMES[i]}</td><td>${v.toFixed(2)}</td>` +
      `<td>${used[i].toFixed(2)}</td><td>${days[i]}</td></tr>`).join("") +
    `<tr class="mtotal"><td>Total ${year}</td><td>${total.toFixed(2)}</td>` +
    `<td>${totalUsed.toFixed(2)}</td><td>${totalDays}</td></tr>`;

  $("monthly-caption").textContent =
    `Total kWh harvested each month of ${year} · ${total.toFixed(1)} kWh so far`;

  monthlyRows = {
    kind: "monthly", date: year,
    columns: ["month", "harvested_kWh", "used_kWh", "days_logged"],
    rows: kwh.map((v, i) => ({
      month: `${year}-${String(i + 1).padStart(2, "0")}`,
      harvested_kWh: v.toFixed(3),
      used_kWh: used[i].toFixed(3),
      days_logged: days[i],
    })),
  };
}

$("pick-myear").value = String(new Date().getFullYear());
$("pick-myear").addEventListener("change", loadMonthlyTab);
$("btn-export-month").addEventListener("click", () => {
  if (!monthlyRows) return;
  downloadText(`solar-pulse-monthly-${monthlyRows.date}.csv`,
               toCSV(monthlyRows.columns, monthlyRows.rows));
});

/* ---------------- bottom tab bar ---------------- */
document.querySelectorAll(".nav-btn").forEach((btn) => {
  btn.addEventListener("click", () => {
    document.querySelectorAll(".nav-btn").forEach((b) => b.classList.remove("active"));
    btn.classList.add("active");
    const tab = btn.dataset.tab;
    $("tab-live").classList.toggle("hidden", tab !== "live");
    $("tab-monthly").classList.toggle("hidden", tab !== "monthly");
    window.scrollTo({ top: 0, behavior: "smooth" });
    if (tab === "monthly") loadMonthlyTab();
    else if (chart) chart.resize();
  });
});

/* ---------------- wire it up ---------------- */
db.ref("live").on("value", (snap) => {
  live = snap.val();
  render();

  // Feed the live sample straight into the day ring. This is what
  // makes the graph actually live at the ESP's 3 s cadence instead
  // of waiting for the next one-minute history write.
  if (live && currentView === "day" && ring.date === localDate()) {
    if (ringPush({ t: live.ts, v: live.v, i: live.i, p: live.p, soc: live.soc })) {
      queueDayDraw();
    }
  }
});

setInterval(render, 3000);             // staleness watchdog

/* Midnight in the browser: roll the day view over to the new date
   so an overnight tab does not sit on yesterday forever. */
setInterval(() => {
  if (currentView !== "day") return;
  const today = localDate();
  if (ring.date && ring.date !== today && $("pick-day").value === ring.date) {
    $("pick-day").value = today;
    loadDay(today);
  }
}, 60 * 1000);

refreshChart();

setInterval(() => {
  invalidateDaily();
  if (currentView !== "day") refreshChart();
  if (!$("tab-monthly").classList.contains("hidden")) loadMonthlyTab();
}, 30 * 60 * 1000);

/* redraw the monthly bars when the theme toggle flips the palette */
new MutationObserver(() => {
  if (monthlyChart && !$("tab-monthly").classList.contains("hidden")) loadMonthlyTab();
}).observe(document.documentElement, { attributes: true, attributeFilter: ["data-theme"] });
