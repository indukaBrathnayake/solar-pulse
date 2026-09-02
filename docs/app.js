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

  /* ---------- CEB / weather plan ----------
     v5 replaces the old 99%-target "rainy" rule with the CEB state
     machine. Firmware that predates it omits live.ceb, in which case
     the line hides itself rather than showing a wrong number. */
  const tn = $("tonight");
  if (live.ceb == null) {
    tn.classList.add("hidden");
  } else {
    tn.classList.remove("hidden");
    const heavy = live.wx === "heavy";
    tn.classList.toggle("rainy", heavy);
    const wxTxt = live.wx === "clear" ? "clear day"
                : heavy               ? "poor-solar day"
                : live.wx === "half"  ? "mixed day"
                : "forecast unavailable";
    tn.textContent = live.ceb
      ? `CEB carrying the house · ${wxTxt} · handing back at ${live.cebRel} if the pack allows`
      : `on solar · ${wxTxt} · CEB engages at ${live.cebOn}%`;
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

  trackBacklog(live.buffered ?? 0);
}

/* ============================================================
   BACKLOG / BACKFILL TRACKING

   While WiFi is down the ESP keeps logging to its own flash. When
   it reconnects it replays that backlog into history/<date>/<ts>
   using the ORIGINAL timestamps, so the writes land in the middle
   of the day, not at the end.

   That is invisible to the day feed: it is orderByKey().limitToLast(1)
   + child_added, a window that only ever contains the newest key.
   Backfilled keys are older, fall outside the window, and never
   fire. The gap therefore stayed on screen until a manual reload.

   So watch the ESP's own queue depth instead. When it drains to
   zero after having been non-zero, the backlog is now in the cloud
   and every affected view is re-fetched from scratch.
   ============================================================ */
let backlogPrev = null;      // last seen depth, null = nothing seen yet
let backlogPeak = 0;         // high-water mark for this outage
let backfillTimer = null;

function trackBacklog(n) {
  const note = $("buffered-note");
  const wasDraining = backlogPrev !== null && backlogPrev > 0;

  if (n > backlogPeak) backlogPeak = n;

  // While the "backfilled N samples" message is showing, #buffered-n
  // does not exist. Leave the note alone until it is restored.
  const slot = $("buffered-n");
  if (slot) {
    note.classList.toggle("hidden", n === 0 && !backfillTimer);
    slot.textContent = n;
    note.classList.toggle("syncing", n > 0 && backlogPrev !== null && n < backlogPrev);
  }

  // Drained: the ESP has handed its whole backlog to Firebase.
  if (wasDraining && n === 0) scheduleBackfillRefresh(backlogPeak);

  backlogPrev = n;
  if (n === 0) backlogPeak = 0;
}

/* Debounced so a backlog that drains over several batches triggers
   one refresh at the end, not one per batch. */
function scheduleBackfillRefresh(count) {
  if (backfillTimer) clearTimeout(backfillTimer);
  const note = $("buffered-note");
  note.classList.remove("hidden");
  note.classList.add("syncing");
  $("buffered-n").textContent = 0;

  backfillTimer = setTimeout(async () => {
    backfillTimer = null;
    await applyBackfill(count);
  }, 2500);
}

async function applyBackfill(count) {
  // Day chart: full re-fetch. The incremental feed structurally
  // cannot see backdated keys, so re-read the whole day.
  if (currentView === "day") {
    await loadDay($("pick-day").value || localDate());
  }
  // Daily totals feed the month and year views and the Monthly tab.
  invalidateDaily();
  if (currentView !== "day") refreshChart();
  if (!$("tab-monthly").classList.contains("hidden")) loadMonthlyTab();

  const note = $("buffered-note");
  note.classList.remove("syncing");
  note.classList.add("done");
  note.innerHTML = `· backfilled ${count} sample${count === 1 ? "" : "s"} from the ESP`;
  setTimeout(() => {
    note.classList.add("hidden");
    note.classList.remove("done");
    note.innerHTML = '· <span id="buffered-n">0</span> samples waiting on the ESP';
  }, 8000);
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
  // The day view pins x to a linear 0-1440 minute axis. Month and
  // year are label-indexed bars, so the category axis has to be put
  // back or they inherit the fixed 24 h range and render nothing.
  chart.options.scales.x = {
    type: "category",
    ticks: { maxTicksLimit: 12, maxRotation: 0 },
  };
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
/* One slot per MINUTE of the day, which is exactly the resolution the
   ESP logs at. Two reasons this is bucketed rather than a plain
   timestamp set:

   1. The /live listener fires every LIVE_PUSH_MS (5 s). Keyed by raw
      timestamp that is 720 new points per hour, ~17000 a day, so the
      ring hit its cap by mid-morning and evicted the start of the day
      — the chart could not show a full day even in principle.
   2. Canonical history samples are ~60 s apart but NOT aligned to :00,
      so a live point and the history point for the same minute have
      different timestamps and would both be kept as duplicates.

   Bucketing by floor(t/60) fixes both: at most 1440 points per day,
   the whole day always fits, and history (canonical) overwrites the
   live estimate for the same minute when it arrives. */
const RING_MAX = 1600;                   // 1440 minutes + headroom

const ring = {
  rows: [],            // ascending by .t
  slot: new Map(),     // minute bucket -> row, for O(1) dedupe/upsert
  date: null,          // which local date this ring holds
};

function ringReset(dateStr) {
  ring.rows.length = 0;
  ring.slot.clear();
  ring.date = dateStr;
}

/* isLive: a 5 s estimate for the minute in progress. It may be
   replaced by a later live sample, and is always superseded by the
   canonical one-minute history sample for the same bucket. */
function ringPush(s, isLive) {
  const ts = Number(s && s.t);
  if (!isFinite(ts) || ts <= 0) return false;
  const bucket = Math.floor(ts / 60);

  const existing = ring.slot.get(bucket);
  if (existing) {
    // History always wins. A live sample only refreshes another live one.
    if (!isLive || existing.live) {
      Object.assign(existing, s, { live: !!isLive });
      return true;
    }
    return false;
  }

  const row = Object.assign({}, s, { live: !!isLive });
  ring.slot.set(bucket, row);
  const n = ring.rows.length;
  if (n && ts < ring.rows[n - 1].t) {
    ring.rows.push(row);
    ring.rows.sort((a, b) => a.t - b.t);      // rare: backlog arrived late
  } else {
    ring.rows.push(row);
  }
  while (ring.rows.length > RING_MAX) {
    const dropped = ring.rows.shift();
    ring.slot.delete(Math.floor(dropped.t / 60));
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

/* Minutes since local midnight of the day being displayed. This is
   the x coordinate for the day chart. */
function minutesOfDay(ts, dateStr) {
  const midnight = new Date(dateStr + "T00:00:00").getTime();   // local
  return (ts * 1000 - midnight) / 60000;
}

function hhmm(mins) {
  const h = Math.floor(mins / 60), m = Math.round(mins % 60);
  return String(h).padStart(2, "0") + ":" + String(m).padStart(2, "0");
}

function drawDay() {
  // A CATEGORY axis takes its width from however many samples exist,
  // so a half-empty day stretched a few morning readings across the
  // whole canvas and the axis moved every time a sample landed.
  // A fixed linear axis in minutes-of-day pins the chart to
  // 00:00-24:00 always: the curve grows into a stable frame instead
  // of the frame resizing around the curve.
  const gen = ring.rows.map((s) => ({ x: minutesOfDay(s.t, ring.date), y: s.p > 0 ? s.p : 0 }));
  const use = ring.rows.map((s) => ({ x: minutesOfDay(s.t, ring.date), y: s.p < 0 ? -s.p : 0 }));

  chart.config.type = "line";
  chart.options.scales.y.title.text = "W";
  chart.options.scales.y.beginAtZero = true;
  chart.data.labels = [];                      // linear axis: points carry x

  // Must be a PLAIN literal. chart.options is a Chart.js proxy, so
  // Object.assign({}, chart.options.scales.x, ...) copies the
  // resolver's own internals back into itself and the scriptable
  // resolver then recurses forever ("Recursion detected").
  chart.options.scales.x = {
    type: "linear",
    min: 0,
    max: 1440,                                 // 24 h, never rescaled
    offset: false,
    bounds: "ticks",
    ticks: {
      stepSize: 120,                           // a label every 2 hours
      autoSkip: false,
      maxRotation: 0,
      callback: (v) => hhmm(v),
    },
  };

  chart.data.datasets = [
    { label: "Generation (charging)", data: gen, borderWidth: 2, pointRadius: 0,
      fill: "origin", tension: 0.35, spanGaps: false },
    { label: "Electricity use", data: use, borderWidth: 2, pointRadius: 0,
      fill: "origin", tension: 0.35, spanGaps: false },
  ];
  chart.options.plugins.legend.display = true;
  chart.update("none");                       // no animation = no flicker

  // Same ring, same samples, second view of them.
  drawSoc();

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


/* ============================================================
   BATTERY SoC OVER TIME

   Reuses the SAME ring the generation chart is built from -- the
   ESP has always shipped `soc` inside every /history sample, so
   this needs no new data source, no new Firebase path and no
   firmware change to feed it.

   Truthful by construction: one point per stored sample, no
   interpolation, and spanGaps:false so a logging gap renders as a
   gap rather than a straight line pretending the pack was there.
   ============================================================ */
let socChart = null;

function drawSoc() {
  const pts = ring.rows
    .filter((s) => s.soc != null && isFinite(s.soc))
    .map((s) => ({ x: minutesOfDay(s.t, ring.date), y: Number(s.soc) }));

  const dim  = themeColor("--muted");
  const grid = themeColor("--grid");
  const tealC = themeColor("--teal");

  const ds = [{
    label: "Battery SoC",
    data: pts,
    borderColor: tealC,
    backgroundColor: tealC + "2E",
    borderWidth: 2,
    pointRadius: 0,
    fill: "origin",
    tension: 0.3,
    spanGaps: false,
  }];

  if (!socChart) {
    socChart = new Chart($("chart-soc"), {
      type: "line",
      data: { labels: [], datasets: ds },
      options: {
        maintainAspectRatio: false,
        animation: { duration: 0 },
        interaction: { mode: "index", intersect: false },
        plugins: {
          legend: { display: false },
          tooltip: {
            callbacks: {
              title: (items) => (items.length ? hhmm(items[0].parsed.x) : ""),
              label: (it) => ` ${Math.round(it.parsed.y)} %`,
            },
          },
        },
        scales: {
          x: {
            type: "linear", min: 0, max: 1440, offset: false, bounds: "ticks",
            ticks: { stepSize: 120, autoSkip: false, maxRotation: 0,
                     callback: (v) => hhmm(v) },
          },
          y: {
            min: 0, max: 100,
            ticks: { stepSize: 20, callback: (v) => v + "%" },
            title: { display: true, text: "SoC" },
          },
        },
      },
    });
  } else {
    socChart.data.datasets = ds;
  }

  const o = socChart.options;
  o.scales.x.ticks.color = dim;
  o.scales.y.ticks.color = dim;
  o.scales.x.grid = { color: grid };
  o.scales.y.grid = { color: grid };
  o.scales.y.title.color = dim;
  socChart.update("none");

  const el = $("soc-caption");
  if (!el) return;
  if (!pts.length) {
    el.textContent = `No state-of-charge samples stored for ${ring.date} yet`;
    return;
  }
  let lo = pts[0].y, hi = pts[0].y;
  for (const p of pts) { if (p.y < lo) lo = p.y; if (p.y > hi) hi = p.y; }
  el.textContent =
    `State of charge on ${ring.date} · now ${Math.round(pts[pts.length - 1].y)}%` +
    ` · low ${Math.round(lo)}% · high ${Math.round(hi)}%`;
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

  // NOTE: Firebase's DataSnapshot.forEach ABORTS as soon as the
  // callback returns a truthy value. ringPush returns true on a
  // successful insert, so `forEach(c => ringPush(c.val()))` loaded
  // exactly ONE sample and silently discarded the rest of the day.
  // The braces matter: this callback must return undefined.
  snap.forEach((c) => { ringPush(c.val()); });
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
  // Same trap as loadDay: Array.prototype.push returns the new LENGTH,
  // which is >= 1 and therefore truthy, so Firebase cancelled the
  // enumeration after the first day. Every monthly and yearly total
  // was computed from a single day. Braces = returns undefined.
  snap.forEach((c) => { rows.push({ date: c.key, ...(c.val() || {}) }); });
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
    if (ringPush({ t: live.ts, v: live.v, i: live.i, p: live.p, soc: live.soc }, true)) {
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

/* redraw theme-coloured charts when the palette flips */
new MutationObserver(() => {
  if (monthlyChart && !$("tab-monthly").classList.contains("hidden")) loadMonthlyTab();
  if (socChart) drawSoc();          // SoC line/fill are theme colours
}).observe(document.documentElement, { attributes: true, attributeFilter: ["data-theme"] });
