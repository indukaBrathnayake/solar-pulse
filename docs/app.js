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

const STALE_MS = 60 * 1000;          // monitor counted offline after 60 s silence
const AMP_DEADBAND = 0.25;           // below this = idle
const PV_HOURS = [6, 19];            // charging inside this window = PV
const CAPACITY_AH = 100;             // pack capacity, fixed system spec

firebase.initializeApp(FIREBASE_CONFIG);
const db = firebase.database();

const $ = (id) => document.getElementById(id);
let live = null;
let dailyCache = null;               // all /daily rows, refreshed periodically

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

async function loadDay(dateStr) {
  const snap = await db.ref("history/" + dateStr).limitToLast(500).once("value");
  const labels = [], data = [];
  const rows = [];
  snap.forEach((c) => {
    const s = c.val();
    const t = new Date(s.t * 1000);
    labels.push(t.toLocaleTimeString("en-GB", { hour: "2-digit", minute: "2-digit" }));
    data.push(s.p);
    rows.push({ time: t.toISOString(), voltage: s.v, current: s.i, power: s.p, soc: s.soc });
  });
  setChartType("line", labels, [{
    data, borderColor: "#F2A20C", borderWidth: 2, pointRadius: 0,
    fill: { target: "origin", above: "rgba(242,162,12,0.10)", below: "rgba(30,140,125,0.10)" },
    tension: 0.3,
  }]);
  currentRows = { kind: "day", date: dateStr, columns: ["time","voltage","current","power","soc"], rows };
  $("chart-caption").textContent = `Power in/out of the battery on ${dateStr}`;
}

function ensureDailyCache() {
  if (dailyCache) return Promise.resolve(dailyCache);
  return db.ref("daily").limitToLast(400).once("value").then((snap) => {
    const rows = [];
    snap.forEach((c) => rows.push({ date: c.key, ...(c.val() || {}) }));
    dailyCache = rows;
    return rows;
  });
}

async function loadMonth(monthStr) {           // "YYYY-MM"
  const rows = (await ensureDailyCache()).filter((r) => r.date.startsWith(monthStr));
  const labels = rows.map((r) => r.date.slice(8));
  const harvested = rows.map((r) => (r.chgWh || 0) / 1000);
  const used = rows.map((r) => (r.disWh || 0) / 1000);
  setChartType("bar", labels, [
    { label: "Harvested", data: harvested, backgroundColor: "rgba(242,162,12,0.8)", borderRadius: 4 },
    { label: "Used", data: used, backgroundColor: "rgba(30,140,125,0.8)", borderRadius: 4 },
  ]);
  chart.options.plugins.legend.display = true;
  chart.update();
  currentRows = { kind: "month", date: monthStr,
    columns: ["date","harvested_kWh","used_kWh"],
    rows: rows.map((r) => ({ date: r.date, harvested_kWh: ((r.chgWh||0)/1000).toFixed(3), used_kWh: ((r.disWh||0)/1000).toFixed(3) })) };
  const total = harvested.reduce((a,b)=>a+b,0);
  $("chart-caption").textContent = `Daily harvest for ${monthStr} · ${total.toFixed(1)} kWh total`;
}

async function loadYear(yearStr) {              // "YYYY"
  const rows = (await ensureDailyCache()).filter((r) => r.date.startsWith(yearStr));
  const byMonth = {};
  rows.forEach((r) => {
    const m = r.date.slice(0, 7);
    if (!byMonth[m]) byMonth[m] = { chg: 0, dis: 0 };
    byMonth[m].chg += (r.chgWh || 0) / 1000;
    byMonth[m].dis += (r.disWh || 0) / 1000;
  });
  const months = Object.keys(byMonth).sort();
  const labels = months.map((m) => m.slice(5));
  const harvested = months.map((m) => byMonth[m].chg);
  const used = months.map((m) => byMonth[m].dis);
  setChartType("bar", labels, [
    { label: "Harvested", data: harvested, backgroundColor: "rgba(242,162,12,0.8)", borderRadius: 4 },
    { label: "Used", data: used, backgroundColor: "rgba(30,140,125,0.8)", borderRadius: 4 },
  ]);
  chart.options.plugins.legend.display = true;
  chart.update();
  currentRows = { kind: "year", date: yearStr,
    columns: ["month","harvested_kWh","used_kWh"],
    rows: months.map((m) => ({ month: m, harvested_kWh: byMonth[m].chg.toFixed(3), used_kWh: byMonth[m].dis.toFixed(3) })) };
  const total = harvested.reduce((a,b)=>a+b,0);
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

  const rows = (await ensureDailyCache()).filter((r) => r.date.startsWith(year));
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
db.ref("live").on("value", (snap) => { live = snap.val(); render(); });
setInterval(render, 3000);             // staleness watchdog, faster tick for a live feel
refreshChart();
setInterval(() => {
  dailyCache = null;
  refreshChart();
  if (!$("tab-monthly").classList.contains("hidden")) loadMonthlyTab();
}, 30 * 60 * 1000);

/* redraw the monthly bars when the theme toggle flips the palette */
new MutationObserver(() => {
  if (monthlyChart && !$("tab-monthly").classList.contains("hidden")) loadMonthlyTab();
}).observe(document.documentElement, { attributes: true, attributeFilter: ["data-theme"] });
