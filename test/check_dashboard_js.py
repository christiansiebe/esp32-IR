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
const ctxStub = new Proxy({}, {
  get: (t, p) => (p in t ? t[p] : (() => {})),
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
  vm.runInContext(SCRIPT + "\n;globalThis.__t={getEntries:()=>historyEntries,drawHistory,showTip,clearTip};", sandbox);
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


def main() -> int:
    node = shutil.which("node")
    if node is None:
        print("node not found; skipping dashboard JS check")
        return 0

    with open(MAIN_C, encoding="utf-8") as handle:
        script = extract_script(handle.read())

    workdir = tempfile.mkdtemp()
    try:
        script_path = os.path.join(workdir, "dash.js")
        with open(script_path, "w", encoding="utf-8") as handle:
            handle.write(script)

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
    return 0


if __name__ == "__main__":
    sys.exit(main())
