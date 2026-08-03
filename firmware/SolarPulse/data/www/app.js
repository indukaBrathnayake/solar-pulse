/* SolarPulse on-device dashboard.
   Talks to the ESP32 only: /api/live, /api/monthly, /api/relay, /api/config.
   Deliberately dependency-free so it still works when the internet is out. */

const $ = (id) => document.getElementById(id);
const MONTHS = ["Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"];

const n = (x, d = 0) => (x == null || isNaN(x) ? "--" : Number(x).toFixed(d));

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
  $("loadSub").textContent = d.src === "utility" ? "fed from the grid" : "fed from the pack";
  $("utilMin").textContent = d.utilMin ?? 0;

  $("today").textContent = n((d.harvestWh || 0) / 1000, 2);
  $("peak").textContent  = n(d.peakW);
  $("life").textContent  = n((d.lifeChg || 0) / 1000, 1);

  setRelay("relay-solar", d.relayS, false);
  setRelay("relay-util",  d.relayU, true);
  setRelay("relay-light", d.light,  false);
  $("why").textContent = (d.manual ? "manual · " : "automatic · ") + (d.why || "");
  $("travel").textContent = d.travel ? "CLOSED — travel mode" : "open — normal";

  // Tonight's plan, decided by the 18:15 target check. Firmware that
  // predates these keys omits them, so hide the line rather than lie.
  const tn = $("tonight");
  if (d.nightFloor == null) {
    tn.classList.add("hidden");
  } else {
    tn.classList.remove("hidden");
    tn.classList.toggle("rainy", !!d.rainy);
    tn.innerHTML = d.rainy
      ? `Tonight: 99% target <b>missed</b> — pack may run down to <b>${d.nightFloor}%</b> before CEB.`
      : `Tonight: target reached — pack held above <b>${d.nightFloor}%</b>.`;
  }

  $("queued").textContent  = d.buffered ?? 0;
  $("bmsLink").textContent = d.bmsLink ? "up" : "lost";
  $("heap").textContent    = n((d.heap || 0) / 1024);

  document.querySelectorAll("[data-src]").forEach((b) => {
    const want = d.manual ? d.src : "auto";
    b.classList.toggle("sel", b.dataset.src === want);
  });
  document.querySelectorAll("[data-light]").forEach((b) =>
    b.classList.toggle("sel", (b.dataset.light === "1") === !!d.light));
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
}).catch(() => {});

poll();
setInterval(poll, 5000);
