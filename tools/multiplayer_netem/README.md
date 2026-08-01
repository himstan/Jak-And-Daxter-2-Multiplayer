# multiplayer-netem

Windows-only opaque UDP relay for testing the local two-instance multiplayer setup at the transport layer. It forwards datagrams between the host game and one client while applying delay, jitter, packet loss, burst loss, reordering, duplication, and deterministic random decisions.

## Project routing

- Host game: `127.0.0.1:26210`
- Discovery: UDP `26211` remains direct between the client and host
- Relay: `127.0.0.1:26212`
- Client game argument: `-mp-client-port 26212`

The relay does not inspect ENet payloads. Handshake packets, reliable traffic, retransmissions, and unreliable sync packets all pass through the same impairment path. Latency is one-way, so a 25 ms profile latency produces approximately 50 ms of ENet round-trip ping before jitter and scheduling effects.

## Normal workflow

From the repository root, use separate terminals:

```powershell
task start-mp-netem NETEM_PROFILE=wifi
task boot-game-netem NETEM_PROFILE=wifi
task boot-game-p2-netem
task stop-mp-netem
```

`boot-game-netem` starts the relay automatically and is safe to use after `start-mp-netem` because the lifecycle helper detects an existing matching process. Ordinary `boot-game` and `boot-game-p2` remain direct, zero-latency paths.

The Taskfile uses the Release executable directory. Build the relay there with:

```powershell
cmake --build .\out\build\Release --target multiplayer-netem --parallel 8
```

## Profiles

Profiles are symmetric by default:

| Profile | One-way latency | Jitter | Loss | Burst length | Reordering | Reorder delay | Duplication |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| `lan` | 1 ms | 0 ms | 0% | 0 | 0% | 0 ms | 0% |
| `wifi` | 25 ms | ±8 ms | 1% | 2 | 0.5% | 15 ms | 0.1% |
| `4g` | 55 ms | ±25 ms | 2% | 3 | 1% | 30 ms | 0.2% |
| `poor-4g` | 100 ms | ±50 ms | 5% | 4 | 2% | 60 ms | 0.5% |
| `stress` | 150 ms | ±75 ms | 10% | 6 | 5% | 100 ms | 1% |

Set a repeatable random sequence with `NETEM_SEED`:

```powershell
task start-mp-netem NETEM_PROFILE=4g NETEM_SEED=42
```

For directional overrides, run the executable directly. `--up-*` applies client-to-host traffic and `--down-*` applies host-to-client traffic:

```powershell
.\out\build\Release\bin\multiplayer-netem.exe `
  --profile wifi `
  --up-loss-percent 5 `
  --down-loss-percent 1 `
  --log-file .\out\build\Release\multiplayer-netem.log
```

Supported directional fields are `latency-ms`, `jitter-ms`, `loss-percent`, `burst-length`, `reorder-percent`, `reorder-delay-ms`, and `duplicate-percent`.

Bandwidth shaping is intentionally excluded from this version.

## Lifecycle and logs

The Taskfile starts the relay hidden and stores its PID in `out/build/Release/multiplayer-netem.pid`. The helper verifies that the PID belongs to the expected `multiplayer-netem.exe` before treating it as the relay or stopping it. Statistics are appended to `out/build/Release/multiplayer-netem.log` once per second and once more during relay shutdown.

If a game or terminal is interrupted, run `task stop-mp-netem`. The relay has bounded delayed-packet queues to avoid unbounded memory growth; defaults are 4096 packets or 4 MiB of payload.

## Tests

The netem unit and loopback integration tests are part of `multiplayer-test`:

```powershell
task build
task multiplayer-tests
```

The tests cover profiles, seeded decisions, burst loss, jitter, reordering, duplication, endpoint routing, queue limits, statistics, and bidirectional disabled-impairment pass-through.
