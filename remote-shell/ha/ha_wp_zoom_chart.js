/* wp-zoom-chart: Waermepumpen-Verlaufsgrafik
 * - Klick auf die Karte oeffnet eine Grossansicht
 * - Rand stufenlos ziehbar (CSS resize + ResizeObserver)
 * - Aufloesung passt sich der Fensterbreite an (mehr Punkte bei mehr Breite)
 * - Zeitraum waehlbar: 1 h / 6 h / 24 h / 7 T / 30 T / 90 T / 1 J
 * Datenquelle: HA-WebSocket. Kurze Zeitraeume aus der Rohhistorie,
 * lange aus der Langzeitstatistik (Tages-/Wochenmittel bzw. -summen).
 * Die Historie vor dem HA-Eigenbestand wurde aus den Gateway-Recordings
 * importiert (bis 2025-08-31).
 */
const RANGES = [
  ["1h", 3600], ["6h", 21600], ["24h", 86400],
  ["7T", 604800], ["30T", 2592000], ["90T", 7776000], ["1J", 31536000],
];

function el(tag, cls, txt) {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (txt != null) e.textContent = txt;
  return e;
}

function downsample(points, buckets) {
  if (points.length <= buckets) return points;
  const out = [];
  const step = points.length / buckets;
  for (let i = 0; i < buckets; i++) {
    const a = Math.floor(i * step), b = Math.floor((i + 1) * step);
    let s = 0, n = 0;
    for (let j = a; j < b && j < points.length; j++) { s += points[j][1]; n++; }
    if (n) out.push([points[a][0], s / n]);
  }
  return out;
}

function clampModalSize(rect, bounds) {
  const w = Math.max(bounds.minW, Math.min(rect.w, bounds.maxW));
  const h = Math.max(bounds.minH, Math.min(rect.h, bounds.maxH));
  return { w, h };
}

class WpZoomChart extends HTMLElement {
  setConfig(config) {
    this._cfg = config || {};
    this._entities = this._cfg.entities || [];
    this._title = this._cfg.title || "Verlauf";
    this._default = this._cfg.default_range || "24h";
    this._height = this._cfg.height || 260;
    if (this._built) return;
    this._built = true;
    this._render();
  }

  set hass(h) {
    this._hass = h;
    if (!this._loaded) { this._loaded = true; this._load(); }
  }

  connectedCallback() { if (this._built) this._render(); }
  disconnectedCallback() { this._destroyChart(); }

  getCardSize() { return 4; }

  // Sections view: let the user resize the card in the grid (columns x rows).
  getGridOptions() { return { columns: 12, rows: 4, min_columns: 6, min_rows: 2 }; }

  _render() {
    this.innerHTML = "";
    const card = el("div", "wz-card");
    const head = el("div", "wz-head");
    head.appendChild(el("div", "wz-title", this._title));
    const exp = el("button", "wz-expand");
    exp.textContent = "\u26f6";
    exp.title = "Grossansicht";
    exp.onclick = (e) => { e.stopPropagation(); this._openModal(); };
    head.appendChild(exp);
    card.appendChild(head);
    const box = el("div", "wz-chart");
    box.style.height = this._height + "px";
    card.appendChild(box);
    this.appendChild(card);
    this._chartEl = box;
    this._ensureApex().then(() => this._buildChart(box));
  }

  async _ensureApex() {
    for (let i = 0; i < 100 && !window.ApexCharts; i++) {
      await new Promise((r) => setTimeout(r, 100));
    }
    return !!window.ApexCharts;
  }

  _buildChart(container) {
    if (!window.ApexCharts) {
      container.appendChild(el("div", "wz-err", "ApexCharts nicht geladen"));
      return null;
    }
    const ch = new window.ApexCharts(container, this._chartOptions());
    ch.render();
    return ch;
  }

  _chartOptions() {
    const series = (this._series || []).map((s) => ({
      name: s.name, data: s.points,
    }));
    return {
      chart: {
        type: "line", height: "100%", width: "100%",
        animations: { enabled: false },
        toolbar: { show: false }, zoom: { enabled: true, type: "x" },
        fontFamily: "inherit",
      },
      series,
      stroke: { curve: "smooth", width: 2 },
      markers: { size: 0 },
      legend: { show: true, position: "bottom" },
      grid: { borderColor: "#e0e0e0", strokeDashArray: 3 },
      xaxis: { type: "datetime", labels: { datetimeUTC: false } },
      yaxis: (this._series || []).map((s) => ({
        seriesName: s.name, show: true,
        title: { text: s.unit || "" },
        opposite: !!s.opposite, decimalsInFloat: 1,
      })),
      tooltip: { x: { format: "dd.MM. HH:mm" } },
      noData: { text: "Keine Daten im Zeitraum" },
    };
  }

  async _load() {
    if (!this._hass) return;
    this._series = await this._fetch(this._default, this._width());
    this._update();
  }

  _width() {
    const w = (this._chartEl && this._chartEl.clientWidth) || 600;
    return Math.max(300, w);
  }

  _update() {
    if (this._chart) {
      this._chart.updateOptions(this._chartOptions(), false, true, false);
      return;
    }
    if (this._chartEl) this._chart = this._buildChart(this._chartEl);
  }

  async _fetch(rangeKey, widthPx) {
    const span = (RANGES.find((r) => r[0] === rangeKey) || RANGES[2])[1];
    const end = Date.now();
    const start = end - span * 1000;
    const buckets = Math.max(80, Math.min(3000, Math.floor(widthPx / 2)));
    const out = [];
    for (const e of this._entities) {
      const pts = await this._entitySeries(e, start, end, span, buckets);
      out.push({ name: e.name || e.entity, unit: e.unit || "",
                 opposite: !!e.opposite, points: pts });
    }
    return out;
  }

  async _entitySeries(e, startMs, endMs, spanSec, buckets) {
    const iso = (ms) => new Date(ms).toISOString();
    const kind = e.kind || "mean";
    try {
      // Rohhistorie: nur solange HA sie vorhaelt (kurze Zeitraeume).
      if (spanSec <= 7 * 86400) {
        const res = await this._hass.callWS({
          type: "history/history_during_period",
          start_time: iso(startMs), end_time: iso(endMs),
          entity_ids: [e.entity], minimal_response: true,
          no_attributes: true,
        });
        const raw = (res && res[e.entity]) || [];
        let pts = [];
        for (const p of raw) {
          const v = parseFloat(p.s != null ? p.s : p.state);
          const t = p.lu != null ? p.lu * 1000 : Date.parse(p.last_changed);
          if (isFinite(v)) pts.push([t, v]);
        }
        if (kind === "sum") {
          let acc = 0, prev = null;
          const d = [];
          for (const [t, v] of pts) {
            if (prev != null) acc += Math.max(0, v - prev);
            prev = v;
            d.push([t, +acc.toFixed(3)]);
          }
          pts = d;
        }
        if (pts.length) return downsample(pts, buckets);
        // Fallback auf Statistik, falls keine Rohdaten mehr da sind.
      }
      const period = spanSec <= 60 * 86400 ? "day"
        : spanSec <= 400 * 86400 ? "week" : "month";
      const res = await this._hass.callWS({
        type: "recorder/statistics_during_period",
        start_time: iso(startMs), end_time: iso(endMs),
        statistic_ids: [e.entity], period,
      });
      const raw = (res && res[e.entity]) || [];
      let acc = 0;
      const pts = [];
      for (const p of raw) {
        const t = p.start;
        let v;
        if (kind === "sum") {
          const c = p.change != null ? p.change : (p.sum != null ? p.sum : 0);
          acc += c; v = +acc.toFixed(3);
        } else {
          v = p.mean != null ? p.mean : (p.max != null ? p.max : null);
        }
        if (v != null && isFinite(v)) pts.push([t, v]);
      }
      return pts;
    } catch (err) {
      return [];
    }
  }

  _openModal() {
    const ov = el("div", "wz-overlay");
    const box = el("div", "wz-modal");
    const head = el("div", "wz-mhead");
    head.appendChild(el("div", "wz-mtitle", this._title));
    const btns = el("div", "wz-ranges");
    this._activeRange = this._activeRange || this._default;
    for (const [k] of RANGES) {
      const b = el("button", "wz-range" + (k === this._activeRange ? " on" : ""), k);
      b.onclick = async () => {
        this._activeRange = k;
        btns.querySelectorAll(".wz-range").forEach((x) =>
          x.classList.toggle("on", x.textContent === k));
        const s = await this._fetch(k, mbox.clientWidth);
        mchart.updateOptions({ series: s.map((x) => ({ name: x.name, data: x.points })) });
      };
      btns.appendChild(b);
    }
    head.appendChild(btns);
    const close = el("button", "wz-close", "\u2715");
    close.onclick = () => this._closeModal(ov);
    head.appendChild(close);
    box.appendChild(head);

    const mbox = el("div", "wz-mchart");
    box.appendChild(mbox);
    box.appendChild(el("div", "wz-hint",
      "Ecke unten rechts ziehen zum Vergroessern \u2013 die Aufloesung waechst mit."));

    // A real drag grip: the native CSS resize handle is a tiny, scrollbar-overlapped
    // corner that is unusable on touch. Pointer events cover mouse + pen + touch.
    const grip = el("div", "wz-grip");
    grip.title = "Ziehen zum Vergroessern";
    box.appendChild(grip);
    grip.addEventListener("pointerdown", (ev) => {
      ev.preventDefault();
      grip.setPointerCapture(ev.pointerId);
      const r = box.getBoundingClientRect();
      // Pin the modal so the centering flex parent cannot fight the drag.
      box.style.position = "fixed";
      box.style.margin = "0";
      box.style.left = r.left + "px";
      box.style.top = r.top + "px";
      box.style.width = r.width + "px";
      box.style.height = r.height + "px";
      const move = (e) => {
        const b = { minW: 420, minH: 320,
                    maxW: window.innerWidth - 16, maxH: window.innerHeight - 16 };
        const s = clampModalSize(
          { w: r.width + (e.clientX - ev.clientX), h: r.height + (e.clientY - ev.clientY) }, b);
        box.style.width = s.w + "px";
        box.style.height = s.h + "px";
      };
      const up = () => {
        grip.removeEventListener("pointermove", move);
        grip.removeEventListener("pointerup", up);
        grip.removeEventListener("pointercancel", up);
      };
      grip.addEventListener("pointermove", move);
      grip.addEventListener("pointerup", up);
      grip.addEventListener("pointercancel", up);
    });

    ov.appendChild(box);
    ov.onclick = (ev) => { if (ev.target === ov) this._closeModal(ov); };
    this._esc = (ev) => { if (ev.key === "Escape") this._closeModal(ov); };
    document.addEventListener("keydown", this._esc);
    document.body.appendChild(ov);

    const mchart = new window.ApexCharts(mbox, this._chartOptions());
    mchart.render();
    this._mchart = mchart;
    this._fetch(this._activeRange, mbox.clientWidth).then((s) => {
      mchart.updateOptions({ series: s.map((x) => ({ name: x.name, data: x.points })) });
    });

    let t = null;
    const ro = new ResizeObserver(() => {
      clearTimeout(t);
      t = setTimeout(async () => {
        const s = await this._fetch(this._activeRange, mbox.clientWidth);
        mchart.updateOptions({ series: s.map((x) => ({ name: x.name, data: x.points })) });
      }, 220);
    });
    ro.observe(mbox);
    ov._ro = ro;
  }

  _closeModal(ov) {
    if (ov && ov._ro) ov._ro.disconnect();
    if (this._mchart) { this._mchart.destroy(); this._mchart = null; }
    document.removeEventListener("keydown", this._esc);
    if (ov && ov.parentNode) ov.parentNode.removeChild(ov);
  }

  _destroyChart() {
    if (this._chart) { this._chart.destroy(); this._chart = null; }
  }
}

if (!customElements.get("wp-zoom-chart")) {
  customElements.define("wp-zoom-chart", WpZoomChart);
}

const style = document.createElement("style");
style.textContent = `
.wz-card{border-radius:12px;background:var(--card-background-color,#fff);
  box-shadow:var(--ha-card-box-shadow,none);border:1px solid var(--divider-color,#e0e0e0);
  padding:8px 10px 4px;cursor:pointer;overflow:hidden}
.wz-head{display:flex;align-items:center;justify-content:space-between;gap:8px;padding:2px 2px 6px}
.wz-title{font-weight:600;font-size:15px}
.wz-expand{background:transparent;border:0;cursor:pointer;font-size:18px;color:var(--secondary-text-color,#666)}
.wz-chart{width:100%}
.wz-err{padding:12px;color:#c62828}
.wz-overlay{position:fixed;inset:0;background:rgba(0,0,0,.55);z-index:9999;
  display:flex;align-items:center;justify-content:center;padding:16px}
.wz-modal{background:var(--card-background-color,#fff);border-radius:14px;
  padding:10px 12px 6px;display:flex;flex-direction:column;position:relative;
  box-sizing:border-box;
  width:min(1400px,94vw);height:min(880px,90vh);
  overflow:hidden;min-width:420px;min-height:320px;
  box-shadow:0 12px 40px rgba(0,0,0,.4)}
.wz-mhead{display:flex;align-items:center;gap:10px;flex-wrap:wrap;padding-bottom:6px}
.wz-mtitle{font-weight:700;font-size:17px;flex:1}
.wz-ranges{display:flex;gap:4px;flex-wrap:wrap}
.wz-range{border:1px solid var(--divider-color,#ccc);background:transparent;
  border-radius:8px;padding:4px 10px;cursor:pointer;font-size:13px;
  color:var(--primary-text-color,#222)}
.wz-range.on{background:var(--primary-color,#03a9f4);color:#fff;border-color:transparent}
.wz-close{background:transparent;border:0;font-size:18px;cursor:pointer;color:var(--secondary-text-color,#666)}
.wz-mchart{flex:1;min-height:240px;width:100%}
.wz-hint{font-size:11px;color:var(--secondary-text-color,#888);padding:4px 2px 2px}
.wz-grip{position:absolute;right:3px;bottom:3px;width:18px;height:18px;
  cursor:nwse-resize;touch-action:none;opacity:.75;
  background:
    linear-gradient(135deg,transparent 0 50%,var(--secondary-text-color,#aaa) 50% 60%,transparent 60%),
    linear-gradient(135deg,transparent 0 70%,var(--secondary-text-color,#aaa) 70% 80%,transparent 80%)}
.wz-grip:hover{opacity:1}
`;
document.head.appendChild(style);
