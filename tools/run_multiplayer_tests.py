#!/usr/bin/env python3

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import time
from typing import Any

ROOT = Path(__file__).resolve().parents[1]

TARGET_CONFIG: dict[str, dict[str, Any]] = {
    "multiplayer-platform-unit-test": {
        "group": "platform",
        "description": "Platform unit tests",
    },
    "multiplayer-platform-integration-test": {
        "group": "platform",
        "description": "Platform GNS integration tests",
    },
    "multiplayer-jak2-unit-test": {
        "group": "jak2",
        "description": "Jak 2 unit tests",
    },
    "multiplayer-jak2-integration-test": {
        "group": "jak2",
        "description": "Jak 2 integration tests",
    },
}


def executable_name(target: str) -> str:
    return f"{target}.exe" if sys.platform == "win32" else target


def run_target(
    target: str,
    executable: Path,
    json_path: Path,
    extra_args: list[str],
) -> tuple[int, dict[str, Any], list[str]]:
    cmd = [
        str(executable),
        "--gtest_color=yes",
        f"--gtest_output=json:{json_path}",
        *extra_args,
    ]

    print("=" * 80)
    print(f"TARGET: {target}")
    print(f"COMMAND: {' '.join(cmd)}")
    print("=" * 80)

    start_time = time.perf_counter()
    result = subprocess.run(cmd, cwd=str(ROOT))
    elapsed = time.perf_counter() - start_time

    failed_test_names: list[str] = []
    data: dict[str, Any] = {
        "tests": 0,
        "failures": 0,
        "disabled": 0,
        "errors": 0,
        "suites": 0,
        "time": elapsed,
        "crashed": result.returncode != 0 and not json_path.exists(),
    }

    if json_path.exists():
        try:
            raw_text = json_path.read_text(encoding="utf-8")
            if raw_text.strip():
                parsed = json.loads(raw_text)
                data["tests"] = parsed.get("tests", 0)
                data["failures"] = parsed.get("failures", 0)
                data["disabled"] = parsed.get("disabled", 0)
                data["errors"] = parsed.get("errors", 0)
                testsuites = parsed.get("testsuites", [])
                data["suites"] = len(testsuites)

                for suite in testsuites:
                    suite_name = suite.get("name", "")
                    for test in suite.get("testsuite", []):
                        if test.get("failures"):
                            failed_test_names.append(f"{suite_name}.{test.get('name', '')}")
        except Exception as e:
            print(f"Warning: Failed to parse GTest JSON report for {target}: {e}", file=sys.stderr)
    elif result.returncode != 0:
        print(f"Warning: {target} exited with error code {result.returncode} before writing test report.", file=sys.stderr)

    return result.returncode, data, failed_test_names


def main() -> int:
    parser = argparse.ArgumentParser(description="Multiplayer test runner with aggregated summary")
    parser.add_argument(
        "--group",
        choices=["all", "platform", "jak2"],
        default="all",
        help="Target group to run (default: all)",
    )
    parser.add_argument(
        "--bin-dir",
        type=Path,
        default=ROOT / "out" / "build" / "Release" / "bin",
        help="Directory containing test executables",
    )
    args, extra_args = parser.parse_known_args()

    bin_dir: Path = args.bin_dir
    group: str = args.group

    targets_to_run = [
        target
        for target, meta in TARGET_CONFIG.items()
        if group == "all" or meta["group"] == group
    ]

    for target in targets_to_run:
        exe = bin_dir / executable_name(target)
        if not exe.is_file():
            print(f"[ERROR] Missing test executable: {exe}", file=sys.stderr)
            print("Please build binaries first with `task build-release`.", file=sys.stderr)
            return 1

    results: dict[str, dict[str, Any]] = {}
    all_failed_tests: dict[str, list[str]] = {}
    overall_exit_code = 0

    with tempfile.TemporaryDirectory() as temp_dir:
        temp_dir_path = Path(temp_dir)
        for target in targets_to_run:
            exe = bin_dir / executable_name(target)
            json_file = temp_dir_path / f"{target}.json"
            ret, metrics, failed_tests = run_target(target, exe, json_file, extra_args)

            results[target] = metrics
            if failed_tests:
                all_failed_tests[target] = failed_tests

            if ret != 0:
                overall_exit_code = ret

    total_suites = sum(m["suites"] for m in results.values())
    total_tests = sum(m["tests"] for m in results.values())
    total_failures = sum(m["failures"] + m["errors"] for m in results.values())
    total_passed = total_tests - total_failures
    total_time = sum(m["time"] for m in results.values())

    print("\n" + "=" * 80)
    print("                           MULTIPLAYER TEST SUMMARY")
    print("=" * 80)
    print(f"{'Target':<42} {'Suites':>6} {'Tests':>6} {'Passed':>7} {'Failed':>7} {'Time':>7}")
    print("-" * 80)

    for target in targets_to_run:
        m = results.get(target, {})
        suites = m.get("suites", 0)
        tests = m.get("tests", 0)
        failures = m.get("failures", 0) + m.get("errors", 0)
        passed = tests - failures
        elapsed = m.get("time", 0.0)
        print(f"{target:<42} {suites:>6} {tests:>6} {passed:>7} {failures:>7} {elapsed:>6.2f}s")

    print("-" * 80)
    print(f"{'TOTAL':<42} {total_suites:>6} {total_tests:>6} {total_passed:>7} {total_failures:>7} {total_time:>6.2f}s")
    print("=" * 80)

    if all_failed_tests:
        print("\n" + "#" * 80)
        print("FAILED TESTS BREAKDOWN:")
        print("#" * 80)
        for target, tests in all_failed_tests.items():
            print(f"\n  [{target}]")
            for t in tests:
                print(f"    - {t}")
        print("\n" + "#" * 80)

    if overall_exit_code == 0 and total_failures == 0:
        print(f">> ALL {total_tests} MULTIPLAYER TESTS PASSED ({total_suites} SUITES ACROSS {len(targets_to_run)} TARGETS)")
        print("=" * 80 + "\n")
        return 0
    else:
        print(f">> TEST RUN FAILED: {total_failures} FAILURE(S) ENCOUNTERED")
        print("=" * 80 + "\n")
        return overall_exit_code if overall_exit_code != 0 else 1


if __name__ == "__main__":
    raise SystemExit(main())
