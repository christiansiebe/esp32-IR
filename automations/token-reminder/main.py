"""Token-expiry reminder: warn before GITHUB_WRITE_TOKEN times out.

Deterministic, no LLM. GitHub returns the token's expiry in the
`github-authentication-token-expiration` response header, so this run reads it
and, when it is inside the warning window, opens (or updates) a GitHub issue -
a visible reminder that survives a sandbox pause. One issue is reused: the body
is updated each run, so it does not spam.

The token must be renewed before it expires, otherwise push/merge and the whole
auto-merge pipeline stop (HTTP 401).
"""
import datetime as dt
import json
import os
import sys
import urllib.error
import urllib.request

REPO = os.environ.get("FLEET_REPO", "christiansiebe/esp32-IR")
API = "https://api.github.com"
WARN_DAYS = int(os.environ.get("TOKEN_WARN_DAYS", "10"))


def get_secret(name):
    if os.environ.get(name):
        return os.environ[name]
    key = os.environ.get("SESSION_API_KEY") or os.environ.get("OH_SESSION_API_KEYS_0", "")
    url = os.environ.get("AGENT_SERVER_URL", "").rstrip("/")
    if url:
        endpoint = f"{url}/api/settings/secrets/{name}"
    else:
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


def gh(token, path, method="GET", body=None):
    headers = {"Accept": "application/vnd.github+json",
               "X-GitHub-Api-Version": "2022-11-28",
               "User-Agent": "esp32-ir-token-reminder"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    data = json.dumps(body).encode() if body is not None else None
    req = urllib.request.Request(API + path, data=data, headers=headers, method=method)
    with urllib.request.urlopen(req, timeout=30) as r:
        return r, r.read().decode()


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


def main():
    token = get_secret("GITHUB_WRITE_TOKEN")
    if not token:
        raise RuntimeError("GITHUB_WRITE_TOKEN is not reachable; cannot check expiry.")
    resp, _ = gh(token, "/user")
    raw = resp.headers.get("github-authentication-token-expiration")
    if not raw:
        print("No expiry header (token does not expire, or it is not a fine-grained token). Nothing to do.")
        return
    expiry = dt.datetime.strptime(raw, "%Y-%m-%d %H:%M:%S %Z").replace(tzinfo=dt.timezone.utc)
    days_left = (expiry - dt.datetime.now(dt.timezone.utc)).days
    print(f"GITHUB_WRITE_TOKEN expires {raw} ({days_left} days left).")
    if days_left > WARN_DAYS:
        print(f"More than {WARN_DAYS} days left; no reminder needed.")
        return

    body = (f"**GITHUB_WRITE_TOKEN expires in {days_left} day(s)** "
            f"(on {expiry:%Y-%m-%d %H:%M} UTC).\n\n"
            "Renew the fine-grained PAT (repo `christiansiebe/esp32-IR`: Contents read/write, "
            "Pull requests read/write, Issues read/write) and update the `GITHUB_WRITE_TOKEN` "
            "secret. Otherwise push, merge and the fleet auto-merge stop with HTTP 401.\n\n"
            "_Automated reminder (token-reminder automation)._\n\n"
            "This token can create but not edit issues, so a renewed token leaves this issue "
            "open: close it by hand once the token is renewed.")
    # Title carries the expiry date so a *renewed* token yields a fresh reminder
    # even if the old issue was never closed. Create-only: the PAT cannot edit
    # or close issues, so dedupe is by exact title over the open issues it can
    # read (one reminder per expiry window is enough).
    title = f"Reminder: GITHUB_WRITE_TOKEN expires {expiry:%Y-%m-%d}"
    open_issues = json.loads(gh(token, f"/repos/{REPO}/issues?state=open&per_page=100")[1])
    match = next((i for i in open_issues
                  if i.get("title") == title and "pull_request" not in i), None)
    if match:
        print(f"Reminder issue #{match['number']} already open; nothing to do.")
        return
    try:
        res = json.loads(gh(token, f"/repos/{REPO}/issues", "POST",
                            {"title": title, "body": body, "labels": ["token-reminder"]})[1])
        print(f"Opened reminder issue #{res['number']}.")
    except urllib.error.HTTPError as exc:
        # A token without the label scope still creates the issue; retry bare.
        if exc.code in (403, 422):
            res = json.loads(gh(token, f"/repos/{REPO}/issues", "POST",
                                {"title": title, "body": body})[1])
            print(f"Opened reminder issue #{res['number']} (without label).")
        else:
            raise


try:
    main()
except Exception as exc:  # noqa: BLE001
    print(f"ERROR: {exc}", file=sys.stderr)
    fire_callback("FAILED", str(exc))
    sys.exit(1)
fire_callback("COMPLETED")
