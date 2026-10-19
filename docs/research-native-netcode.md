# Research spike: can iv-coop reuse GTA IV's native multiplayer netcode in the story?

Status: static analysis only (nothing was run or patched). Target: `GTAIV.exe` 1.2.0.59 (image base `0x400000`).
Method: RTTI/strings, vtable walking, the native table (hash -> handler, via FusionFix's `natives.ixx` names),
`dumpbin /disasm`, plus a web search. Scratch scripts are not in the repo.

## Verdict

**Partially / not provable statically.** In short:

| Question | Answer |
|----------|--------|
| Is the original MP netcode still in the CE binary? | **Yes.** All of it is linked in and reachable through vtables: `CNetObjPlayer/Ped/Automobile/Bike/Boat/Heli/Plane/Train`, `CNetworkObjectMgr`, `CNetworkObjectReassignMgr`, `CNetworkEventMgr` + `CNetworkEvent`, `CNetworkBlender` family, ~50 `CNetCloneTask*`, `CNetworkArrayMgrNY`, `CNetworkDamageTracker`, `rage::sn*` session classes, `netConnectionManager`. |
| Can I read how it works statically? | **No, not the core.** `.text` `0x401000-0x4FB000` (250 pages, entropy > 7.3) is encrypted in the file. Every CNetObj*/CNetworkEvent/CNetworkBlender/clone-task virtual function, and the session natives' bodies, live in it. It is presumably decrypted at load by the Rockstar launcher stub, which is out of scope. The rest of `.text` is plain. |
| Do story scripts get killed when a network game is active? | **Not automatically.** Only the explicit native `TERMINATE_ALL_SCRIPTS_FOR_NETWORK_GAME` does it (below). |
| Is there a transport that works in CE? | **Unknown.** GFWL (`xlive`) is gone, but the engine's own `rage::net*` stack (winsock UDP/TCP, `netConnectionManager`, ROS/rline relay) is intact. GTA Connected shows Rockstar sync packets can be carried by a custom relay. |
| Can native sync run during the *story*? | **Plausible but unproven.** The design matches ours (host-owned script objects, ownership reassign). But the engine's "network game" is a separate game mode that tears the SP session down, so we would be forcing a mode the story was never run in. Needs a runtime experiment (see end). |

Recommendation: do **not** drop the M2b-M5 own-sync plan yet. Run the cheap, game-visible experiment first; if it fails,
fall back to reusing only the pieces that don't need a session (see "Fallbacks").

## 1. Class inventory (from RTTI, `.data` type descriptors)

All of these exist: `CNetworkObject`, `CNetObjEntity`, `CNetObjDynamicEntity`, `CNetObjPhysical`, `CNetObjPed`,
`CNetObjPlayer`, `CNetObjDummyPed`, `CNetObjVehicle`, `CNetObjAutomobile`, `CNetObjBike`, `CNetObjBoat`, `CNetObjHeli`,
`CNetObjPlane`, `CNetObjTrain`, `CNetObjObject` (+ `...SyncData` structs), `CNetworkObjectMgr`
(`CAtdNodeNetworkObject`, `CAtdNodeSyncMessageInfo`), `CNetworkObjectReassignMgr`, `CNetworkEventMgr`/`CNetworkEvent`,
`CNetworkArrayMgr(NY)`/`CNetworkArrayHandler`, `CScriptHostVariablesArrayHandler`, `CNetworkBlender` + `CNetBlenderPed/
DummyPed/Physical/LinInterp/Boat/Heli`, `CNetCloneTask*` (BeHit, Die, Dead, Gun, GangDriveby, NewGetInVehicle,
NewExitVehicle, NewUseCover, SimpleCarDrive, SimpleNM*, PlayAnim, PlayScriptAnim, ...), `CTaskSimpleNetworkClone`,
`CNetworkDamageTracker`.

Session/transport: `rage::sn{HostSession,JoinSession,MigrateSession,EstablishSession,ConnectToPeer,JoinGamersToRline,
LeaveGamersFromRline}Task`, `snEventSession{Hosted,Joined,Established,Started,Ended,Destroyed,MigrateStart/End,...}`,
`snMsg{AddGamerToSession,RemoveGamersFromSession,ChangeSessionAttributes}Cmd`, `netMessage`, `netTimeSyncMsg`,
`netComplaintMsg`, `netEvent{ConnectionRequested,Established,Closed,Error,FrameReceived,AckReceived,Reassigned,
RelayAddressChanged}`, `cxn*Msg`, `rlMsgQos*`/`rlMsgSearch*`, `rlPc*`/`rlPresence*` events, `ros.*` messages.
Wire messages: `CMsgCloneSync`, `CMsgPackedCloneSyncAcks`, `CMsgPeerData`.

Notable strings: `Peer-to-Peer Update`, `Update network objects`, `PeerManager.log`, `Bandwidth.csv`,
`RECEIVED_CLONE_SYNC`, `SENDING_CLONE_SYNC`, `Updating on the network update thread` (sync runs on its own thread),
`SCRIPT_RESERVING_{PEDS,VEHICLES,OBJECTS}_FOR_HOST`, `SCRIPT_REGISTERED_HOST_OBJECT`, `Orphaned host object`,
`Reassigned to peer %s`, `Priority %d (==|<|>) ... takes ownership over peer` (ownership migration by priority),
`NETEVENTTYPE_HOST_VARIABLES_VERIFY`, `MP_SESSION_FULL`, `WE_ARE_NEW_HOST`, `SET_NEW_HOST`, `CloneAlways`,
`NoCloneForEnemies`, `Hosting TvT/RENDEZVOUS/STANDARD session`, `NETWORK_PLAYER_1..32`, `NETWORK_TEAM_1..8`.
So host migration, script-host objects and distance-independent clone ownership all exist natively.

## 2. Vtables and the encrypted range

Vtable addresses (RTTI COL -> vtable): `CNetObjPlayer 0xFCCB34`, `CNetObjPed 0xFCB8C4`, `CNetObjAutomobile 0xFCDE2C`,
`CNetObjVehicle 0xFCD2FC`, `CNetworkEvent 0xFCBCCC`, `CNetworkBlender 0xFCB848`, `CNetworkArrayMgrNY 0xFE0BD8`,
`CNetCloneTaskSimpleCarDrive 0xFCCD04`. Every slot points into `0x401000-0x4FB000` (e.g. `CNetObjPlayer` slot 0 =
`0x4B8AF0`, `CNetObjPed` = `0x49C2C0`). Disassembling there yields garbage (checked at `0x47A670`, `0x47C2E0`),
and a block-entropy scan flags exactly that one contiguous range. Consequences:

- The code is present, which is the important fact (not stripped).
- No static xrefs from the sync strings (`RECEIVED_CLONE_SYNC`, ...) to plain code, since the users sit in the encrypted range.
- Further analysis needs the decrypted image at runtime. That crosses into the launcher/DRM boundary (the `.rkstr`/`.tbm`
  sections), so this spike stopped here. Runtime dumping is a decision for the owner, not a default step.

Plain-code islands that **are** readable and relevant: rline/ROS (`ros.rockstargames.com` ref at `0x6DCB96`,
`%s.relay.%s` at `0x6DCBA5`), `[RAGENET] Send` worker setup at `0x6CC863` (the `netConnectionManager` thread; vtable
`0x18E49D8`), and the game-mode state below.

## 3. How a network game starts, and what CE does for transport

Natives (hash from FusionFix `natives.ixx`; handlers found via the native registrar at `0x86E820`/`0x86FD40`):

| Native | Hash | Handler |
|--------|------|---------|
| `NETWORK_HOST_GAME` | `0x5BEA05E2` | `0xBD8630` -> `0xBDAF00` -> `0x47A670` (encrypted) |
| `NETWORK_JOIN_GAME` | `0x60806A0C` | `0xBD89A0` |
| `NETWORK_START_SESSION` / `_PENDING` / `_SUCCEEDED` | `0x58802CE5`/`0x7F853FF4`/`0x5873667B` | `0xBD8E20`/`0xBD8E40`/`0xBD8E60` -> `0x47BBD0/0x47B8D0/0x47B8E0` |
| `NETWORK_LEAVE_GAME` / `_PENDING` | `0x55D66E24`/`0x497E6745` | `0xBD8A70`/`0xBD8A90` |
| `NETWORK_SET_LAN_SESSION` | `0x6FDA43A3` | `0xBD8C50` |
| `NETWORK_IS_NETWORK_AVAILABLE` | `0x04E11812` | `0xBD8820` -> `0xBDB280` |
| `IS_NETWORK_SESSION` | `0x6E2B38F3` | `0xBD7EF0` -> `0x942840` (`mode == 1`) |
| `IS_NETWORK_GAME_RUNNING` / `_PENDING` | `0x1CF773D4`/`0x7563071D` | `0xBD7E90`/`0xBD7E70` -> `0x47C2E0/0x47C330` (encrypted) |
| `NETWORK_IS_SESSION_STARTED` | `0x65B83AFB` | `0xBD8960` |
| `GET_NETWORK_ID_FROM_PED/VEHICLE/OBJECT` | `0x7BEE5003`/`0x1BC70617`/`0x50424095` | `0xBD7AB0`/`0xBD7AD0`/`0xBD7A90` |
| `SET_NETWORK_ID_CAN_MIGRATE`, `_EXISTS_ON_ALL_MACHINES`, `_STOP_CLONING`, `REQUEST_CONTROL_OF_NETWORK_ID` | `0x2FA5601D`, `0x4E2C764D`, `0x086452E7`, `0x29926B20` | `0xBD92F0`, `0xBD9310`, `0xBD9330`, `0xBD8FF0` |

Handlers are thin: they forward to the (encrypted) session layer, so whether `NETWORK_HOST_GAME` still works is only
answerable at runtime.

**Game mode global (plain code):** `0x011D6FC4` = current mode, `0x011D6FC8` = pending mode, `0x011D6FCC` = "change pending" flag.
`0x942720`/`0x942730` read current/pending, `0x942B20` sets pending (+flag), `0x9431D0` sets pending=1 / current=0
(switch toward network), `0x9427D0` commits (current = pending, clears several state blocks, then calls `0x9CB680`,
`0xA2E9E0`, tail-jumps `0x908370`). `IS_NETWORK_SESSION` is `mode == 1`. So "network game" is a distinct game mode
with its own setup/teardown path, called from `0x5A43E2/0x5A540C/0x5A6153/0x5A79E3/0x5C1556` (frontend/game-state code).
Forcing it from inside a running story save is the main unknown.

**Transport.** Imports: no `xlive.dll`, no Steam. `WS2_32` is imported by ordinal: `socket`, `bind`, `sendto`,
`recvfrom`, `send`, `recv`, `select`, `ioctlsocket`, `WSAStartup`, plus `getaddrinfo`. Also `RPCRT4`/`CRYPT32`/`WINTRUST`
(ROS). The rage `net` stack owns its own sockets (`netConnectionManager`, `[RAGENET] Send/Recv`, `netTcp Worker`,
`netThreadPool`), with relay support (`%s.relay.%s`, `cxnRelayAddrChangedMsg`) and ROS endpoints
(`ros.rockstargames.com`, `cloud.rockstargames.com`, title `lanoire`/`pcros`). CE therefore kept the Rockstar Online
Services path rather than GFWL; Rockstar states MP/leaderboards were removed, but the code was not. Whether a session
can be established without a ROS ticket (LAN session via `NETWORK_SET_LAN_SESSION`) is untested.
Because GTA Connected relays "Rockstar's own sync packets" through its own server, the packet format is separable from
the matchmaking, so a replacement transport under `netConnectionManager` is feasible in principle (they must hook
send/receive or the message layer; no details published).

## 4. Scripts not marked safe for network game

- `THIS_SCRIPT_IS_SAFE_FOR_NETWORK_GAME` (`0x63AB65DC`, handler `0xB952B0` -> `0xB975F0`): sets `byte [thread+0x95] = 1`
  on the active thread (`0x94B860` = active GtaThread). Sibling `THIS_SCRIPT_SHOULD_BE_SAVED` (`0xB952C0`) sets `+0x96`.
- `TERMINATE_ALL_SCRIPTS_FOR_NETWORK_GAME` (`0xB95290` -> `0xB975D0` -> `0xB81C60`): loops `sm_Threads`
  (`0x01B4AF04`, count `0x01B4AF08`) and for each live thread with `+0x95 == 0` calls vtable slot `+0x10`
  (terminate); then `0xB82E60`.
- Other readers of `+0x95`: `0xB81C86` (that loop), plus `0x881651`, `0x88197C` (script/program lookup tables, not a
  kill). `0x9A41D5` reads a `+0x95` on a different object (a timed flag), unrelated.

So there is **no automatic reaper**. The MP frontend/script calls `TERMINATE_ALL_SCRIPTS_FOR_NETWORK_GAME` when
entering an MP game. If we start a session ourselves and never call that native, story scripts keep running. (We
would hold the opposite risk: story scripts have never run with `IS_NETWORK_SESSION == true`, and natives gated on mode may
behave differently.) We can also set `+0x95 = 1` ourselves on every story thread cheaply if needed.

## 5. Prior work

- GTA Connected "Native Multiplayer" ([wiki](https://wiki.gtaconnected.com/NativeMultiplayer)): server-only toggle
  (`<iv_nativemultiplayer>`); server relays Rockstar sync packets unchanged, games manage element references; each
  session has a host; players can end up in separate sessions; element IDs come from natives and change on re-stream;
  only suitable for built-in IV gamemodes. It does not cover the single-player story. Their [bug tracker](https://bugs.gtaconnected.com/view.php?id=552)
  also shows the native event layer being driven through a script function, and remote entities being freed/reallocated on re-stream.
- Rockstar [support note](https://support.rockstargames.com/articles/5znA3ag8pAsKuExHi2I3aa/grand-theft-auto-iv-and-grand-theft-auto-episodes-from-liberty-city-updates-to-remove-games-for-windows-live):
  CE removed GFWL, MP and leaderboards from the product (services, not necessarily code).
- No public document of IV's netcode classes or of IV-SDK networking was found; this doc appears to be new ground.

## 6. Risks

1. **Opaque core.** Sync format, ownership rules and the 32-player/16-player assumptions are in encrypted code.
   Debugging it blind is expensive; a bad call can crash.
2. **Game mode switch.** MP mode (`mode==1`) has different setup/teardown (`0x9427D0`), likely loads MP data, ignores the
   SP save/world state and changes ped/vehicle population, wanted level, radar and respawn rules.
3. **Story entities are not network objects.** Scripts create peds/cars with no network ID. They would need
   `GET_NETWORK_ID_FROM_*` registration (natives exist) and `SCRIPT_RESERVING_*_FOR_HOST`-style ownership.
   Our "script owner = host, sim owner = nearest" is close to native `host object` + reassign priority, but the native
   reassign is session-peer based and may not keep script copies authoritative.
4. **Ambient world / streaming.** Native sync has scope ("No peers left in scope"); a no-range-limit design relies on
   `CloneAlways`-like flags and may not behave when peers stream different maps.
5. **Player objects.** `CNetObjPlayer` ties to the original MP player slots/teams, `M_Y_Multiplayer` models and
   frontend-driven lobby; story player is a single protagonist ped.
6. **ROS / DRM / ToS.** Session setup may require a ROS ticket and the encrypted layer is launcher-gated. Do not rely on
   Rockstar services; transport must be fully ours.
7. **FusionFix** hooks nothing in this area, but check before we hook `netConnectionManager`.

## 7. Recommended next experiment (runtime, read-only first)

Goal: decide in one afternoon whether the native session can come up in a story save at all, with no patching.

1. In the ASI (M1 invoker, already works) log these natives once at the main menu and in a loaded save:
   `IS_NETWORK_SESSION`, `NETWORK_IS_NETWORK_AVAILABLE`, `IS_NETWORK_GAME_RUNNING`, `IS_NETWORK_GAME_PENDING`,
   `NETWORK_GET_GAME_MODE`; also read the three mode globals at `0x11D6FC4/8/C`. These show whether the
   encrypted session layer is alive and what state it is in.
2. In a **throwaway save, offline**: call `NETWORK_SET_LAN_SESSION(true)` then `NETWORK_HOST_GAME` (check
   `NETWORK_HOST_GAME_PENDING`/`_SUCCEEDED` each frame), do not call the terminate native, and log mode, script count
   (`sm_Threads`), `GET_NETWORK_ID_FROM_PED(player)` and whether the story scripts keep ticking. Expected failure modes:
   immediate rejection (no ROS), mode switch with world reset, crash. All are informative.
3. If it hosts: a second PC (the launcher allows one copy per machine) with `NETWORK_JOIN_GAME` on LAN, then watch
   whether the remote `CNetObjPlayer` ped appears and moves.
4. Only if 1-3 look promising, consider a runtime hook on `netConnectionManager` send/recv (readable setup at
   `0x6CC863`) to carry packets over ENet; that is the actual unblock for NAT/relay.

Steps 1-2 need no second PC and no hooks.

## 7a. Results

**Run 1 (2026-10-20), step 2: native LAN host inside a loaded story save.** Probe: `src/game/netprobe.cpp`,
`[NetProbe] Enabled=1`, Ctrl+Shift+F12, offline, saves backed up first.

| | Before | After `NETWORK_SET_LAN_SESSION(1)` + `NETWORK_HOST_GAME(16,0,4,0,0,0)` |
|---|---|---|
| `NETWORK_IS_NETWORK_AVAILABLE` | 1 | 1 |
| `NETWORK_HOST_GAME` result / pending / succeeded | — | returned 1, pending 1 → 0 after **8 frames**, succeeded **1** |
| `IS_NETWORK_GAME_RUNNING` / `NETWORK_IS_SESSION_STARTED` | 0 / 0 | **1 / 1** (half a second later) |
| `NETWORK_GET_GAME_MODE` | -1 | 16 |
| `IS_NETWORK_SESSION`, mode globals cur/pend/flag | 0, 0/0/0 | **unchanged** 0, 0/0/0 |
| Script threads | 30 | 30, still running; the game kept ticking normally at ~60 fps |
| `GET_NETWORK_ID_FROM_PED(player)` | — | 0 (player probably not registered as a network object) |

Conclusions:
- The encrypted session layer is alive and hosts a LAN session **offline, with no ROS/Rockstar services**.
- Hosting alone does **not** switch the game into network mode and does not touch the story: we are hosting a
  session while still in single-player mode.
- Unknown: whether objects get cloned in this state (player network id 0 suggests not yet). The MP frontend's next
  step is `NETWORK_START_SESSION`, which may perform the SP→MP mode switch. Run 2 tests that (Ctrl+Shift+F10).

**Run 2 (2026-10-20): `NETWORK_HOST_GAME` again while the run-1 session was still up** (same process, not
restarted). The log stops inside the call; the game went "not responding" with one core spinning and then exited.
So **hosting twice hangs the game thread.** The MP frontend never does this. The probe now refuses to host when a
session exists or is pending, and only allows `NETWORK_START_SESSION` after a successful host. `NETWORK_START_SESSION`
itself is still untested.

**Run 3 (2026-10-20), fresh launch: host once, then `NETWORK_START_SESSION`.**

| Step | Result |
|---|---|
| `NETWORK_HOST_GAME(16,0,4,0,0,0)` | succeeded in 8 frames (as in run 1) |
| `NETWORK_START_SESSION` | returned 1; pending 1 → 0, **succeeded after 2 frames** |
| `GET_NETWORK_ID_FROM_PED(player)` | **0 → 1**: the player became a registered network object |
| `IS_NETWORK_SESSION`, mode globals | still 0, 0/0/0: no SP→MP mode switch |
| Story | 34 script threads before and after, ~79 fps, no world reset, nothing visible for 30 s |

**Status: a full native session (hosted + started, player registered for sync) runs inside a story save,
offline, with no Rockstar services and no patches.** The remaining unknown is whether the clone sync actually
reaches another machine (and how remote players appear) while the game stays in SP mode. That needs a second PC on
the same LAN running `NETWORK_JOIN_GAME` (step 3). The launcher allows one game copy per machine, so this can't be
done on one PC. Join-side probe keys still have to be added (find/join LAN session).

## 8. Fallbacks if the native session is a dead end

- Keep the M2b-M5 own-sync plan (current architecture).
- Reuse *pieces* without a session: native `CNetBlender*` interpolation, `CNetCloneTask*` ped animation for puppets
  (once the encrypted range is understood), and natives like `SET_NETWORK_ID_*` for entity flagging.
- Cheap immediate win independent of netcode: set `thread+0x95 = 1` on story threads to make them tolerant of any
  later native-network experiment.
