# steamemu

A drop-in reimplementation of Valve's Steamworks API. It builds to a shared library
(`steam_api.dll` / `steam_api64.dll` on Windows, `libsteam_api.so` on Linux) that a game
loads **instead of** the real one. The emulator satisfies the API surface a game links
against, shims authentication so no real Steam client is required, and uses the **local
network** for peer discovery and transport so that two instances running the emulator can
see and play with each other without Valve's backend.

Status: greenfield. The project directory starts empty; there is no build system, source
tree, or git repo yet. Establish those as the first tasks.

## The one rule that governs everything: match the ABI exactly

A game was compiled against Valve's headers and calls our library through a **frozen binary
interface**. If our layout disagrees with the game's expectations by one byte or one vtable
slot, the game crashes or silently misbehaves. We do not get to redesign the interface — we
reproduce it. Two independent surfaces must both be honored:

1. **Fat C++ interfaces (vtable / COM-style).** Games using the C++ headers call virtual
   methods on `ISteam*` objects (`ISteamUser`, `ISteamFriends`, ...). We must expose objects
   whose **vtable order, method signatures, and calling convention match the headers for the
   exact interface version the game requests.** Method order is the ABI — never reorder,
   insert, or drop a virtual; keep deprecated/`_DEPRECATED` methods in their slots. Inherit
   the pure-virtual class or hand-build a vtable; either way the layout is law.

2. **Flat C API.** Games (and non-C++ bindings) call the exported `SteamAPI_ISteam<Iface>_<Method>(self, ...)`
   C functions declared in `steam_api_flat.h`. These take the interface pointer as the first
   `self` argument and forward to the same implementation. Every flat export must exist with
   the exact name and signature.

Both paths must reach the **same underlying state** (same user, same friends list, same
lobby). Implement the logic once and route both the vtable slot and the flat wrapper to it.

## Interface versioning is by exact string

Interfaces are versioned by string, and the game asks for a specific one. Returning the wrong
version, or `NULL`, breaks initialization. Current strings the reference SDK expects (from the
`*_INTERFACE_VERSION` defines):

```
SteamUser023   SteamFriends018   SteamUtils010   SteamMatchMaking009   STEAMUSERSTATS_INTERFACE_VERSION013
STEAMAPPS_INTERFACE_VERSION009   STEAMREMOTESTORAGE_INTERFACE_VERSION016   STEAMUGC_INTERFACE_VERSION021
SteamNetworking006   SteamNetworkingSockets012   SteamNetworkingUtils004   SteamNetworkingMessages002
STEAMHTTP_INTERFACE_VERSION003   STEAMSCREENSHOTS_INTERFACE_VERSION003   STEAMMUSIC_INTERFACE_VERSION001
SteamInput006   SteamController008   STEAMHTMLSURFACE_INTERFACE_VERSION_005   SteamGameServer015 ...
```

Games do **not** all target the latest version, and this is **ABI-critical for native C++
vtable games**: Valve inserts/removes/retypes methods between versions, so slot N of an older
version may be a different method than slot N of the newest. Handing a game our newest vtable
for an older requested version makes it call the wrong virtual → garbage → "disconnected"/crash.
(Flat-API games call by exported name, not slot — but the interface pointer they hold may still
be a legacy sub-object, which is why the flat wrappers re-anchor it; see below.)

**How we do it:** `tools/oldversions.py` reconstructs
every historical version of each interface from the **`./third_party/SteamworksSDK` git history** (74
commits with v1.00..v1.64 subjects — the repo has no tags; every release's header is a separate
commit), **plus `tools/inter_versions/`**: vendored headers for the 22 versions that shipped
BETWEEN release commits and so never appear in that history's headers (`SteamUser015`,
`SteamFriends010/012/016`, `SteamClient013`, four UGC and four RemoteStorage versions, etc.
`STEAMVIDEO_INTERFACE_V003` reconstructed from the v1.58a release's steam_api.json, which
describes it while that release's header still said V002; vtable order in those files is ABI,
do not edit). It emits `src/generated/versions.h` (each version renamed
`ISteamX_vNNN`; versions whose vtable is provably identical — same method sequence AND same
preprocessor-gate positions — share one class). Each concrete `emu::C<Iface>`
**multiply-inherits the newest interface plus every historical version**, so the compiler emits
a correct sub-vtable per version. Dispatch (`<Iface>_ForVersion(v)` in `registry.cpp`) returns
`(void*)(ISteamX_vNNN*)p` — the middle `(void*)` stops the compiler re-adjusting the pointer —
i.e. the sub-object for the EXACT requested version. A NULL version string means "default"
(real-client behavior; `GetISteamUtils(pipe, NULL)` must not crash), and an unknown/newer string
falls back to the newest **with an `EMU_WARN`** (never NULL for a known interface) — with two
data-driven exceptions in `oldversions.py`: `NULL_VERSIONS` (`SteamController001/002`,
`SteamNetworkingSockets007` — the real steamclient returns NULL for these and games handle it)
and `ALIAS_VERSIONS` (`SteamGameServer006/007` share the 008 vtable). SDK
1.24–1.34's digitless `"STEAMCONTROLLER_INTERFACE_VERSION"` — the only digitless version string
in history, naming a structurally different ISteamController — is extracted and dispatched like
any other version; `FindInterface` matches every distinct stem an interface ever had, not just
the newest one. Curation the raw history needs is automatic: a compat prelude makes old bodies
compile against the current SDK (aliases old annotation macros, forward-declares removed
interfaces/types, and reproduces the historical by-value
`RemoteStorageUpdatePublishedFileRequest_t` struct layout exactly — its size is ABI on 32-bit
Windows where the callee pops it), and the 12 methods whose return type changed across versions
(e.g. `ISteamFriends::SetPersonaName` void→SteamAPICall_t) are renamed `<name>_compat` in the
old body (slot kept) and forwarded, since C++ forbids overriding two same-signature methods that
differ only in return type. Old-only signatures (removed/re-parameterized since) with known
behavior forward to the surviving implementation via `OLD_BODIES` in `tools/oldversions.py`
(auth tickets, CreateLobby, RequestCurrentStats, channel-less P2P) instead of dead neutral
stubs. Extraction **fails the regen hard** on any parse or coverage gap — gen.py passes its
VERSIONS map as a cross-check so the two files cannot drift apart silently;
`STEAMEMU_NEWEST_ONLY=1` is the explicit opt-out for SDK checkouts without git history (every
version string then gets the newest vtable). Two consumers need care:
`SteamInternal_CreateInterface` routes `SteamClient*` strings through `SteamClient_ForVersion`
(the entry interface gets version dispatch too), and every generated flat export re-anchors its
`self` argument via `emu::FlatNormalize()` — a flat binding may hold a legacy sub-object from a
versioned locator, but the flat wrappers are compiled against the newest layout, so they map the
pointer back to the instance's primary interface before dispatching. See
`test/version_dispatch_test.cpp`. Interfaces are located through:

- `SteamInternal_CreateInterface(ver)` — global interfaces.
- `SteamInternal_FindOrCreateUserInterface(hUser, ver)` and the gameserver variant.
- `ISteamClient::GetISteam*(hUser, hPipe, ver)` and the versioned accessors
  `SteamAPI_SteamUser_v023()` etc.

All of these funnel through the `STEAM_DEFINE_INTERFACE_ACCESSOR` macro on the game side, which
caches the pointer via `SteamInternal_ContextInit`. We return a **stable pointer** for a given
(user, version) — the concrete singleton's fixed sub-object, not a fresh object each call.

## Initialization / lifecycle contract

Games initialize one of three ways (`steam_api.h`): `SteamAPI_InitEx` (C++, version-checked via
`SteamInternal_SteamAPI_Init` with a `\0`-delimited version string), `SteamAPI_Init` (wrapper),
or `SteamAPI_InitFlat` (dynamic loaders). We must export all of them, return
`k_ESteamAPIInitResult_OK`, and stand up pipe/user handles (`SteamAPI_GetHSteamPipe`,
`SteamAPI_GetHSteamUser`). Also required: `SteamAPI_Shutdown`,
`SteamAPI_RestartAppIfNecessary` (return `false` so the game keeps running — do not relaunch),
`SteamAPI_ReleaseCurrentThreadMemory`, `SteamAPI_IsSteamRunning` (return `true`).

`steam_appid.txt` in the game's working dir carries the AppID during development; honor it
(and/or the `SteamAppId` env var) instead of requiring the real client.

## Callbacks and call-results — the async ABI

This is the part reimplementations most often get subtly wrong. Two delivery mechanisms:

- **Standard dispatch.** Games create `CCallback` / `CCallResult` objects that call our exported
  `SteamAPI_RegisterCallback` / `SteamAPI_UnregisterCallback` / `SteamAPI_RegisterCallResult` /
  `SteamAPI_UnregisterCallResult`. Nothing is delivered until the game calls
  `SteamAPI_RunCallbacks()` — we queue callbacks and flush them from that call, invoking
  `CCallbackBase::Run` on the registered objects on the calling thread.
- **Manual dispatch.** `SteamAPI_ManualDispatch_*` — the game pumps `CallbackMsg_t` structs itself.
  Support it too; some engines prefer it.

Each callback struct has a unique `k_iCallback` id built from per-interface bases
(`k_iSteamUserCallbacks = 100`, `k_iSteamFriendsCallbacks = 300`, ...; see the enum block in
`steam_api_internal.h`). "Call-results" are async returns keyed by a `SteamAPICall_t` handle
returned from a method now and completed on a later `RunCallbacks`. **Struct layout and packing
matter**: the SDK packs callbacks with `VALVE_CALLBACK_PACK_SMALL/LARGE` (4- or 8-byte) — get
this wrong and the game reads garbage.

## Auth shimming

There is no real Steam backend. Fabricate a plausible identity and make auth calls succeed
locally: assign each instance a `CSteamID`, satisfy `ISteamUser` login state (`BLoggedOn()` →
true, `GetSteamID()` → our id), and stub the ticket/session flow (`GetAuthSessionTicket`,
`BeginAuthSession`, `GetAuthTicketForWebApi`, and the deprecated
`InitiateGameConnection`/`TerminateGameConnection`) to return success and validate peers among
emulator instances rather than against Valve. Do not attempt to defeat VAC or DRM on titles the
user is not authorized to run — this is for local/LAN play and interop testing.

## Peer-to-peer over the local network

The headline feature: two emulator instances discover each other and communicate without Valve.
Plan a small local layer — LAN broadcast/multicast discovery plus a direct socket transport —
that backs the multiplayer-facing interfaces:

- `ISteamNetworkingSockets` / `ISteamNetworkingMessages` / legacy `ISteamNetworking` — carry
  actual game packets between peers.
- `ISteamMatchmaking` (lobbies) — create/join/list lobbies over the LAN; lobby data and member
  lists synced between instances.
- `ISteamFriends` — present other discovered instances as friends/players so invites and
  presence work.

Keep discovery/transport isolated behind an internal interface so the emulated Steam interfaces
call into it and the networking backend can evolve independently.

## Overlay emulation (Dear ImGui)

The real Steam overlay renders in-game (shift-tab, invite dialogs, notifications) by hooking the
game's graphics API. We emulate it with **[Dear ImGui](https://github.com/ocornut/imgui)**: hook
the game's present/swap path (D3D9/10/11/12, OpenGL, Vulkan) and draw our own overlay UI on top.
This backs the interfaces that expect an overlay to exist — `ISteamFriends::ActivateGameOverlay*`,
`ISteamUtils::IsOverlayEnabled`/`BOverlayNeedsPresent`, and the invite/notification flows — so
games that gate features on "is the overlay up?" behave correctly, and LAN peers can be shown,
invited, and joined through it.

Guidance:
- ImGui is a **vendored third-party dependency** — keep it under something like `third_party/imgui`
  and out of the ABI/interface discussion; it never crosses the Steam API boundary.
- Isolate the graphics hooking and ImGui rendering behind an internal overlay module with a
  backend-agnostic interface, mirroring how networking is isolated. The emulated `ISteam*` methods
  call into that module; they must not depend on any specific renderer.
- The overlay is **optional and late** — it depends on hooking the game's real render loop, which
  the headless smoke test can't exercise. Do not let it block the core API/callback work; the
  interfaces above can return sensible values with no overlay drawn until the module lands.

## Reference SDK

The canonical headers live at `./third_party/SteamworksSDK/public/steam/`.
Treat them as read-only ground truth — **do not edit them**. Key files:

- `steam_api.h` — init/shutdown, manual dispatch, top-level exports.
- `steam_api_common.h` — `S_API` linkage macros, `CCallback`/`CCallResult` templates, `HSteamPipe`/`HSteamUser`.
- `steam_api_internal.h` — interface accessor macros, callback id bases, `CallbackMsg_t`, packing.
- `steam_api_flat.h` — the complete flat C export list to reproduce (auto-generated; ~147 KB).
- `steam_api.json` — machine-readable description of every interface, method, callback, and struct
  (~635 KB). Excellent source for code-generating both surfaces; prefer generating over hand-typing.
- `isteam*.h` — one fat interface each; vtable order = declaration order.
- `steamtypes.h` / `steamclientpublic.h` — `CSteamID`, `CGameID`, handle typedefs, enums.
- `lib/` — Valve's prebuilt import libs (reference for export names/ABI).

## Configuration

A user drops a `steamemu.ini` next to the game to set identity and environment; `src/config.cpp`
loads it once per process (`emu::Config()` singleton). It is resolved from, in order: the
`STEAMEMU_CONFIG` env var, then `steamemu.ini` in the cwd, then `steamemu.ini` next to our own
module (the DLL/`.so`, via `GetModuleFileName`/`dladdr`) — so a game launched with a different
working directory still finds the file dropped next to `steam_api64.dll`. The `SteamAPI init` log
line reports which file (if any) was loaded (`Config().LoadedPath()`). Format is a small INI: `[sections]`, `key = value`, `#`/`;` comments,
case-insensitive keys. `steamemu.ini.example` documents every key with examples. For the keys the
emulator has always honored, an **environment variable still wins over the file** (back-compat
with existing setups and the tests):

| Key (file) | Env override | Meaning / default |
|---|---|---|
| `account_id` / `steam_id` | `STEAMEMU_STEAMID` | Local identity: a 32-bit account id, or a value ≥ 2³² is taken as a full 64-bit SteamID. Default account `1000001`. |
| `persona_name` | `STEAMEMU_NAME` | Display name. Default `Player`. |
| `language` | — | Game language (`GetCurrentGameLanguage`). Default `english`. |
| `ui_language` | — | Steam UI language (`GetSteamUILanguage`). Defaults to `language`. |
| `app_id` | `SteamAppId` / `SteamGameId` | AppID override; else `steam_appid.txt`. |
| `country` | — | ISO code for `GetIPCountry`. Default `US`. |
| `log_level` | `STEAMEMU_LOG_LEVEL` | `off`/`error`/`warn`/`info`/`debug`/`trace` (or 0–5). Default `info`. `debug` prints each Steam method once (coverage); `trace` prints every call (exact sequence). |
| `log_file` | `STEAMEMU_LOG` | Log file path; empty = off. The PID is appended (`steamemu.log.4321`) so multi-process games (launcher/anti-cheat/game) don't clobber one file. Config key works even when env vars don't reach the process. |
| `save_path` / `storage_dir` | — | Storage root override; else `steamemu_storage/<appid>`. |
| `[dlc]` `<appid> = <name>` | — | Installed DLC: backs `BIsDlcInstalled` / `GetDLCCount` / `BGetDLCDataByIndex`. |
| `[http]` `<url> = <status> [body]` | — | Canned `ISteamHTTP` responses (`body` inline or `@file`); longest-URL-prefix match. Lets `https://` endpoints succeed without TLS; unlisted requests fail. |
| `[net]` `listen_port` | — | Fixed P2P data port; `0` (default) = ephemeral (required for multi-instance-per-host). |
| `[net]` `discovery_address` / `discovery_port` | — | Multicast discovery group/port. Defaults `239.198.7.4` / `47854`. |

Interface `BODIES` read `emu::Config()` for language/country/DLC; `net.cpp` reads the net
overrides; `storage.cpp` honors `save_path`. String getters return `const char*` borrowed from
Config members (stable for process life).

## Conventions & gotchas

- Exported symbols use `extern "C"` and, on Windows, `__declspec(dllexport)` when building the
  library — build with `STEAM_API_EXPORTS` defined so the `S_API` macro exports rather than imports.
- Preserve Valve's exact export names and (on Windows) calling convention (`S_CALLTYPE` = `__cdecl`).
- Build both 32-bit (`steam_api.dll`) and 64-bit (`steam_api64.dll`); many games are 32-bit.
- Structs crossing the boundary must match Valve's size and packing exactly — verify with
  `static_assert(sizeof(...))` against the reference where feasible.
- Prefer **code-generating** the flat wrappers and vtable plumbing from `steam_api.json` so the
  large, mechanical surface stays consistent and version bumps are cheap.
- When a method's behavior is unknown, the safe default is "succeed with empty/neutral data"
  (return true, zeroed out-params) rather than crash — but log it so gaps are visible.

## Layout

```
tools/gen.py          Code generator: reads steam_api.json and emits src/generated/*.
tools/oldversions.py  Reconstructs every HISTORICAL interface version from the
                      ./third_party/SteamworksSDK git history + tools/inter_versions/ (used by
                      gen.py for per-version vtable dispatch — see "Interface
                      versioning" below). Fails the regen hard on extraction gaps.
tools/inter_versions/ VENDORED headers for the 22 interface versions that shipped
                      between SDK release commits.
                      Vtable order in these files is ABI — do not edit.
src/generated/        GENERATED — do not edit; run `cmake --build build --target regen`.
  interfaces.h          concrete emu::C<Iface> classes; each multiply-inherits the
                        newest interface + every historical version (full vtables).
  versions.h            all historical interface versions, renamed ISteamX_vNNN.
  flat_api.cpp          every SteamAPI_ISteam<Iface>_<Method> flat C export + versioned accessors.
  registry.{h,cpp}      per-interface singletons, <Iface>_ForVersion(v) dispatch,
                        and the FindInterface(version) locator.
src/steam_api.cpp     Exported C entry points: Init/Shutdown lifecycle, the SteamInternal_*
                      interface-location plumbing, callback-dispatch entry points.
src/emu.{h/cpp}        emu_common.h: fixed handles, config-driven identity/AppID/persona,
                      leveled logging (EMU_ERROR/WARN/INFO/DEBUG/TRACE + per-method EMU_LOG).
src/config.{h,cpp}    steamemu.ini parser + emu::Config() singleton: identity, language,
                      country, app_id, save_path, [dlc], net overrides (env still wins).
src/dispatch.{h,cpp}  Callback / call-result dispatch (standard + manual), the async ABI.
src/auth.{h,cpp}      Local auth shimming: fabricated tickets, peer validation, no backend.
src/net.{h,cpp}       LAN backend: multicast discovery + UDP P2P transport, lobby gossip,
                      and connection-oriented ISteamNetworkingSockets.
src/storage.{h,cpp}   Local persistence: ISteamRemoteStorage files + ISteamUserStats stats.
src/services.{h,cpp}  ISteamHTTP (real HTTP client), Music, GameServerStats, Screenshots,
                      HTMLSurface/Timeline handle bookkeeping.
test/harness.cpp      Smoke test: identity round-trip over accessor + vtable paths.
test/api_test.cpp     Behavioral test: flat==vtable, defaults, real callback + auth round-trip.
test/storage_test.cpp Two-phase test: cloud file + stats/achievement persist across runs.
test/http_test.cpp    ISteamHTTP GET against a localhost server; verifies status/body/headers.
test/config_test.cpp  Forked child: a temp steamemu.ini drives identity/language/country/
                      DLC/save_path through the Steam interfaces.
test/lan_test.cpp     Two-instance test: peers discover each other and exchange P2P packets.
test/lobby_test.cpp   Two-instance test: create/list/join a lobby, sync data + members.
test/lobby_rich_test.cpp Two-instance test: lobby member-data, chat, RequestLobbyData, filters.
test/leaderboard_test.cpp Two-phase test: leaderboard create/upload/download, ranking + persist.
test/sockets_test.cpp Two-instance test: ISteamNetworkingSockets connect + message delivery.
test/async_test.cpp   Call-result completion: CCallResult + plain Callback<T> delivery,
                      FileReadAsync byte round-trip, encrypted-app-ticket pair,
                      StartPurchase-must-fail, ISteamUtils polling path.
CMakeLists.txt        Build; STEAMWORKS_SDK points at the reference SDK; `regen` target.
```

The concrete interface classes **inherit the reference `ISteam*` headers and implement every
pure virtual**, so the compiler emits the vtable in the exact slot order the game expects — we
never hand-build a vtable. They additionally **multiply-inherit every historical version** of
the interface (see "Interface versioning is by exact string") so a game that asks for an older
version gets that version's own correct sub-vtable. The whole mechanical surface (29 interfaces,
~900 methods, plus the 913-entry flat C API and the versioned global accessors) is **generated
from `steam_api.json`** by `tools/gen.py`. Real behavior lives in that script's `BODIES` map (keyed by
`Interface::Method`) so regeneration never clobbers it; everything else is a logged neutral
stub — **except methods annotated with a `callresult` struct, whose generated default queues a
zeroed success result (m_eResult/m_result = OK, m_bSuccess = 1) and returns the handle**
(`callresult_default` in gen.py). A promised async result must always complete: a `return {}`
stub hands the game call handle 0 and its CCallResult waits forever — the single most common
"runs but hangs" emulator failure. Game-server interfaces queue on the game-server pipe. A
BODIES key that matches no method **fails the regen** (it means a typo/rename silently reverted
a method to a stub — this guard caught a shipped `IsAPICallCompleted` stub that returned false
forever). `flat_api.cpp` `#include`s the reference `steam_api_flat.h`, so the compiler checks every
flat definition against Valve's prototype — a build-time ABI guard.

Generator gotchas it already handles (see comments in `gen.py`): methods hidden behind
`STEAM_PRIVATE_API` occupy vtable slots but are absent from the JSON (`EXTRA_METHODS`); a family
of inline non-virtual helpers on `ISteamNetworkingUtils` must NOT be overridden
(`NONVIRTUAL_HELPERS`); the JSON renders the two `*_ToString` `size_t` slots as `uint32`
(corrected in `EXTRA_METHODS`); `ISteamNetworkingFakeUDPPort` is only forward-declared in the
public SDK (flat exports are neutral stubs); two networking interfaces need an out-of-line
destructor definition.

## Build & run

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug   # add -DSTEAMWORKS_SDK=/path if not ./third_party/SteamworksSDK
cmake --build build
cmake --build build --target regen              # re-run tools/gen.py after a version bump
ctest --test-dir build --output-on-failure      # identity_roundtrip + api_behavior
./build/harness ; ./build/api_test              # or run them directly
```

Current state: **full API surface up, async ABI live, LAN play working — validated against a
real commercial title.**
Call-results are ALSO delivered as
plain callbacks (games hook `Callback<LobbyCreated_t>`, not just the CallResult); delivery
carries realistic latency (instant callbacks run handlers before games have prepared their
data); the game-server side of one process has its own pipe, its own manual-dispatch queue,
its own P2P identity (`emu::LocalGSSteamID()`) and its own receive inbox; and a hand-written
`compat_flat.cpp` supplies flat exports that newer SDKs deleted but older games still P/Invoke
(a missing export kills managed init before our code ever runs).

The complete flat +
vtable surface for 29 interfaces is generated and exported (913 flat exports + versioned
accessors, all matching the reference). Callback/call-result dispatch is real: registrations
queue, `RunCallbacks()` / manual dispatch flush them on the calling thread. Auth is shimmed
locally (fabricated tickets carry the issuer's `CSteamID`; peers validate each other, no Valve
backend). Identity, AppID, persona, language, country, installed DLC, save path and networking
are **config-driven** via `steamemu.ini` (`emu::Config()`), with env vars still winning for the
legacy keys — see the **Configuration** section above and `steamemu.ini.example`.

The **LAN backend is live**: two instances discover each other via an IP-multicast beacon
(loopback for same-host, so it is testable in CI) and exchange game packets over UDP. It backs
legacy `ISteamNetworking` P2P, the modern connection-oriented `ISteamNetworkingSockets`
(listen/connect handshake, `SteamNetConnectionStatusChangedCallback_t`, message objects, poll
groups), `ISteamFriends` presence (discovered peers appear as friends), and `ISteamMatchmaking`
lobbies (create/list/join, lobby-data + member-list sync via a gossip protocol, plus per-member
data, lobby chat, `RequestLobbyData`, and list filters).

**Persistence is real**: `ISteamRemoteStorage` reads/writes actual files, `ISteamUserStats`
stores stats + achievements, and leaderboards (find/create/upload/download) persist under
`leaderboards/` — all under `steamemu_storage/<appid>/` (or the configured `save_path`) and
surviving across runs.

**Every one of the 28 interfaces now carries real behavior** — ~320 methods have explicit
implementations (see `BODIES` in `tools/gen.py`); the rest use the documented "succeed with
empty/neutral data" default, which for those methods (getters with no data, feature-gate
queries) is the correct answer. Highlights beyond the LAN/persistence work above:

- **`ISteamHTTP`** — a real HTTP/1.1 client over TCP (`services.cpp`): resolves, connects,
  sends, and parses status/headers/body; `SendHTTPRequest` completes with `HTTPRequestCompleted_t`.
  `https://` has no TLS, so it fails by default — but a `[http]` config section supplies **canned
  responses** (status + inline/`@file` body, longest-URL-prefix match) so known https endpoints
  (auth, config, telemetry) return the exact response a game expects, no sockets involved.
- **`ISteamMusic`** — a playback state machine; **`ISteamGameServerStats`** — per-user stat
  store; **`ISteamScreenshots`** — writes captures to disk; **`ISteamParentalSettings`** —
  everything unlocked; **`ISteamNetworkingUtils`** — real timestamps, config store, message
  allocation, IP/identity formatting; **`ISteamMatchmakingServers`** — LAN browse completes empty;
  **`ISteamUGC`** — query lifecycle + item state + create/submit/subscribe callbacks;
  `ISteamHTMLSurface`/`ISteamTimeline`/`ISteamParties`/`ISteamRemotePlay`/`ISteamVideo` return
  valid handles / empty-but-correct state.

**The async-hang class is closed** (HANDOFF.md, all four tasks done): every
`SteamAPICall_t`-returning method completes — stubs auto-queue a zeroed success result via the
generator (`callresult_default`), curated methods carry real data (`FileReadAsync` real bytes,
`FileShare`/`UGCDownload` plausible handles, encrypted-app-ticket pair via `emu::Auth()`,
`GetNumberOfCurrentPlayers` = 1, `StartPurchase` deliberately fails, global-stats getters answer
"received but empty"). Poll-loop getters games spin on are answered: placeholder **avatars**
(stable handle per user/size, solid-color RGBA in `services.cpp`), `GetAppInstallDir` (cwd),
`GetUserDataFolder` (`<storage>/user`), `GetConnectionRealTimeStatus` (connected, 1ms ping from
the LAN backend), `GetPublicIP` (real LAN IPv4 via `emu::LanIPv4()`), voice reports
not-recording, `IsAPICallCompleted`/`GetAPICallResult` work through the ISteamUtils vtable, and
the `ISteamNetworkingUtils` config store round-trips values (the typed helpers funnel into it).
Wrong-zero enums audited (`EResult{}` = None ≠ OK; `GetSyncPlatforms` → All; SDR/FakeIP getters
fail honestly).

Fifteen tests cover it end-to-end (`ctest --test-dir build`): `harness`, `api_test`,
`async_test`, `version_dispatch_test`, `shutdown_test`, `manual_dispatch(+result)_test`,
`storage_test`, `http_test`, `config_test`, `lan_test`, `lobby_test`, `lobby_rich_test`,
`leaderboard_test`, `sockets_test`. The only interface
feature that cannot be made functional here is the overlay / `ISteamHTMLSurface` browser (needs a
real renderer); real `https://` transport would need a TLS library, but the `[http]` mock section
covers the common case, so it is only genuine live/streaming HTTPS that remains out of scope.

This host has only a native 64-bit Linux toolchain, so only `libsteam_api.so` (64-bit) builds
here. 32-bit (`-m32`, needs multilib) and Windows (`steam_api.dll`/`steam_api64.dll`, via MinGW
or MSVC) targets require the matching toolchain; CMake already picks the correct output name per
platform/arch.

Richer lobbies and leaderboards are now **done**:
- **Per-member data** (`SetLobbyMemberData`/`GetLobbyMemberData`) rides the gossip presence
  message (each member's own KV blob after the owner's lobby-data blob), **lobby chat**
  (`SendLobbyChatMsg` → multicast → `LobbyChatMsg_t` → `GetLobbyChatEntry`), **`RequestLobbyData`**
  for lobbies you have not joined (marks the lobby "watched" so its `LobbyDataUpdate_t` fire), and
  **`RequestLobbyList` filters** (string/numeric/slots/result-count, applied then cleared per
  Steam semantics; `GetLobbyByIndex` indexes the filtered snapshot, with a live-scan fallback
  before the first request). Covered by `test/lobby_rich_test.cpp`.
- **Leaderboards** in `ISteamUserStats` (find/create/upload/download/get) persist under
  `<storage>/leaderboards/`; see `emu::Leaderboards()` in `storage.cpp` and
  `test/leaderboard_test.cpp`.

### Next steps (deepening existing behavior)
1. Live HTTPS transport in the `ISteamHTTP` client (needs a TLS library) and streaming responses.
   Canned https responses already work via the `[http]` config section (see **Configuration**).
2. Overlay (Dear ImGui) — optional/late; can't be exercised headless. Backs the
   `ISteamHTMLSurface` browser and `ActivateGameOverlay*` once a render hook lands.

Note on the LAN backend design: it uses an eventually-consistent gossip model (each lobby member
periodically multicasts its presence; the owner's presence carries authoritative lobby data plus
that member's own per-member data), so there is no join handshake that can fail. Peers/lobby-
members time out after a few seconds of silence. Discovery is multicast group `239.198.7.4:47854`
(overridable via config); P2P data is direct unicast UDP to a peer's beacon-announced port (also
learned from received datagrams).
