#!/usr/bin/env python3

from __future__ import annotations

import argparse
import os
import re
import shlex
import socket
import subprocess
import sys
import time
from pathlib import Path
from typing import List, Sequence


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_CAPACITY_HEADER = Path("goal_src/jak2/multiplayer/core/mp-capacity-h.gc")
CAPACITY_PATTERN = re.compile(r"\((?:defconstant|defglobalconstant)\s+MP_MAX_PLAYERS\s+(\d+)\)")


class LauncherError(RuntimeError):
    pass


def resolve_repository_path(path: Path) -> Path:
    if path.is_absolute():
        return path
    return REPO_ROOT / path


def parse_peer_count(value: str) -> int:
    try:
        peer_count = int(value.strip())
    except (AttributeError, ValueError) as error:
        raise LauncherError(
            "PEERS must be an integer of at least 1. "
            "Example: task boot-game-peers PEERS=7"
        ) from error

    if peer_count < 1:
        raise LauncherError(
            "PEERS must be an integer of at least 1. "
            "Example: task boot-game-peers PEERS=7"
        )
    return peer_count


def read_multiplayer_capacity(header_path: Path) -> int:
    if not header_path.is_file():
        raise LauncherError(f"Multiplayer capacity header was not found: {header_path}")

    matches = CAPACITY_PATTERN.findall(header_path.read_text(encoding="utf-8"))
    if len(matches) != 1:
        raise LauncherError(
            f"Expected exactly one numeric MP_MAX_PLAYERS definition in {header_path}"
        )
    return int(matches[0])


def validate_listener_ports(ports: Sequence[int]) -> None:
    listeners: List[socket.socket] = []
    try:
        for port in ports:
            listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            if os.name == "nt" and hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
                listener.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            try:
                listener.bind(("0.0.0.0", port))
                listener.listen(1)
            except OSError as error:
                listener.close()
                raise LauncherError(
                    f"Cannot launch peers because listener port {port} is occupied."
                ) from error
            listeners.append(listener)
    finally:
        for listener in listeners:
            listener.close()


def peer_arguments(port: int, game: str, host_address: str) -> List[str]:
    return [
        "--port",
        str(port),
        "-v",
        "--game",
        game,
        "--",
        "-boot",
        "-debug",
        "-fakeiso",
        "-mp-client",
        host_address,
    ]


def format_command(arguments: Sequence[str]) -> str:
    if os.name == "nt":
        return subprocess.list2cmdline(arguments)
    return shlex.join(arguments)


def process_options() -> dict:
    if os.name == "nt":
        return {"creationflags": subprocess.CREATE_NEW_CONSOLE}
    return {"start_new_session": True}


def stop_launched_processes(processes: Sequence[subprocess.Popen]) -> None:
    running_processes = [process for process in processes if process.poll() is None]
    for process in running_processes:
        try:
            process.terminate()
        except OSError:
            pass

    for process in running_processes:
        if process.poll() is not None:
            continue
        try:
            process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            try:
                process.kill()
            except OSError:
                pass


def launch_peers(
    executable: Path,
    game: str,
    host_address: str,
    ports: Sequence[int],
    launch_delay_ms: int,
    dry_run: bool,
) -> None:
    launched_processes: List[subprocess.Popen] = []
    failed_peer = 0
    failed_port = 0

    try:
        for peer_number, port in enumerate(ports, start=1):
            failed_peer = peer_number
            failed_port = port
            arguments = [str(executable), *peer_arguments(port, game, host_address)]

            if dry_run:
                print(f"[Peer Launcher] Peer {peer_number} port={port}")
                print(f"[Peer Launcher] {format_command(arguments)}")
                continue

            process = subprocess.Popen(
                arguments,
                cwd=REPO_ROOT,
                **process_options(),
            )
            launched_processes.append(process)
            print(
                f"[Peer Launcher] Started peer {peer_number} "
                f"pid={process.pid} port={port}",
                flush=True,
            )

            time.sleep(launch_delay_ms / 1000.0)
            exit_code = process.poll()
            if exit_code is not None:
                raise LauncherError(
                    f"Peer {peer_number} exited during startup with code {exit_code}."
                )
    except (LauncherError, OSError) as error:
        stop_launched_processes(launched_processes)
        raise LauncherError(
            f"Failed to launch peer {failed_peer} on port {failed_port}. {error}"
        ) from error

    mode = "Dry run complete for" if dry_run else "Launched"
    print(f"[Peer Launcher] {mode} {len(ports)} peer(s).")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Launch multiple OpenGOAL multiplayer client peers."
    )
    parser.add_argument("--peers", required=True)
    parser.add_argument("--executable", required=True, type=Path)
    parser.add_argument("--game", required=True)
    parser.add_argument("--host-address", default="127.0.0.1")
    parser.add_argument("--base-port", type=int, default=8120)
    parser.add_argument("--launch-delay-ms", type=int, default=250)
    parser.add_argument("--capacity-header", type=Path, default=DEFAULT_CAPACITY_HEADER)
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args()


def run() -> None:
    arguments = parse_arguments()
    peer_count = parse_peer_count(arguments.peers)
    executable = resolve_repository_path(arguments.executable).resolve()
    capacity_header = resolve_repository_path(arguments.capacity_header).resolve()

    if not executable.is_file():
        raise LauncherError(f"Game executable was not found: {executable}")
    if not arguments.game.strip():
        raise LauncherError("Game name must not be empty.")
    if not arguments.host_address.strip():
        raise LauncherError("Host address must not be empty.")
    if not 1 <= arguments.base_port <= 65535:
        raise LauncherError("Base port must be between 1 and 65535.")
    if arguments.launch_delay_ms < 0:
        raise LauncherError("Launch delay must not be negative.")

    multiplayer_capacity = read_multiplayer_capacity(capacity_header)
    maximum_peers = multiplayer_capacity - 1
    if peer_count > maximum_peers:
        raise LauncherError(
            f"PEERS={peer_count} exceeds the compiled client capacity of {maximum_peers}."
        )

    last_port = arguments.base_port + peer_count - 1
    if last_port > 65535:
        raise LauncherError(
            f"Requested peer range ends at invalid listener port {last_port}."
        )

    ports = list(range(arguments.base_port, last_port + 1))
    validate_listener_ports(ports)
    launch_peers(
        executable,
        arguments.game,
        arguments.host_address,
        ports,
        arguments.launch_delay_ms,
        arguments.dry_run,
    )


def main() -> int:
    try:
        run()
        return 0
    except LauncherError as error:
        print(f"[Peer Launcher] Error: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
