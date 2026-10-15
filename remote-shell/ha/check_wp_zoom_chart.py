#!/usr/bin/env python3
"""Guard the custom Lovelace card ``tools/ha_wp_zoom_chart.js``.

The card is plain browser JavaScript deployed to HA's ``www/``, so nothing in
the firmware build would notice a syntax error or a broken helper. This test
loads the file in Node with a tiny DOM/ApexCharts stub and checks the parts that
are easy to break silently:

  1. ``node --check`` for syntax,
  2. the custom element registers and ``setConfig`` accepts a config,
  3. ``_fetch`` picks the raw-history path for short ranges and the statistics
     path for long ranges, and
  4. the resolution grows monotonically with the container width (the whole
     point of the zoom view).

Skipped (exit 0) when node is unavailable.
"""

import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
CARD = os.path.join(HERE, "ha_wp_zoom_chart.js")

HARNESS = r"""
const fs = require('fs');
const vm = require('vm');
const code = fs.readFileSync(process.argv[2], 'utf8');

function mkEl(tag) {
  return {
    tagName: (tag || 'div').toUpperCase(), children: [], className: '',
    textContent: '', title: '', style: {}, _attrs: {},
    classList: { add() {}, remove() {}, toggle() {}, contains() { return false; } },
    appendChild(c) { this.children.push(c); c.parentNode = this; return c; },
    removeChild(c) { this.children = this.children.filter((x) => x !== c); return c; },
    setAttribute(k, v) { this._attrs[k] = v; }, getAttribute(k) { return this._attrs[k]; },
    addEventListener() {}, removeEventListener() {},
    querySelectorAll() { return []; },
    get clientWidth() { return this._cw || 600; }, get clientHeight() { return 400; },
  };
}
const registered = {};
const sandbox = {
  console, setTimeout, clearTimeout,
  customElements: { define(n, c) { registered[n] = c; }, get(n) { return registered[n]; } },
  document: { createElement: mkEl, head: mkEl('head'), body: mkEl('body'),
    addEventListener() {}, removeEventListener() {} },
  ResizeObserver: class { observe() {} disconnect() {} },
  HTMLElement: class { constructor() { Object.assign(this, mkEl('div')); } },
  Math, Date, JSON, parseFloat, isFinite, Promise, Object, Array, Number, String,
};
sandbox.window = sandbox; sandbox.globalThis = sandbox;
sandbox.window.innerWidth = 1920; sandbox.window.innerHeight = 1080;
vm.createContext(sandbox);

function fail(m) { console.error('FAIL: ' + m); process.exit(1); }

try { vm.runInContext(code, sandbox, { filename: 'ha_wp_zoom_chart.js' }); }
catch (e) { fail('load: ' + e.message); }

const Cls = registered['wp-zoom-chart'];
if (!Cls) fail('custom element wp-zoom-chart not registered');

// Sections view: the card must expose grid sizing so HA lets the user resize it.
const inst0 = new Cls();
if (typeof inst0.getGridOptions !== 'function') fail('getGridOptions missing (no sections resize)');
const go = inst0.getGridOptions();
if (!go.columns || !go.rows) fail('getGridOptions lacks columns/rows');

const inst = new Cls();
try { inst.setConfig({ title: 'T', entities: [{ entity: 'sensor.x', name: 'X' }] }); }
catch (e) { fail('setConfig: ' + e.message); }
if (inst.getCardSize() < 1) fail('getCardSize too small');

// The modal resize clamps to the viewport and honours the minimum size.
if (typeof sandbox.resizeRect !== 'function') fail('resizeRect not exposed');
const b = { minW: 420, minH: 320, vw: 1920, vh: 1080, pad: 8 };
const base = { left: 260, top: 100, width: 1400, height: 880 };

// Grow to the south-east: only the right/bottom edges move.
let r = sandbox.resizeRect(base, 'se', 200, 80, b);
if (r.left !== 260 || r.top !== 100) fail('se moved origin: ' + JSON.stringify(r));
if (r.width !== 1600 || r.height !== 960) fail('se size wrong: ' + JSON.stringify(r));

// Grow to the north-west: the origin moves, the opposite corner stays fixed.
r = sandbox.resizeRect(base, 'nw', -100, -80, b);
if (r.left !== 160 || r.top !== 20) fail('nw origin wrong: ' + JSON.stringify(r));
if (r.width !== 1500 || r.height !== 960) fail('nw size wrong: ' + JSON.stringify(r));

// Pure edge drags must not touch the perpendicular axis.
r = sandbox.resizeRect(base, 'e', 120, 0, b);
if (r.width !== 1520 || r.height !== 880 || r.top !== 100) fail('e wrong: ' + JSON.stringify(r));
r = sandbox.resizeRect(base, 's', 0, 90, b);
if (r.height !== 970 || r.width !== 1400 || r.left !== 260) fail('s wrong: ' + JSON.stringify(r));

// Minimum size is never undershot.
r = sandbox.resizeRect(base, 'se', -5000, -5000, b);
if (r.width !== 420 || r.height !== 320) fail('min size not honoured: ' + JSON.stringify(r));
r = sandbox.resizeRect(base, 'nw', 5000, 5000, b);
if (r.width !== 420 || r.height !== 320) fail('nw min size not honoured: ' + JSON.stringify(r));

// The box stays inside the viewport even on a huge drag.
r = sandbox.resizeRect(base, 'se', 5000, 5000, b);
if (r.left < 0 || r.top < 0 || r.left + r.width > 1920 || r.top + r.height > 1080)
  fail('se escaped viewport: ' + JSON.stringify(r));
r = sandbox.resizeRect(base, 'nw', -5000, -5000, b);
if (r.left < 0 || r.top < 0 || r.left + r.width > 1920 || r.top + r.height > 1080)
  fail('nw escaped viewport: ' + JSON.stringify(r));

// Raw 7-day history (enough points that downsampling must kick in).
const t0 = Date.now() - 7 * 86400 * 1000;
const raw = [];
for (let i = 0; i < 4000; i++) raw.push({ s: String(20 + Math.sin(i / 30) * 5), lu: (t0 + i * 60000) / 1000 });

function instFor(w) {
  const c = new Cls();
  c.setConfig({ title: 'T', height: 200, default_range: '7T',
    entities: [{ entity: 'sensor.x', name: 'X', kind: 'mean' }] });
  c._chartEl = mkEl('div'); c._chartEl._cw = w;
  return c;
}

(async () => {
  // Short range -> raw history path.
  const c = instFor(600);
  let usedHist = false, usedStat = false;
  c._hass = { callWS: async (m) => {
    if (m.type === 'history/history_during_period') { usedHist = true; return { 'sensor.x': raw }; }
    if (m.type === 'recorder/statistics_during_period') { usedStat = true; return { 'sensor.x': [] }; }
    return {};
  } };
  const s = await c._fetch('7T', 600);
  if (!usedHist) fail('short range did not use history/history_during_period');
  if (!s[0].points.length) fail('short range returned no points');

  // Long range -> statistics path (raw history no longer retained by HA).
  const c2 = instFor(600);
  usedHist = usedStat = false;
  c2._hass = { callWS: async (m) => {
    if (m.type === 'history/history_during_period') { usedHist = true; return { 'sensor.x': [] }; }
    if (m.type === 'recorder/statistics_during_period') { usedStat = true;
      return { 'sensor.x': [{ start: t0, mean: 12.5 }, { start: t0 + 86400000, mean: 13.5 }] }; }
    return {};
  } };
  const s2 = await c2._fetch('1J', 600);
  if (!usedStat) fail('long range did not use recorder/statistics_during_period');
  if (s2[0].points.length !== 2) fail('long range points wrong: ' + s2[0].points.length);

  // Resolution must grow with the container width.
  const widths = [300, 600, 1000, 1600, 2400];
  let prev = 0, counts = [];
  for (const w of widths) {
    const ci = instFor(w);
    ci._hass = { callWS: async () => ({ 'sensor.x': raw }) };
    const r = await ci._fetch('7T', w);
    const n = r[0].points.length;
    counts.push(n);
    if (n < prev) fail('resolution not monotonic at ' + w + 'px: ' + counts.join(','));
    prev = n;
  }
  if (counts[counts.length - 1] <= counts[0]) fail('resolution did not increase: ' + counts.join(','));
  console.log('OK (resolution ' + widths.map((w, i) => w + 'px->' + counts[i]).join(', ') + ')');
})();
"""


def main():
    node = shutil.which("node")
    if not node:
        print("node not found - skipping card JS test")
        return 0
    with tempfile.NamedTemporaryFile("w", suffix=".js", delete=False) as fh:
        fh.write(HARNESS)
        harness = fh.name
    try:
        check = subprocess.run([node, "--check", CARD],
                               capture_output=True, text=True)
        if check.returncode != 0:
            print("node --check failed:\n" + check.stderr)
            return 1
        run = subprocess.run([node, harness, CARD], capture_output=True, text=True)
        sys.stdout.write(run.stdout)
        sys.stderr.write(run.stderr)
        return run.returncode
    finally:
        os.unlink(harness)


if __name__ == "__main__":
    sys.exit(main())
