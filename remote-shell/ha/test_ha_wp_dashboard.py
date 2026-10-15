"""Host tests for remote-shell/ha/ha_wp_dashboard.py (pure logic, no network).

Only ``resource_url`` is exercised: the deploy itself needs a live HA and is
covered by the end-to-end run on the Odroid. ``jwt``/``websockets`` are stubbed
so the module imports on a plain host.

Why this matters: HA serves ``/local/*`` with ``Cache-Control: max-age=2678400``
(31 days). A Lovelace resource URL without a query string is therefore cached
for a month and every card update silently never reaches the browser. The
content hash makes each deploy a new URL.
"""

import os
import sys
import types

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

for name in ("jwt", "websockets"):
    sys.modules.setdefault(name, types.ModuleType(name))

import ha_wp_dashboard as hwd  # noqa: E402

CHECKS = 0
FAILURES = 0


def check(cond, msg):
    global CHECKS, FAILURES
    CHECKS += 1
    if not cond:
        FAILURES += 1
        print("  FAIL:", msg)


def write(path, data):
    with open(path, "wb") as fh:
        fh.write(data)


def test_url_is_versioned_and_content_addressed(tmp="/tmp/hwd_card.js"):
    print("test_url_is_versioned_and_content_addressed")
    write(tmp, b"console.log(1)")
    a = hwd.resource_url(tmp)
    check(a.startswith("/local/wp-zoom-chart.js?v="), f"prefix: {a}")
    ver = a.split("?v=")[1]
    check(len(ver) == 10, f"hash length: {ver}")

    # Same bytes -> same URL (idempotent re-deploy, no cache churn).
    check(hwd.resource_url(tmp) == a, "stable for identical content")

    # Changed bytes -> changed URL (busts the browser cache).
    write(tmp, b"console.log(2)")
    b = hwd.resource_url(tmp)
    check(b != a, f"hash must change with content: {a} vs {b}")
    os.remove(tmp)


if __name__ == "__main__":
    test_url_is_versioned_and_content_addressed()
    print(f"\n{CHECKS - FAILURES}/{CHECKS} checks passed")
    sys.exit(1 if FAILURES else 0)
