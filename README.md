# steamemu

[![CI](https://github.com/partyessenist/steamemu/actions/workflows/ci.yml/badge.svg)](https://github.com/partyessenist/steamemu/actions/workflows/ci.yml)
[![Release](https://img.shields.io/github/v/release/partyessenist/steamemu?include_prereleases&sort=semver)](https://github.com/partyessenist/steamemu/releases/latest)

A drop-in reimplementation of Valve's Steamworks API. It builds to a library that a game
loads **instead of** the real one — `steam_api.dll` / `steam_api64.dll` on Windows,
`libsteam_api.so` on Linux — and satisfies the Steam API surface the game links against, so
the game runs with **no Steam client and no Valve backend**. Two instances on a LAN discover
each other and play together over the local network.

It's for local / LAN play and interop testing of games you own. It does **not** defeat DRM,
VAC, or ownership checks on titles you aren't authorized to run.

> Developer/architecture docs (ABI, code generation, interface versioning) live in
> [`CLAUDE.md`](CLAUDE.md). This file is for **using** the emulator to run a game.

---

## Quick start

1. **Get the library** for your game's platform/arch — `steam_api64.dll` (64-bit Windows),
   `steam_api.dll` (32-bit Windows), or `libsteam_api.so` (64-bit Linux). Build it (see
   [Building](#building-from-source)) or use a prebuilt one.
2. **Back up the game's original** Steam library, then **replace it** with ours. It usually
   sits next to the game executable (e.g. `.../MyGame/bin/steam_api64.dll`).
3. **Tell it the AppID.** Either drop a `steam_appid.txt` containing just the numeric AppID
   next to the game exe, or set the `SteamAppId` environment variable, or put `app_id =` in
   `steamemu.ini` (below).
4. **Launch the game.** It should start without Steam running.

That's the whole install for a single-player title. For identity, DLC, LAN multiplayer, and
diagnostics, add a `steamemu.ini`.

### If the game exits immediately or says "Steam is required"

Some games check `SteamAppId` (or the parent process) **before** loading our library. Launch
via a small script that sets the env first and runs from the exe's own folder:

```bat
:: start.bat  (Windows)
set SteamAppId=534380
start "" MyGame.exe
```

---

## Configuration — `steamemu.ini`

Drop a file named `steamemu.ini` next to the game's `steam_api` library (or in its working
dir). It's found in this order: the `STEAMEMU_CONFIG` env var (explicit path) →
`steamemu.ini` in the working directory → `steamemu.ini` next to the DLL/`.so`. The startup
log line reports which file (if any) was loaded.

Format is a small INI: `[sections]`, `key = value`, `#`/`;` comments, case-insensitive keys,
everything optional. **[`steamemu.ini.example`](steamemu.ini.example) documents every key with
examples** — copy it and edit. The essentials:

| Key | Meaning | Default |
|---|---|---|
| `account_id` | Your fabricated identity as a 32-bit account id … | `1000001` |
| `steam_id` | … or a full 64-bit SteamID instead (takes precedence). Must not be an account-id-0 value. | — |
| `persona_name` | Display name shown to LAN peers. | `Player` |
| `language` | Game language (`GetCurrentGameLanguage`). | `english` |
| `ui_language` | Steam UI language. | = `language` |
| `country` | ISO code for `GetIPCountry`. | `US` |
| `app_id` | AppID override (else `steam_appid.txt` / `SteamAppId`). | — |
| `log_level` | `off`/`error`/`warn`/`info`/`debug`/`trace` (or `0`–`5`). | `info` |
| `log_file` | Log file path (PID is appended). Empty = off. | off |
| `save_path` | Storage root for saves/stats. `storage_dir` is an alias. | `steamemu_storage/<appid>` |
| `[dlc]` `<appid> = <name>` | Installed DLC — backs `BIsDlcInstalled`/`GetDLCCount`. | none |
| `[http]` `<url> = <status> [body]` | Canned `ISteamHTTP` responses (no TLS needed), longest-prefix match. | none |
| `[net]` `listen_port` | Fixed P2P UDP port; `0` = ephemeral (**required** for two instances per host). | `0` |
| `[net]` `discovery_address` / `discovery_port` | LAN multicast discovery group/port; both peers must match. | `239.198.7.4` / `47854` |

Minimal example:

```ini
account_id   = 1000001
persona_name = Alice
language     = english
app_id       = 480

[dlc]
12345 = Deluxe Pack
```

---

## Environment variables

For the legacy keys, an **environment variable wins over the ini file** (back-compat). The
newer ini-only keys (language, country, `save_path`, `[dlc]`, `[http]`, `[net]`) have no env
override.

| Variable | Effect | Overrides ini key |
|---|---|---|
| `STEAMEMU_STEAMID` | Identity: 32-bit account id, or ≥ 2³² = full 64-bit SteamID. | `account_id` / `steam_id` |
| `STEAMEMU_NAME` | Display / persona name. | `persona_name` |
| `SteamAppId` | AppID. | `app_id` |
| `SteamGameId` | AppID fallback (only if `SteamAppId` unset). | `app_id` |
| `STEAMEMU_CONFIG` | Full path to the `steamemu.ini` to load (checked first). | — |
| `STEAMEMU_LOG` | Log file path (PID appended). | `log_file` |
| `STEAMEMU_LOG_LEVEL` | Log verbosity (`off`…`trace` or `0`–`5`). | `log_level` |

At startup the emulator also **publishes** some Steam launch vars into the process
environment so games that check them behave (`SteamAppId`, `SteamGameId`,
`SteamOverlayGameId`, `SteamAppUser`, `SteamUser`, `SteamClientLaunch=1`, `SteamEnv=1`, and
`SteamPath` if unset). You don't set these — they're outputs.

---

## LAN multiplayer

Two instances running the emulator on the same LAN find each other automatically — no
configuration needed beyond a matching discovery group. Discovery is an IP-multicast beacon
(`239.198.7.4:47854` by default; also works over loopback, so two instances on one PC can
play). It backs:

- **Lobbies** (`ISteamMatchmaking`) — create / list / join, with lobby + member data, chat,
  and list filters synced between instances.
- **P2P transport** — legacy `ISteamNetworking` and modern `ISteamNetworkingSockets` carry
  the actual game packets over UDP.
- **Presence** (`ISteamFriends`) — discovered peers appear as friends so invites/joins work.

Running **two instances on one host?** Give each a different identity (`account_id`) and
leave `[net] listen_port = 0` (ephemeral) so they don't fight over a port.

Peers time out after a few seconds of silence; there's no join handshake that can fail.

---

## Logging & troubleshooting

Set a log file and level to see what the game asks for:

```ini
log_file  = steamemu.log
log_level = debug     ; each Steam method the game calls, once (coverage)
                      ; use `trace` for EVERY call in exact order (diagnosing a hang)
```

The PID is appended to the filename (`steamemu.log.4321`), so a launcher  and the
game each get their own file — open the largest, which is usually the gameplay process.
Setting `log_file` in the ini (rather than `STEAMEMU_LOG`) is the reliable way for
anti-cheat-relaunched games whose process may not inherit your environment.

Common issues:

- **Game exits instantly / "Steam required":** the `SteamAppId` env gate — see
  [the start.bat note](#if-the-game-exits-immediately-or-says-steam-is-required).
- **No `steamemu.log` at all:** the game never called us — it may not have loaded our library
  (wrong file replaced, or it bailed at an env check first).
- **Runs but hangs waiting on something async:** turn on `trace` and check the last calls; a
  method that returns a `SteamAPICall_t` must complete (this emulator completes them).
- **`https://` request fails:** there's no TLS stack; stub the endpoint under `[http]` in the
  ini (see the sample) to serve a canned response with no network.

---

## Building from source

Needs a C++ toolchain, CMake, and Valve's Steamworks SDK headers (pull the submodule).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug   # add -DSTEAMWORKS_SDK=/path if not ./third_party/SteamworksSDK
cmake --build build
ctest --test-dir build --output-on-failure     # run the test suite
```

CMake picks the correct output name per platform/arch automatically. Build **both** 32-bit
(`steam_api.dll`) and 64-bit (`steam_api64.dll`) for Windows — many games are 32-bit — using
the matching toolchain (MinGW or MSVC); 32-bit Linux needs multilib. If you change the target
Steamworks SDK version, regenerate the mechanical surface:

```sh
cmake --build build --target regen
```

---

## What works, and what doesn't

Working: full flat + vtable API surface for 29 interfaces with per-version dispatch; real
callback / call-result delivery; locally-shimmed auth (fabricated tickets, peers validate
each other); LAN discovery, P2P, lobbies, and presence; persistent cloud saves, stats,
achievements, and leaderboards; a real `ISteamHTTP` client (plus `[http]` mocks for
`https://`); config-driven identity/AppID/DLC/language/country; and an opt-in **Dear ImGui
game overlay** (Windows) that draws over the game via a D3D11 swapchain hook.

The overlay is opt-in — set `[overlay] enabled = 1` in `steamemu.ini` (Windows only) and
toggle it in-game with the configured hotkey (default `shift+tab`). It shows your identity,
discovered LAN players and lobbies, and invite/notification toasts, and backs
`ISteamFriends::ActivateGameOverlay*`, `ISteamUtils::IsOverlayEnabled` /
`BOverlayNeedsPresent`, and the `GameOverlayActivated_t` pause/resume signal.

Out of scope / limitations:

- **Overlay backends** — only Direct3D 11 is hooked so far (games on D3D9/10/12, OpenGL or
  Vulkan fall back to a separate overlay window). Overlay is Windows-only.
- **Live `https://` transport** — no TLS library; use the `[http]` canned-response section
  for known endpoints. Genuine live/streaming HTTPS is unsupported.
- Anything requiring Valve's real backend (server-side ticket validation, real matchmaking
  across the internet) — this is LAN/local only by design.

Behavior for any given game depends on which interfaces and versions it uses.

---

## Legal / scope

For running and testing games **you own**, locally or on your LAN. Don't use it to circumvent
DRM/anti-cheat on titles you aren't authorized to run, or to enable piracy.
