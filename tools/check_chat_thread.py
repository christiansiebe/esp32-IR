#!/usr/bin/env python3
"""Heartbeat-Waechter fuer den Chat-Thread (docs/CHAT-THREAD.md).

Bei jedem Heartbeat aufrufen. Meldet **nur**, wenn sich etwas geaendert hat
(neue Thread-Nachricht, neuer PR-Kommentar, neuer Commit auf origin) — sonst
„KEINE AENDERUNG." Damit kann der Kontroll-Chat still mitlesen und nur bei
echten Neuigkeiten berichten.

Aufruf:
    python3 tools/check_chat_thread.py            # nutzt $ESP32_IR_REPO oder /tmp/esp32-IR
    ESP32_IR_REPO=/pfad python3 tools/check_chat_thread.py

Zustand: $CHAT_THREAD_STATE (Default: ~/.cache/esp32_ir_thread_state.json).
Der Zustand ist lokal und wird NICHT committet.
"""
import json
import os
import subprocess

BRANCH = os.environ.get("CHAT_THREAD_BRANCH", "origin/cline/auftrag-history-dubletten")
PR = os.environ.get("CHAT_THREAD_PR", "3")
REPO = os.environ.get("ESP32_IR_REPO", "/tmp/esp32-IR")
STATE = os.environ.get(
    "CHAT_THREAD_STATE",
    os.path.join(os.path.expanduser("~"), ".cache", "esp32_ir_thread_state.json"),
)
SLUG = os.environ.get("GITHUB_REPO", "christiansiebe/esp32-IR")


def sh(cmd):
    return subprocess.run(cmd, shell=True, cwd=REPO, capture_output=True, text=True).stdout


def thread_messages():
    txt = sh(f"git show {BRANCH}:docs/CHAT-THREAD.md")
    return [l.strip() for l in txt.splitlines() if l.startswith("## 20")]


def pr_comments():
    out = sh(
        "curl -sS -m 10 -H \"Authorization: Bearer $GITHUB_WRITE_TOKEN\" "
        "-H 'Accept: application/vnd.github+json' "
        f"https://api.github.com/repos/{SLUG}/issues/{PR}/comments"
    )
    try:
        return [f"{c['user']['login']}@{c['created_at']}:{c['body'][:60]}" for c in json.loads(out)]
    except Exception:
        return []


def latest_commits():
    return sh(
        "git for-each-ref --sort=-committerdate --format='%(objectname:short) %(refname:short)' "
        "refs/remotes/origin | head -6"
    ).strip().splitlines()


def snapshot():
    return {"thread": thread_messages(), "pr_comments": pr_comments(), "commits": latest_commits()}


def main():
    sh("git fetch origin --quiet")
    cur = snapshot()
    old = {}
    if os.path.exists(STATE):
        try:
            old = json.load(open(STATE))
        except Exception:
            old = {}
    os.makedirs(os.path.dirname(STATE), exist_ok=True)
    json.dump(cur, open(STATE, "w"), indent=1)

    if not old:
        print("BASELINE gesetzt.")
        print(f"Thread-Nachrichten: {len(cur['thread'])} | PR-Kommentare: {len(cur['pr_comments'])}")
        return

    changes = []
    new_thread = [m for m in cur["thread"] if m not in old.get("thread", [])]
    if new_thread:
        changes.append("NEUE THREAD-NACHRICHT(EN):\n  " + "\n  ".join(new_thread))
    new_comments = [c for c in cur["pr_comments"] if c not in old.get("pr_comments", [])]
    if new_comments:
        changes.append("NEUE PR-KOMMENTARE:\n  " + "\n  ".join(new_comments))
    if cur["commits"] != old.get("commits"):
        changes.append("NEUE COMMITS auf origin:\n  " + "\n  ".join(cur["commits"]))

    if changes:
        print("### AENDERUNG ERKANNT ###")
        print("\n".join(changes))
    else:
        print("KEINE AENDERUNG.")


if __name__ == "__main__":
    main()
