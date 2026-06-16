# Multiplayer C++ Networking Layer

This directory contains the PC-side networking bridge for the Jak II multiplayer mod.
The GOAL side owns gameplay extraction/application; this C++ layer owns ENet session
management, packet validation, packet serialization, remote-state caches, and the
`pc-multi-*` functions exposed to GOAL.

The current architecture is compatibility-first: exported GOAL function names,
shared struct layouts, packet structs, packet channels, and reliability flags are
kept stable unless a protocol migration is planned explicitly.

## Runtime Flow

1. GOAL calls `init_multiplayer_pc_port` during runtime setup.
   `multiplayer_api.cpp` registers all exported `pc-multi-*` symbols.

2. GOAL starts hosting, searching, or joining through:
   - `pc-multi-setup-host`
   - `pc-multi-start-search`
   - `pc-multi-setup-client`

   These route through `multiplayer_api.cpp` into `multiplayer_manager.cpp` and
   `multiplayer_scanner.cpp`.

3. Every frame, GOAL calls `pc-multi-poll`.
   `multiplayer_api.cpp` services ENet events, dispatches received packets by
   `PacketType`, updates connection status, and runs stale-cache cleanup.

4. On the network tick, GOAL pushes local state and buffers through functions such as:
   - `pc-multi-send-state`
   - `pc-multi-send-events`
   - `pc-multi-send-enemies`
   - `pc-multi-send-traffic`
   - `pc-multi-send-palace-squid`

   Each API function forwards to a focused sync module.

5. GOAL pulls received state through functions such as:
   - `pc-multi-receive-state`
   - `pc-multi-receive-events`
   - `pc-multi-receive-enemies`
   - `pc-multi-receive-traffic`
   - `pc-multi-receive-palace-squid`

   C++ copies cached remote state into GOAL-owned buffers without changing their
   layout.

## File Responsibilities

### Public Entry Points

- `multiplayer.h`
  Public declarations used by the runtime. Keep this stable unless the runtime
  integration changes.

- `multiplayer.cpp`
  Compatibility anchor kept in the existing build list. Implementation lives in
  focused modules.

- `multiplayer_api.h` / `multiplayer_api.cpp`
  GOAL bridge and packet dispatcher. Owns all `pc_multi_*` functions, GOAL pointer
  conversion guards, symbol registration, ENet polling, and high-level packet
  dispatch by `PacketType`.

### Session And Transport

- `multiplayer_types.h`
  C++ runtime state plus C++ mirrors of GOAL-facing buffers. Structs that mirror
  GOAL layouts must stay byte-compatible with `goal_src/jak2/multiplayer/core/mp-types.gc`.

- `multiplayer_protocol.h`
  Wire packet definitions and packet constants. Changes here can affect network
  compatibility and often require matching serialization changes.

- `multiplayer_session.h` / `multiplayer_session.cpp`
  Owns the singleton `MultiplayerData`, debug receive flag, session reset helpers,
  full-sync state changes, stale-cache cleanup, and receive timeout handling.

- `multiplayer_packet.h` / `multiplayer_packet.cpp`
  Shared packet utilities: quantized float packing, count clamping, `PacketView`
  validation helpers, and safe ENet send wrappers.

- `multiplayer_manager.h` / `multiplayer_manager.cpp`
  ENet host/client lifecycle, disconnect behavior, reliable/unreliable send entry
  points, and host discovery responder.

- `multiplayer_scanner.h` / `multiplayer_scanner.cpp`
  LAN discovery scanner for finding a host. The scanner thread is owned by
  `MultiplayerData` and joined on stop/disconnect.

## Sync Modules

All sync modules live under `sync/` and should keep GOAL ABI compatibility unless
the change is intentionally coordinated with GOAL.

- `sync/player_sync.h` / `sync/player_sync.cpp`
  Local player state send, remote player state receive, turret state, full sync,
  and remote-player prediction into `RemotePlayerInfoGOAL`.

- `sync/event_sync.h` / `sync/event_sync.cpp`
  Reliable game-event packets. Bounds the inbound event queue and copies events
  between C++ packet storage and GOAL `mp-event-buffer`.

- `sync/enemy_sync.h` / `sync/enemy_sync.cpp`
  Enemy sync chunking, packed enemy serialization, remote enemy cache updates,
  and copy-back into GOAL enemy buffers.

- `sync/traffic_sync.h` / `sync/traffic_sync.cpp`
  Shared traffic helpers used by pedestrian and vehicle sync: packet sizing,
  level-hash acceptance/reset, generic slot lookup, and top-level send/receive
  routing.

- `sync/palace_squid_sync.h` / `sync/palace_squid_sync.cpp`
  Palace squid boss state packets and GOAL buffer copy-back.

## Traffic Submodules

- `pedestrian/multiplayer_pedestrian.h` / `pedestrian/multiplayer_pedestrian.cpp`
  Pedestrian traffic packet handling and packed pedestrian serialization.
  Uses shared helpers from `sync/traffic_sync.*`.

- `vehicle/multiplayer_vehicle.h` / `vehicle/multiplayer_vehicle.cpp`
  Vehicle traffic packet handling and packed vehicle serialization.
  Uses shared helpers from `sync/traffic_sync.*`.

## Adding Or Changing Sync Data

For C++-only packet changes:
- Update `multiplayer_protocol.h`.
- Add validation in the receiving sync module before reading payload bytes.
- Use `PacketView` for exact-size or counted-payload checks.
- Keep packet size calculations centralized where possible.

For GOAL-shared buffer changes:
- Update `goal_src/jak2/multiplayer/core/mp-types.gc`.
- Mirror the exact layout in `multiplayer_types.h`.
- Preserve 16-byte alignment and update `static_assert(sizeof(...))`.
- Build with `task build`, and run `task build-mi` if GOAL files changed.

## Safety Rules

- Never read packet payload fields before validating packet size.
- Clamp remote counts before using them in packet-size math or loops.
- Keep inbound queues bounded.
- Check ENet packet allocation and peer state before sending.
- Do not change GOAL-facing struct sizes casually; these structs are raw memory
  bridges.
- Keep `pc-multi-*` names and signatures stable unless GOAL is updated in the
  same change.
