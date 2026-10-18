"""Fleet auto-merge: merge the safe pull requests, flag the rest.

Deterministic, no LLM. Runs on the platform scheduler. A PR is auto-merged only
when ALL of these hold, so a merge can never silently damage `main`:

  - it targets `main` (stacked PRs and feature branches are left alone),
  - it is not a draft,
  - GitHub reports it `mergeable` with `mergeable_state == "clean"` (no
    conflicts, no missing branch protections),
  - every CI check is green (and there is at least one),
  - it is small: <= MAX_FILES changed files and <= MAX_ADDITIONS added lines,
  - it touches no binary blob (.bin/.elf/.map).

Anything larger or unproven is printed with the reason and left for the parent
to review by hand. The merge uses the async merge API and waits for the result.
"""
import json
import os
import sys
import time
import urllib.request

REPO = os.environ.get("FLEET_REPO", "christiansiebe/esp32-IR")
API = "https://api.github.com"
MAX_FILES = int(os.environ.get("AUTOMERGE_MAX_FILES", "30"))
MAX_ADDITIONS = int(os.environ.get("AUTOMERGE_MAX_ADDITIONS", "1200"))
DRY_RUN = os.environ.get("AUTOMERGE_DRY_RUN", "").lower() in ("1", "true", "yes")
BINARY_EXT = (".bin", ".elf", ".map", ".a", ".o", ".png", ".jpg", ".pdf")


def get_secret(name):
    """Fetch a named secret: ambient env, else the agent server's secret store."""
    if os.environ.get(name):
        return os.environ[name]
    key = os.environ.get("SESSION_API_KEY") or os.environ.get("OH_SESSION_API_KEYS_0", "")
    url = os.environ.get("AGENT_SERVER_URL", "").rstrip("/")
    if url:  # local Agent Canvas
        endpoint = f"{url}/api/settings/secrets/{name}"
    else:  # OpenHands Cloud / Enterprise
        api = os.environ.get("OPENHANDS_CLOUD_API_URL", "").rstrip("/")
        sandbox = os.environ.get("SANDBOX_ID", "")
        if not api or not sandbox:
            return ""
        endpoint = f"{api}/api/v1/sandboxes/{sandbox}/settings/secrets/{name}"
    try:
        req = urllib.request.Request(endpoint, headers={"X-Session-API-Key": key})
        with urllib.request.urlopen(req, timeout=30) as r:
            return r.read().decode().strip()
    except Exception:  # noqa: BLE001
        return ""


def resolve_token():
    """The first token that actually authenticates (merging needs a real one)."""
    for name in ("GITHUB_WRITE_TOKEN", "GITHUB_TOKEN", "GH_TOKEN"):
        tok = get_secret(name)
        if not tok:
            continue
        try:
            req = urllib.request.Request(API + "/user", headers={
                "Authorization": f"Bearer {tok}", "Accept": "application/vnd.github+json",
                "User-Agent": "esp32-ir-fleet-automerge"})
            with urllib.request.urlopen(req, timeout=30) as r:
                login = json.loads(r.read().decode()).get("login")
            print(f"token {name} authenticates as {login}")
            return tok
        except Exception as exc:  # noqa: BLE001
            print(f"token {name} unusable: {exc}")
    return ""


def diagnostics():
    interesting = sorted(k for k in os.environ
                         if any(s in k.upper() for s in ("GITHUB", "SECRET", "TOKEN", "API")))
    return ("no usable GitHub token. env keys of interest: " + ", ".join(interesting)
            + " | AGENT_SERVER_URL set=" + str(bool(os.environ.get("AGENT_SERVER_URL")))
            + " SANDBOX_ID set=" + str(bool(os.environ.get("SANDBOX_ID")))
            + " CLOUD_API set=" + str(bool(os.environ.get("OPENHANDS_CLOUD_API_URL"))))


def gh(token, path, method="GET", body=None):
    headers = {"Accept": "application/vnd.github+json",
               "X-GitHub-Api-Version": "2022-11-28",
               "User-Agent": "esp32-ir-fleet-automerge"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(API + path, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=30) as r:
        raw = r.read().decode()
    return json.loads(raw) if raw.strip() else {}


def ci_green(token, sha):
    runs = gh(token, f"/repos/{REPO}/commits/{sha}/check-runs")["check_runs"]
    if not runs:
        return False, "no checks"
    if any(r["status"] != "completed" for r in runs):
        return False, "checks pending"
    ok = all(r.get("conclusion") in ("success", "neutral", "skipped") for r in runs)
    return ok, "green" if ok else "red"


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


def merge_async(token, num, sha):
    res = gh(token, f"/repos/{REPO}/pulls/{num}/merge-async", method="PUT",
             body={"merge_method": "squash", "sha": sha})
    uuid = res.get("details", {}).get("uuid")
    if not uuid:
        return res.get("status", "?"), res
    for _ in range(10):
        time.sleep(6)
        st = gh(token, f"/repos/{REPO}/pulls/{num}/merge-async/{uuid}")
        if st.get("status") != "pending":
            return st.get("status"), st.get("details", {})
    return "timeout", {}


def main():
    token = resolve_token()
    if not token:
        raise RuntimeError(diagnostics())
    prs = gh(token, f"/repos/{REPO}/pulls?state=open&per_page=50")
    merged, flagged = [], []
    for pr in sorted(prs, key=lambda p: p["number"]):
        num = pr["number"]
        if pr["base"]["ref"] != "main":
            flagged.append((num, f"base={pr['base']['ref']}"))
            continue
        if pr.get("draft"):
            flagged.append((num, "draft"))
            continue
        detail = gh(token, f"/repos/{REPO}/pulls/{num}")
        if not detail.get("mergeable") or detail.get("mergeable_state") != "clean":
            flagged.append((num, f"state={detail.get('mergeable_state')}"))
            continue
        green, why = ci_green(token, detail["head"]["sha"])
        if not green:
            flagged.append((num, why))
            continue
        files = gh(token, f"/repos/{REPO}/pulls/{num}/files?per_page=100")
        names = [f["filename"] for f in files]
        if detail["changed_files"] > MAX_FILES or detail["additions"] > MAX_ADDITIONS:
            flagged.append((num, f"too big ({detail['changed_files']} files/+{detail['additions']})"))
            continue
        if any(n.lower().endswith(BINARY_EXT) for n in names):
            flagged.append((num, "touches a binary blob"))
            continue
        if DRY_RUN:
            print(f"#{num} WOULD MERGE {detail['title'][:60]}")
            continue
        status, info = merge_async(token, num, detail["head"]["sha"])
        print(f"#{num} merge -> {status} {json.dumps(info)[:120]}")
        if status == "merged":
            merged.append(num)
        else:
            flagged.append((num, f"merge {status}"))

    print(f"\nMERGED: {merged}")
    print("LEFT FOR REVIEW:")
    for item in flagged:
        print(f"  #{item[0]}: {item[1]}")


try:
    main()
except Exception as exc:  # noqa: BLE001
    print(f"ERROR: {exc}", file=sys.stderr)
    fire_callback("FAILED", str(exc))
    sys.exit(1)
fire_callback("COMPLETED")
