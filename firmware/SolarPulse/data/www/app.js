/* SolarPulse on-device dashboard.
   Talks to the ESP32 only: /api/live, /api/monthly, /api/relay, /api/config.
   Deliberately dependency-free so it still works when the internet is out. */

const $ = (id) => document.getElementById(id);
const MONTHS = ["Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"];

const n = (x, d = 0) => (x == null || isNaN(x) ? "--" : Number(x).toFixed(d));

// filled from /api/config so the banner quotes the real threshold
let cfgCutoff = 35;

function fmtUptime(sec) {
  if (!sec && sec !== 0) return "--";
  const d = Math.floor(sec / 86400), h = Math.floor((sec % 86400) / 3600);
  const m = Math.floor((sec % 3600) / 60);
  return d ? `${d}d ${h}h` : h ? `${h}h ${m}m` : `${m}m`;
}

/* ---------------- live ---------------- */
async function poll() {
  let d;
  try {
    d = await (await fetch("/api/live", { cache: "no-store" })).json();
  } catch (e) {
    $("stamp").textContent = "no answer from the ESP";
    $("stamp").classList.add("stale");
    return;
  }
  $("stamp").classList.remove("stale");
  $("stamp").textContent = d.ts > 1e9
    ? new Date(d.ts * 1000).toLocaleString("en-GB", { hour12: false })
    : "clock not set yet";

  $("pvW").textContent   = n(d.pvW);
  $("loadW").textContent = n(d.loadW);
  $("gridW").textContent = n(d.gridW);
  $("soc").textContent   = n(d.soc);
  $("socBar").style.width = Math.max(0, Math.min(100, d.soc || 0)) + "%";
  $("packVI").textContent = `${n(d.v, 2)} V · ${n(d.i, 2)} A`;
  $("pvSub").textContent  = d.pvW > 5 ? "array producing" : "no production";
  // Authoritative committed relay state, same key the OLED uses.
  $("loadSub").textContent = d.house === "CEB" ? "fed from the grid"
                           : d.house === "Pack" ? "fed from the pack" : "source unknown";
  $("utilMin").textContent = d.utilMin ?? 0;

  $("today").textContent = n((d.harvestWh || 0) / 1000, 2);
  $("peak").textContent  = n(d.peakW);
  $("life").textContent  = n((d.lifeChg || 0) / 1000, 1);

  setRelay("relay-solar", d.relayS, false);
  setRelay("relay-util",  d.relayU, true);
  setRelay("relay-light", d.light,  false);
  $("why").textContent = (d.manual ? "manual · " : "automatic · ") + (d.why || "");
  $("travel").textContent = d.travel ? "CLOSED — travel mode" : "open — normal";
  // CEB handover plan. Firmware that predates these keys omits
  // them, so hide the line rather than lie.
  const tn = $("tonight");
  if (d.ceb == null) {
    tn.classList.add("hidden");
  } else {
    tn.classList.remove("hidden");
    const heavy = d.wx === "heavy";
    tn.classList.toggle("rainy", heavy);
    // Two signals, in the order the firmware ranks them: the online
    // forecast while it still carries confidence (fcConf > 0), then
    // the measured expected-solar label, then the coarse class. The
    // OLED renders the same fields in the same order, so the two
    // displays cannot disagree.
    const fcOk = (d.fcConf || 0) > 0 && d.fcText && d.fcText !== "UNKNOWN";
    const wxTxt = fcOk
                ? "forecast " + d.fcText.toLowerCase()
                : d.wxText && d.wxText !== "UNKNOWN"
                ? "expecting " + d.wxText.toLowerCase()
                : (d.wx === "clear" ? "expecting a good solar day"
                :  heavy            ? "expecting a low-solar day"
                :  d.wx === "half"  ? "expecting a mixed solar day"
                :  "not enough evidence to expect anything yet");
    const pv = d.pvVerdict === "good" ? " · measured PV confirms it"
             : d.pvVerdict === "poor" ? " · measured PV says worse"
             : "";
    // d.house is the authoritative committed relay state.
    tn.innerHTML = (d.house === "CEB")
      ? `<b>CEB</b> carrying the house · ${wxTxt}${pv} · hands back at <b>${d.cebRel}</b> if the pack allows.`
      : `On <b>Pack</b> · ${wxTxt}${pv} · CEB engages at <b>${d.cebOn}%</b>.`;
  }

  // Battery protection banner. Load cutoff is the loudest thing the
  // controller can do, so it gets its own always-visible strip.
  const prot = $("protect");
  if (d.emerg) {
    prot.classList.remove("hidden");
    prot.classList.add("cut");
    prot.innerHTML = `<b>EMERGENCY</b> — pack at ${n(d.soc)}% and still supplying the ` +
                     `house. Turn the inverter off.`;
  } else if (d.cutoff) {
    prot.classList.remove("hidden");
    prot.classList.add("cut");
    prot.innerHTML = `<b>Load disconnected</b> — pack at ${n(d.soc)}%, ` +
                     `below the ${cfgCutoff}% cutoff. Alarm sounding.`;
  } else if (d.buzz === 1) {
    prot.classList.remove("hidden");
    prot.classList.remove("cut");
    prot.innerHTML = `<b>Low battery warning</b> — pack at ${n(d.soc)}%. ` +
                     `Load disconnects at ${cfgCutoff}%.`;
  } else {
    prot.classList.add("hidden");
  }

  $("queued").textContent  = d.buffered ?? 0;
  $("bmsLink").textContent = d.bmsLink ? "up" : "lost";
  $("bleState").textContent = d.ble ?? "--";
  $("reconn").textContent  = d.reconn ?? 0;
  $("heap").textContent    = n((d.heap || 0) / 1024);
  $("uptime").textContent  = fmtUptime(d.up);

  document.querySelectorAll("[data-src]").forEach((b) => {
    const want = d.manual ? d.src : "auto";
    b.classList.toggle("sel", b.dataset.src === want);
  });
  // The mode, not the output state: with AUTO selected the lamp may
  // be off simply because it is daytime, and highlighting "Lights
  // off" then would misreport what the controller was told.
  const lm = d.lightMode || (d.light ? "on" : "off");
  document.querySelectorAll("[data-light]").forEach((b) =>
    b.classList.toggle("sel", b.dataset.light === lm));

  const lw = $("lightWhy");
  if (lw) {
    if (d.lightsLow) {
      lw.textContent = "lights off · low battery";
    } else if (lm === "auto") {
      lw.textContent = `lights auto · on at ${d.lightOn ?? "--:--"}` +
        `${d.lightSunset === false ? " (no sunset cached)" : ""}` +
        ` · off at ${d.lightOff ?? "--:--"}` +
        (d.lightMin != null ? ` · ${d.lightMin} min today` : "");
    } else {
      lw.textContent = `lights forced ${lm}` +
        (d.lightMin != null ? ` · ${d.lightMin} min today` : "");
    }
  }
}

function setRelay(id, on, warn) {
  const el = $(id);
  el.classList.toggle("on", !!on && !warn);
  el.classList.toggle("warn", !!on && warn);
}

/* ---------------- controls ---------------- */
async function cmd(qs) {
  try { await fetch("/api/relay?" + qs, { method: "POST" }); } catch (e) {}
  poll();
}
document.querySelectorAll("[data-src]").forEach((b) =>
  b.addEventListener("click", () => cmd("src=" + b.dataset.src)));
document.querySelectorAll("[data-light]").forEach((b) =>
  b.addEventListener("click", () => cmd("light=" + b.dataset.light)));

/* ---------------- monthly ---------------- */
async function loadMonthly() {
  const year = $("year").value || new Date().getFullYear();
  let d;
  try {
    d = await (await fetch("/api/monthly?year=" + year, { cache: "no-store" })).json();
  } catch (e) {
    $("mtotal").textContent = "could not read the daily log";
    return;
  }
  const peak = Math.max(0.001, ...d.months.map((m) => m.kwh));
  const thisMonth = new Date().getMonth();
  const thisYear  = new Date().getFullYear();

  $("chart").innerHTML = d.months.map((m, i) => {
    const h = Math.round((m.kwh / peak) * 100);
    const now = i === thisMonth && +year === thisYear;
    return `<div class="col${now ? " now" : ""}">
              <b>${m.kwh >= 0.05 ? m.kwh.toFixed(1) : ""}</b>
              <u style="height:${h}%"></u>
              <em>${MONTHS[i][0]}</em>
            </div>`;
  }).join("");

  $("mrows").innerHTML = d.months.map((m, i) =>
    `<tr><td>${MONTHS[i]}</td><td>${m.kwh.toFixed(2)}</td>
     <td>${m.used.toFixed(2)}</td></tr>`).join("") +
    `<tr class="tot"><td>Total ${year}</td><td>${d.totalKwh.toFixed(2)}</td>
     <td></td></tr>`;

  $("mtotal").textContent = `${d.totalKwh.toFixed(1)} kWh harvested in ${year}`;
}

/* ---------------- tabs ---------------- */
document.querySelectorAll(".bottom button").forEach((b) => {
  b.addEventListener("click", () => {
    document.querySelectorAll(".bottom button").forEach((x) => x.classList.remove("active"));
    b.classList.add("active");
    $("tab-live").classList.toggle("hidden", b.dataset.tab !== "live");
    $("tab-monthly").classList.toggle("hidden", b.dataset.tab !== "monthly");
    if (b.dataset.tab === "monthly") loadMonthly();
  });
});

/* ---------------- boot ---------------- */
$("year").value = new Date().getFullYear();
$("year").addEventListener("change", loadMonthly);
$("todayCsv").href = "/api/history?date=" + new Date().toLocaleDateString("en-CA");

fetch("/api/config").then((r) => r.json()).then((c) => {
  $("sched").textContent = `${c.travelOn} – ${c.travelOff}`;
  if (c.socLoadCutoff != null) cfgCutoff = c.socLoadCutoff;
  if (c.socBuzzerWarn != null && c.socLoadCutoff != null) {
    $("protRule").textContent =
      `Buzzer warns at ${c.socBuzzerWarn}%, load disconnects at ${c.socLoadCutoff}%.`;
  }
}).catch(() => {});

poll();
setInterval(poll, 5000);
