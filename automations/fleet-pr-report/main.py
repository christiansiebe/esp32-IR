"""Fleet PR report: one compact status of every open pull request.

Deterministic, no LLM, no token cost. Runs on the platform scheduler. The parent
reads the report (and the sub-agents act on it) instead of every chat polling
GitHub by hand.

Per PR it prints: number, draft, mergeable state, CI rollup, title, and the
changed files. PRs with green CI and no conflicts are flagged READY so they can
be reviewed and merged; drafts and conflicting PRs are flagged separately.
"""
import json
import os
import sys
import urllib.error
import urllib.request

REPO = os.environ.get("FLEET_REPO", "christiansiebe/esp32-IR")
API = "https://api.github.com"


def token():
    return (os.environ.get("GITHUB_WRITE_TOKEN") or os.environ.get("GITHUB_TOKEN")
            or os.environ.get("GH_TOKEN") or "")


def gh(path):
    headers = {
        "Accept": "application/vnd.github+json",
        "X-GitHub-Api-Version": "2022-11-28",
        "User-Agent": "esp32-ir-fleet-pr-report",
    }
    tok = token()
    if tok:  # optional: the repo is public, so unauthenticated reads also work
        headers["Authorization"] = f"Bearer {tok}"
    req = urllib.request.Request(API + path, headers=headers)
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read().decode())


def fire_callback(status="COMPLETED", error=None):
    url = os.environ.get("AUTOMATION_CALLBACK_URL", "")
    if not url:
        return
    body = {"status": status, "run_id": os.environ.get("AUTOMATION_RUN_ID", "")}
    if error:
        body["error"] = error
    try:
        urllib.request.urlopen(urllib.request.Request(
            url, data=json.dumps(body).encode(),
            headers={"Content-Type": "application/json",
                     "Authorization": f"Bearer {os.environ.get('AUTOMATION_CALLBACK_API_KEY', '')}"},
        ), timeout=30)
    except Exception as exc:  # noqa: BLE001
        print(f"Callback error: {exc}")


def ci_rollup(pr):
    """Best-effort CI state from the head commit's check runs / statuses."""
    sha = pr["head"]["sha"]
    state, total = "none", 0
    try:
        runs = gh(f"/repos/{REPO}/commits/{sha}/check-runs")["check_runs"]
        total = len(runs)
        if runs:
            if any(r["status"] != "completed" for r in runs):
                state = "pending"
            elif all(r.get("conclusion") in ("success", "neutral", "skipped") for r in runs):
                state = "green"
            else:
                state = "red"
    except Exception:  # noqa: BLE001
        pass
    if state == "none":
        try:
            comb = gh(f"/repos/{REPO}/commits/{sha}/status")
            total = len(comb.get("statuses", []))
            if total:
                state = {"success": "green", "pending": "pending",
                         "failure": "red", "error": "red"}.get(comb.get("state"), "none")
        except Exception:  # noqa: BLE001
            pass
    return state, total


def main():
    prs = gh(f"/repos/{REPO}/pulls?state=open&per_page=50")
    ready, drafts, conflicts = [], [], []
    print(f"{len(prs)} offene PRs in {REPO}")
    for pr in sorted(prs, key=lambda p: p["number"]):
        num = pr["number"]
        try:  # the list endpoint reports mergeable=null; ask for the single PR
            pr = gh(f"/repos/{REPO}/pulls/{num}")
        except Exception:  # noqa: BLE001
            pass
        ci, n = ci_rollup(pr)
        mergeable = pr.get("mergeable")
        is_draft = pr.get("draft", False)
        flag = "READY" if (not is_draft and ci == "green" and mergeable) else "     "
        line = (f"#{num:<4} {flag} draft={is_draft} mergeable={mergeable} "
                f"ci={ci}({n}) {pr['title'][:60]}")
        print(line)
        if mergeable is False:
            conflicts.append(num)
        if is_draft:
            drafts.append(num)
        elif ci == "green" and mergeable:
            ready.append(num)

    print(f"\nREADY (review+merge): {ready}")
    print(f"DRAFT: {drafts}")
    print(f"CONFLICTING: {conflicts}")
    fire_callback("COMPLETED")


try:
    main()
except Exception as exc:  # noqa: BLE001
    print(f"ERROR: {exc}", file=sys.stderr)
    fire_callback("FAILED", str(exc))
    sys.exit(1)
