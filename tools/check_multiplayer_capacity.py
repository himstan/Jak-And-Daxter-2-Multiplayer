"""Verify that the GOAL and C++ multiplayer capacities remain ABI-compatible."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
GOAL_CAPACITY = ROOT / "goal_src" / "jak2" / "multiplayer" / "core" / "mp-capacity-h.gc"
CPP_CAPACITY = ROOT / "game" / "multiplayer" / "multiplayer_protocol.h"
MIN_PLAYERS = 2
MAX_PLAYERS = 32


def read_capacity(path: Path, pattern: str, language: str) -> int:
    text = path.read_text(encoding="utf-8")
    matches = re.findall(pattern, text)
    if len(matches) != 1:
        raise ValueError(f"expected exactly one {language} player-capacity definition in {path}")
    return int(matches[0])


def main() -> int:
    try:
        goal_capacity = read_capacity(
            GOAL_CAPACITY, r"\(defconstant\s+MP_MAX_PLAYERS\s+(\d+)\)", "GOAL"
        )
        cpp_capacity = read_capacity(
            CPP_CAPACITY,
            r"inline\s+constexpr\s+uint32_t\s+kMPMaxPlayers\s*=\s*(\d+)\s*;",
            "C++",
        )
    except (OSError, ValueError) as error:
        print(f"multiplayer capacity check failed: {error}", file=sys.stderr)
        return 1

    if not MIN_PLAYERS <= goal_capacity <= MAX_PLAYERS:
        print(
            f"multiplayer capacity check failed: MP_MAX_PLAYERS={goal_capacity}; "
            f"expected {MIN_PLAYERS}-{MAX_PLAYERS}",
            file=sys.stderr,
        )
        return 1
    if goal_capacity != cpp_capacity:
        print(
            "multiplayer capacity check failed: "
            f"GOAL MP_MAX_PLAYERS={goal_capacity}, C++ kMPMaxPlayers={cpp_capacity}",
            file=sys.stderr,
        )
        return 1

    print(f"multiplayer capacity check passed: {goal_capacity} players")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

