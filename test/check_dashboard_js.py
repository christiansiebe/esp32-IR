#!/usr/bin/env python3
"""Guard the dashboard JavaScript that lives inside main/main.c C strings.

The dashboard <script> is embedded in C string literals, so the C compiler
cannot see syntax errors in it. This test reconstructs the page, then:

  1. runs ``node --check`` on the script (syntax), and
  2. runs it against a tiny DOM/canvas stub and exercises the hover/tap tooltip
     (drawHistory -> showTip -> clearTip), catching runtime errors.

Skipped (exit 0) when node is unavailable.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
MAIN_C = os.path.join(HERE, "..", "main", "main.c")

HARNESS = r"""
const dashedXs = [];
let dashOn = false;
const ctxStub = new Proxy({}, {
  get: (t, p) => {
    if (p in t) return t[p];
    switch (p) {
      case 'setLineDash': return (a) => { dashOn = !!(a && a.length); };
      case 'moveTo': return (x) => { if (dashOn) dashedXs.push(x); };
      default: return () => {};
    }
  },
  set: (t, p, v) => { t[p] = v; return true; },
});
function makeEl() {
  return {
    style: {}, textContent: '', value: '', width: 0, height: 0,
    offsetWidth: 40, offsetHeight: 18,
    addEventListener: () => {}, appendChild: () => {},
    getContext: () => ctxStub,
    getBoundingClientRect: () => ({ width: 800, height: 260, left: 0, top: 0 }),
  };
}
const els = {};
const documentStub = {
  getElementById: (id) => (els[id] = els[id] || makeEl()),
  createElement: () => makeEl(),
  addEventListener: () => {},
};
const sandbox = {
  document: documentStub,
  window: { devicePixelRatio: 2, innerWidth: 1200, addEventListener: () => {} },
  setTimeout, clearTimeout, Date, Math, Number, JSON, console, AbortController,
  fetch: async () => ({ ok: true, json: async () => ({ records: [] }) }),
};
sandbox.globalThis = sandbox;
vm.createContext(sandbox);
try {
  vm.runInContext(SCRIPT + "\n;globalThis.__t={getEntries:()=>historyEntries,drawHistory,showTip,clearTip,setCompare:v=>{historyCompare=v;}};", sandbox);
} catch (e) { console.error('FAIL load: ' + e.message); process.exit(1); }

const base = 1700000000000 - (1700000000000 % 3600000);
const rows = [
  { timestamp: base, watts: 300 },
  { timestamp: base + 60000, watts: -120 },
  { timestamp: base + 120000, watts: 500 },
];
try { sandbox.__t.drawHistory(rows, 2, base, null); }
catch (e) { console.error('FAIL drawHistory: ' + e.message); process.exit(1); }
if (sandbox.__t.getEntries().length !== 3) { console.error('FAIL entries'); process.exit(1); }

const target = sandbox.__t.getEntries()[1];
try { sandbox.__t.showTip({ clientX: target.plotX + 1, clientY: 100 }); }
catch (e) { console.error('FAIL showTip: ' + e.message); process.exit(1); }
const tip = els['history-tip'];
if (tip.style.display !== 'block') { console.error('FAIL tip not shown'); process.exit(1); }
if (!tip.textContent.includes('120') || !/W/.test(tip.textContent)) {
  console.error('FAIL tip text: ' + tip.textContent); process.exit(1);
}
try { sandbox.__t.clearTip(); }
catch (e) { console.error('FAIL clearTip: ' + e.message); process.exit(1); }
if (tip.style.display !== 'none') { console.error('FAIL tip not hidden'); process.exit(1); }

sandbox.__t.showTip({ clientX: 5000, clientY: 100 });
if (els['history-tip'].style.display !== 'none') { console.error('FAIL tip far'); process.exit(1); }
console.log('dashboard tooltip runtime ok');

// Day windows must break exactly at 0:00 UTC, not at arbitrary quarters.
// Start at 06:00 UTC over 48 h: the UTC midnights must fall at 18 h and 42 h
// into the window (distinct from the plain quarter grid at 0/12/24/36/48 h).
const dstart = 6 * 3600000, dspan = 48 * 3600000;
try { sandbox.__t.drawHistory(rows, 48, dstart, null); }
catch (e) { console.error('FAIL drawHistory day: ' + e.message); process.exit(1); }
if (dashedXs.length !== 2) {
  console.error('FAIL day boundaries: ' + JSON.stringify(dashedXs)); process.exit(1);
}
const innerW = 800 - 42 - 12;
for (const [i, hOff] of [18, 42].entries()) {
  const expectX = 42 + (hOff / 48) * innerW;
  if (Math.abs(dashedXs[i] - expectX) > 0.5) {
    console.error('FAIL day boundary x: got ' + dashedXs[i] + ' want ' + expectX);
    process.exit(1);
  }
}
// The marked positions must land on exact UTC midnights.
for (const dx of dashedXs) {
  const t = dstart + (dx - 42) / innerW * dspan;
  if (Math.abs(t % 86400000) > 1) {
    console.error('FAIL day boundary not midnight UTC: ' + t); process.exit(1);
  }
}
console.log('dashboard day boundaries ok');

// The "Vortag vergleichen" compare day carries yesterday's timestamps, which
// must be shifted onto the selected day's window before plotting. Otherwise the
// t<start||t>end guard clips every compare bar and the button appears dead.
const cstart = Date.UTC(2026, 9, 10, 0, 0, 0), cday = Date.UTC(2026, 9, 9, 0, 0, 0);
const today = [{ timestamp: cstart + 8 * 3600000, watts: 120000 },
               { timestamp: cstart + 9 * 3600000, watts: 120000 }];
const yesterday = [{ timestamp: cday + 8 * 3600000, watts: 200 },
                   { timestamp: cday + 9 * 3600000, watts: 900 }];
sandbox.__t.setCompare(new Date(cday).toISOString().slice(0, 10));
try { sandbox.__t.drawHistory(today, 24, cstart, yesterday); }
catch (e) { console.error('FAIL drawHistory compare: ' + e.message); process.exit(1); }
const cmpEntries = sandbox.__t.getEntries();
const cmpVisible = cmpEntries.filter(e => e.value === 200 || e.value === 900);
if (cmpVisible.length !== 2) {
  console.error('FAIL compare day not aligned onto window: ' + JSON.stringify(cmpEntries));
  process.exit(1);
}
for (const e of cmpVisible) {
  if (e.plotX < 42 - 0.5 || e.plotX > 800 - 12 + 0.5) {
    console.error('FAIL compare bar outside plot area: ' + e.plotX); process.exit(1);
  }
}
console.log('dashboard compare alignment ok');
process.exit(0);
"""


def c_string_contents(block: str) -> str:
    parts = []
    i = 0
    while True:
        j = block.find('"', i)
        if j < 0:
            break
        k = j + 1
        out = []
        while True:
            c = block[k]
            if c == "\\":
                nxt = block[k + 1]
                if nxt == '"':
                    out.append('"'); k += 2; continue
                if nxt == "\\":
                    out.append("\\"); k += 2; continue
                if nxt == "n":
                    out.append("\n"); k += 2; continue
                if nxt == "t":
                    out.append("\t"); k += 2; continue
                out.append(nxt); k += 2; continue
            if c == '"':
                break
            out.append(c); k += 1
        parts.append("".join(out))
        i = k + 1
    return "".join(parts)


def extract_script(source: str) -> str:
    start = source.index("static const char sml_page[] =")
    marker = "updateHistory();</script>\";"
    end = source.index(marker, start) + len(marker)
    page = c_string_contents(source[start:end])
    match = re.search(r"<script>(.*)</script>", page, re.S)
    if not match:
        raise SystemExit("no <script> block found in the dashboard page")
    return match.group(1)


def extract_diag_script(source: str) -> str:
    start = source.index("static esp_err_t diag_page_handler")
    marker = "</script><p><a href=\\\"/\\\">Back</a></p></body></html>\";"
    end = source.index(marker, start) + len(marker)
    page = c_string_contents(source[start:end])
    match = re.search(r"<script>(.*)</script>", page, re.S)
    if not match:
        raise SystemExit("no <script> block found in the diagnostics page")
    return match.group(1)


def main() -> int:
    node = shutil.which("node")
    if node is None:
        print("node not found; skipping dashboard JS check")
        return 0

    with open(MAIN_C, encoding="utf-8") as handle:
        source = handle.read()
    script = extract_script(source)
    diag_script = extract_diag_script(source)

    workdir = tempfile.mkdtemp()
    try:
        script_path = os.path.join(workdir, "dash.js")
        with open(script_path, "w", encoding="utf-8") as handle:
            handle.write(script)

        diag_path = os.path.join(workdir, "diag.js")
        with open(diag_path, "w", encoding="utf-8") as handle:
            handle.write(diag_script)
        diag_check = subprocess.run([node, "--check", diag_path],
                                    capture_output=True, text=True)
        if diag_check.returncode != 0:
            print("diagnostics page JS syntax error:")
            print(diag_check.stderr)
            return 1

        check = subprocess.run([node, "--check", script_path],
                               capture_output=True, text=True)
        if check.returncode != 0:
            print("dashboard JS syntax error:")
            print(check.stderr)
            return 1

        harness = ("const fs=require('fs'),vm=require('vm');"
                   "const SCRIPT=fs.readFileSync(%r,'utf8');\n" % script_path)
        harness += HARNESS
        harness_path = os.path.join(workdir, "harness.js")
        with open(harness_path, "w", encoding="utf-8") as handle:
            handle.write(harness)

        run = subprocess.run([node, harness_path], capture_output=True, text=True,
                             timeout=30)
        sys.stdout.write(run.stdout)
        sys.stderr.write(run.stderr)
        if run.returncode != 0:
            print("dashboard JS runtime check failed")
            return 1
    finally:
        shutil.rmtree(workdir, ignore_errors=True)

    print("dashboard JS syntax ok (%d bytes)" % len(script))
    print("diagnostics page JS syntax ok (%d bytes)" % len(diag_script))
    return 0


if __name__ == "__main__":
    sys.exit(main())
