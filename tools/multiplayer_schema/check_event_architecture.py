"""Validate the ownership boundaries of the GOAL multiplayer event API."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
GOAL_ROOT = ROOT / "goal_src" / "jak2"
EVENT_ROOT = GOAL_ROOT / "multiplayer" / "event"
QUEUE_FILE = EVENT_ROOT / "mp-event-queue.gc"
BUILDER_FILE = EVENT_ROOT / "mp-event-builders.gc"
HEADER_FILE = EVENT_ROOT / "mp-event-h.gc"
HOOK_FILE = EVENT_ROOT / "mp-event-hooks.gc"
ENGINE_FILE = GOAL_ROOT / "kernel" / "gstate.gc"


def goal_sources() -> list[Path]:
    return sorted(GOAL_ROOT.rglob("*.gc"))


def without_line_comments(text: str) -> str:
    return "\n".join(line.split(";;", 1)[0] for line in text.splitlines())


def main() -> int:
    errors: list[str] = []
    sources_file = EVENT_ROOT / "mp-event-sources.gc"

    if sources_file.exists():
        errors.append(f"obsolete event source file exists: {sources_file}")
    if not BUILDER_FILE.exists():
        errors.append(f"missing typed builder module: {BUILDER_FILE}")
    if not HEADER_FILE.exists():
        errors.append(f"missing event header module: {HEADER_FILE}")
    if not QUEUE_FILE.exists():
        errors.append(f"missing event queue module: {QUEUE_FILE}")
    if not HOOK_FILE.exists():
        errors.append(f"missing engine event hook module: {HOOK_FILE}")
    if not ENGINE_FILE.exists():
        errors.append(f"missing canonical engine event module: {ENGINE_FILE}")
    if errors:
        for error in errors:
            print(f"ERROR: {error}")
        return 1

    sources = goal_sources()
    source_api_pattern = re.compile(r"mp-event-source-|mp-event-sources")
    direct_event_pattern = re.compile(
        r"\bnew\s+'[^']+\s+'(?P<type>mp-event|mp-[a-z0-9-]+-event)(?![a-z0-9-])"
    )
    queue_state_pattern = re.compile(
        r"\*mp-event-queue-(?:write|read)-ptr\*|"
        r"\*mp-event-queue-count\*|mp-event-queue-commit!"
    )
    obsolete_send_pattern = re.compile(r"\bmp-send-[a-z0-9-]*event!?\b")
    obsolete_engine_hook_pattern = re.compile(
        r"\*original-send-event-function\*|"
        r"\bmp-event-function-hijack\b|"
        r"\(set!\s+send-event-function\b"
    )

    for path in sources:
        text = path.read_text(encoding="utf-8")
        code = without_line_comments(text)
        if source_api_pattern.search(code):
            errors.append(f"obsolete event source API in {path}")
        if obsolete_send_pattern.search(code):
            errors.append(f"per-event transport sender in {path}")
        if obsolete_engine_hook_pattern.search(code):
            errors.append(f"runtime send-event-function hijack in {path}")
        if path != QUEUE_FILE and queue_state_pattern.search(code):
            errors.append(f"event queue state accessed outside queue module: {path}")
        if path != BUILDER_FILE:
            for match in direct_event_pattern.finditer(code):
                line_start = code.rfind("\n", 0, match.start()) + 1
                line_end = code.find("\n", match.end())
                line = code[line_start:] if line_end < 0 else code[line_start:line_end]
                if (path.name == "mp-event-h.gc" and "*mp-event-buffer*" in line) or (
                    path.name == "mp-waypoint-sync.gc"
                    and "*mp-waypoint-scratch-event*" in line
                ):
                    continue
                errors.append(f"direct event allocation outside builder module: {path}")

    engine_code = without_line_comments(ENGINE_FILE.read_text(encoding="utf-8"))
    if not re.search(
        r"\(define-extern\s+mp-handle-engine-event!\s+"
        r"\(function\s+process-tree\s+event-message-block\s+none\)\)",
        engine_code,
    ):
        errors.append("canonical engine hook declaration is missing from gstate.gc")
    if not re.search(r"\(mp-handle-engine-event!\s+arg0\s+arg1\)", engine_code):
        errors.append("canonical engine hook call is missing from send-event-function")

    hook_code = without_line_comments(HOOK_FILE.read_text(encoding="utf-8"))
    if not re.search(r"\(defun\s+mp-handle-engine-event!\s+", hook_code):
        errors.append("mp-handle-engine-event! definition is missing from mp-event-hooks.gc")

    builder_code = without_line_comments(BUILDER_FILE.read_text(encoding="utf-8"))
    if re.search(r"\bmp-event-enqueue!\b", builder_code):
        errors.append("typed builders must not enqueue events")

    declared = set(
        re.findall(
            r"\(define-extern\s+(mp-event-build-[a-z0-9-]+)\b",
            HEADER_FILE.read_text(encoding="utf-8"),
        )
    )
    defined = set(re.findall(r"\(defun\s+(mp-event-build-[a-z0-9-]+)\b", builder_code))
    for name in sorted(declared - defined):
        errors.append(f"builder declared but not defined: {name}")
    for name in sorted(defined - declared):
        errors.append(f"builder defined but not declared: {name}")

    if errors:
        for error in errors:
            print(f"ERROR: {error}")
        return 1

    print(f"Multiplayer event architecture check passed ({len(defined)} typed builders).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
