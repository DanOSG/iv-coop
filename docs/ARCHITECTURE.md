# iv-coop architecture

**4-player story co-op** for **GTA IV: The Complete Edition**, `GTAIV.exe` **1.2.0.59** (32-bit), running
alongside FusionFix and Ultimate ASI Loader. The goal is **full campaign compatibility**: the host plays the story
as Niko (or Johnny/Luis in EFLC), and up to 3 friends join as their own characters. **There is no range limit.**
Joiners can roam the whole map, drive their own cars, or ride as passengers next to story characters when there
is a free seat. Where a section really can't include other players, we make a deliberate, documented concession
(below) rather than break the mission.

Free-roam is not a separate mode. It's simply the campaign between missions.

## Core principles

1. **The host is the only story authority.** Mission and ambient-story scripts (`.sco`) run only on the host,
   along with saves, progression, the phone, contacts and friend activities.
2. **Script authority is separate from simulation ownership.** With no range limit, a mission entity can be on
   the far side of the map from the host. The host's engine has no collision streamed there and would freeze or
   drop the entity. So every networked entity has:
   - a **script owner**: always the host, whose copy is the one scripts read and write;
   - a **sim owner**: whichever player is nearest/streaming it, who runs its physics and AI and streams the
     state back. The host's copy follows it (frozen, collision off, positioned from snapshots).
   Sim ownership migrates as players move. This is the core mechanism of the whole mod, so it is built early.
3. **Clients mirror; they don't decide.** Clients run no story scripts. Everything story-visible (blips, objectives,
   HUD text, subtitles, dialogue, cutscenes, wanted level from missions) reaches them as replicated events/state.
4. **Hard cases fall back to spectating.** If a section can't take extra players, they get a host-attached
   spectator camera for that section and rejoin afterwards. They never block the host.

## Shape

- **One `.asi` plugin** (`plugins/iv-coop.asi`), C++20, MSVC, Win32. The same build serves host and client; the
  role is chosen in the in-game menu.
- **Listen server, 4 co-op players max (8 with invaders, M11).** ENet over UDP (reliable + unreliable channels) behind a `Transport`
  interface. The first version connects by direct IP or port forward; Steam relay or NAT punch-through come later.
- **Peer-hosted only: no dedicated servers, ever.** A session always lives inside the host player's game. To get
  through home routers without manual setup:
  1. **UPnP/NAT-PMP** port mapping when hosting: automatic and serverless, and it works on most home routers.
  2. **Steam (app 12210):** the game itself runs Steam as GTA IV in our process. Its `steam_api.dll` exports
     `ISteamMatchmaking` (lobbies) and the legacy relayed P2P `ISteamNetworking` (`SendP2PPacket`); newer
     interfaces may be reachable from the Steam client via `SteamInternal_FindOrCreateUserInterface`. The host
     creates a lobby tagged with the session password; joiners find it by password (worldwide, no IPs),
     friends can also join from the Steam overlay, and traffic goes through Valve's relays (no NAT trouble).
     Steam copies only; the others use 1 and 3. Verified in game (2026-10-22): Steam is up in-process
     (SteamUser021, SteamFriends017, SteamMatchMaking009, SteamNetworking006, SteamUtils010) and a host's lobby
     is found worldwide by its password. Transport (net/steam_relay): ENet is unchanged and talks to local UDP
     sockets; a proxy in the game tick moves those datagrams over legacy P2P to SteamIDs (host: one local socket
     per lobby member, P2P accepted for lobby members only; joiner: ENet connects to a local proxy port; ENet
     MTU 1200 to fit unreliable P2P). Joiner path untested until a second Steam account is available.
  3. Direct IP + manual port forward always remains as a fallback.
- **Installer radio option** (0.6.1): a checkbox installs Tomasak's Radio Restoration "split" over TJGM's pack:
  GTA IV's stations get the "vanilla" lists (original Vladivostok FM songs) and the Episodes their own lists
  (`EFLC_SOUNDS.DAT15`, `eflc_GAME.DAT16`, episode `setup2.xml`/`downsplit.dat`, needs FusionFix's `DISABLE_FILE`),
  so the Episodes keep their club music. Only the release's option files are used (`opVANILLA`, `opSPLITbase`,
  `opSPLITVANILLA`, CRC-checked against its `hashes.ini`): TJGM's pack already has the same audio
  (`RADIO_RESTORATION.rpf`, `RR_EP1/2_SFX.rpf`, `SOUNDS.DAT15` identical to the 23-05-2025 release's). The release
  (~1 GB RAR, GPL-3.0) is downloaded from its official GitHub page and read with Windows' tar; replaced files go to
  `_iv-coop_backup`. Test path: `iv-coop-setup --radiosplit <game> <release.rar> <report>`.
- **Network hardening** (net/session, net/upnp, tools/bot --chaos):
  - **UPnP** (net/upnp): when hosting by IP (phone Host with no Steam, the host hotkey), a worker thread asks the
    router to forward the UDP port with Windows' built-in `IUPnPNAT` / `IStaticPortMappingCollection` COM API (no
    third-party dependency; it can block for seconds, so it never runs on the game thread). The outcome reaches the
    phone ("Port 41300 opened on your router" / "Couldn't open the port: ...; friends join via Steam") and the log.
    The mapping is removed on Leave and on any disconnect; a stale mapping from a crash is replaced on the next
    open. Not done: removal when the game process is killed or quits without leaving (COM can't be used safely at
    process teardown), so the mapping then stays on the router until it is replaced or the router reboots.
  - **Handshake / version**: the ENet connect data carries `kProtocolVersion` and Hello carries it again plus the mod
    version text (informational, logged by the host). A mismatch is refused with `Reject::VersionMismatch`; the
    disconnect data is `reason | host_protocol << 8`, so the joiner's phone shows "version mismatch: the host runs
    protocol vN, you run vM; update iv-coop". Every remote disconnect (full, host left, kicked, timeout, version)
    is also shown on the phone as "Disconnected: <reason>". A connection that never sends Hello is dropped after 5 s.
  - **Timeouts / teardown**: ENet peer timeout is 8 s at most. A joiner that drops (clean or timed out) raises `Left`
    on the host: its puppet, vehicles and networked entities are removed, and `EntitySync::evaluate_ownership`
    hands its entities to the nearest remaining player. A joiner that loses the host gets `Disconnected`: puppets,
    entities, story/looks/props state are cleared, the Steam relay and lobby are left, and the joiner's own game
    state is restored (`g_restore_pending`). Packet queues are cleared on every session start/stop so stale packets
    can't leak into the next session. There is no automatic re-join; the player re-joins from the phone.
  - **Size and rate limits**: packets above `kMaxPacket` (16 KB) are refused by ENet (`maximumPacketSize`), dropped on
    receive in both roles, and refused on send (`send_game`); queue overflows (game thread stalled) are counted.
    The host drops packets above `kMaxPacketsPerSec` (1500) per joiner and kicks a joiner that stays above it for 5 s
    in a row. Message decoders must go through the bounds-checked `Reader` (`get()` fails past the end; counts read
    from the wire must be range-checked before sizing a container; `remaining()` is available); audited: all current
    decoders do, Welcome's roster count is now capped at 4.
  - **Traffic stats**: every 10 s the network thread logs, per peer, KB/s and packets/s sent and received, rtt, and the
    five busiest message types each way (`[Net] net 10s: peer #1 'Name' ... top out: ... | top in: ...`), plus the
    running dropped-packet total; `SessionStatus` carries `bytes_sent/received` and `dropped_in/out`.
  - **Chaos test**: `iv-coop-bot --chaos [1-3]` (default 2) injects faults on everything the bot sends (`Session::set_chaos`):
    drops (unreliable only), delay/reordering, duplicates, truncated and bit-flipped game packets, and every 15-45 s
    either a clean leave + re-join or a dead link (ENet servicing stops for 10 s, so both sides time out) + re-join.
    Example: `iv-coop-bot --host 127.0.0.1 --name Chaos --chaos 3 --seconds 120` against a running game host, or
    against `iv-coop-bot --serve`.
- **Menu: the phone** (working, 2026-10-22: game/phone, net/steam). CE removed the phone's Multiplayer item, so
  it is put back. The original design: the phone's **Multiplayer** item opens co-op instead of Rockstar's online modes
  (no public/private settings). Its screen (spcellphonenetwork, screen 1047) lists match types through game-text
  keys; the mod overrides those labels (e.g. `CPGO_MATCHT_0` "Host Co-op", `CPGO_MATCHT_1` "Join Co-op", plus
  "Leave Co-op" while in a session) by hooking the engine's text lookup, and intercepts the selection
  (`SET_CELLPHONE_RANKED` / the next-screen switch) so Rockstar's network flow never starts. Choosing Host or Join
  then opens the phone's own **dial pad**, and the 6 digits typed are the **session password** (the same digits on
  both sides find the Steam lobby). The phone scripts compare dialled numbers with `COMPARE_STRING`, which our
  native hooks already see.
- **Remote players** are real player peds in spare player slots (the engine still has the original MP slot code).
  They're driven by the remote player's replicated pad state plus position/velocity correction, so animation,
  aiming, cover and vehicle entry come from the game itself. Fallback: an AI ped with events blocked.

## Player characters

The original MP character models are still in the CE data: `M_Y_Multiplayer` and `F_Y_Multiplayer`
(`common/data/peds.ide`), with the `move_m@multiplyr` / `move_f@multiplyr` move sets and the
`Male/FemalePlayerSettings.dat` + `clothes.dat` item tables.

- **Preferred:** a pre-join **character creator** built on those models and tables, with a live preview ped in
  our overlay. The original lobby UI was tied to GFWL and the removed network frontend, so it is rebuilt rather
  than revived. The result is saved to `plugins/iv-coop/character.ini` and sent on join.
- **Fallback:** a random ambient ped with random variations.
- Only the host is the protagonist (Niko / Johnny / Luis). Others see the host as the
  protagonist through an extra *player* ped (player slots 1-3), since only player peds can
  wear that model; see research-native-netcode.md section 7b.

## Story co-op: how every system is handled

| System | Handling |
|--------|----------|
| **Joining mid-campaign** | The client receives the host's **world progression state**: island/bridge unlocks, IPL and interior state, the open/closed state of story buildings, time and weather, and the active mission. Its own save is never loaded or touched. |
| **Mission scripts** | Run on the host only. On clients, script launching is suppressed apart from a whitelist of HUD/system scripts. Mission start blips are replicated, but only the host can start missions. |
| **Mission entities** (peds, cars, objects, pickups) | Networked entities: the host is script owner, the nearest player is sim owner (principle 2). Clients can shoot, ram and kill them; damage goes to the sim owner, and death/destroy events go back to the host's script copy. |
| **Pickups** (weapons, health, armour, cash, mission items) | The host's story-script pickups (`CREATE_PICKUP`, `_ROTATE`, `_WITH_AMMO`, `CREATE_MONEY_PICKUP`; message 56, `story/pickups`) are recreated on every joiner at the same place, and a late joiner gets the current set; pickup blips (`ADD_BLIP_FOR_PICKUP`) are mirrored with their colour/display props. **Collection:** the host's script decides. A pickup the script polls with `HAS_PICKUP_BEEN_COLLECTED` is mission-critical: if a joiner collects their copy, the host is told and its script's `HAS_PICKUP_BEEN_COLLECTED` / `DOES_PICKUP_EXIST` then answer "collected" for it (the host's copy is removed and the other joiners' copies too), so a package grabbed by anyone progresses the mission. Anything the host collects, or its script removes, disappears for every joiner. Pickups the script never polls (ambient weapon/health/armour/cash) are per machine: each player gets the effect only from collecting their own copy and nothing else changes (cash goes to the collector's wallet). |
| **Mission-provided vehicles** | When a mission spawns a car *for the protagonist* (the host's player is warped or tasked into it, or the script blips it as "your car"), the mission's car is left exactly as the script expects: the host, plus any story characters the script seats in it. Co-op partners get a **duplicate**: same model and colours, spawned on the **nearest clean road node**, with all partners in it together (first joiner drives, others ride). It never touches the mission script's entity, so no mission logic can break. The duplicate is removed when the mission car is released. |
| **Riding with story characters** | Seat occupancy is synced. Joiners can take any **free** seat in any vehicle, including NPC-driven mission cars. When a script tasks a ped into a specific seat, that seat is **reserved**. A joiner sitting in a seat the script needs is moved to another free seat or gently ejected. |
| **Own vehicles** | Joiners drive whatever they like, anywhere. Driver is sim owner, passengers receive. |
| **Cutscenes** (`.cut`) | Host start/skip/end are replicated. Every client plays the same cutscene locally **wherever they are**, through the cutscene camera. IV casts the local player ped as the protagonist (see M7 findings), so on a joiner that ped is the scripted actor and the joiner's own character is a separate *cutscene double* ped that the player steers (pad stick or WASD, camera-relative) and that is what the others see and what gets streamed. Remote players are never inserted; partners' puppets stay visible where they really are, except the host's puppet on a joiner (it would double the protagonist). Afterwards the joiner's character is put where the double ended up, and a "go to host" option is available. |
| **Scripted cameras / in-engine scenes** | The host's active script camera is streamed (`story/scene_cam`); everyone sees the host's shot and partners **keep control** of their character throughout (only the host's scripts freeze the host). Setting "Scenes": Host cam / Own cam. |
| **Dialogue, phone calls, subtitles** | **Positional for joiners:** they hear the host's dialogue from where it is said in the world, not as flat 2D audio. The host's conversations (`NEW_SCRIPTED_CONVERSATION` / `ADD_NEW_CONVERSATION_SPEAKER` / `ADD_LINE_TO_CONVERSATION` / `START_SCRIPT_CONVERSATION`, and their phone variants) are replicated as the speaker table plus the lines. Each joiner rebuilds the conversation with every speaker mapped to their **local copy** of that character: the protagonist becomes the host's player puppet, and story characters become their networked entity copies, with the original voice names. The game then plays each line in 3D from that ped, with normal distance falloff. **Phone calls:** only the host's phone rings. For joiners, the caller's lines play from the host's puppet in the caller's voice through a phone-style filter (as if heard from Niko's handset), and Niko's own lines play from Niko. Subtitles follow the lines; beyond earshot they're optional (setting, default on during missions). Speakers with no local copy fall back to the host puppet. Mission scripts rarely call the conversation natives themselves: they post the line they want through globals and the shared **`speechcontrol`** script plays it (`NEW_SCRIPTED_CONVERSATION`, `ADD_NEW_CONVERSATION_SPEAKER`, `ADD_LINE_TO_CONVERSATION`, `START_SCRIPT_CONVERSATION`, `NEW_MOBILE_PHONE_CALL`, `ADD_LINE_TO_MOBILE_PHONE_CALL`), so capturing speechcontrol on the host covers almost all mission dialogue. |
| **Objectives, HUD text, blips, GPS route** | Mirrored from host script state. Mission timers, counters and bars are **drawn by the script every frame** (`DISPLAY_TEXT*`, `DRAW_RECT`, `DRAW_SPRITE`; the engine's on-screen timer is almost unused), so they need their own replication: the drawing calls, or the values behind them. `docs/missions.md` flags the missions that do this. |
| **Mission teleports / fades / sequences** | **Regroup** (`sync/regroup`, 2026-10-27): when the host's story moves on (its character is moved by a script, the screen fades back in, a mission starts), partners further than 25 m are brought over 1.5 s later: into a free seat of the host's car if the host drives, else their own car (or a new one of the host's model) on the road behind; on foot, next to the host on safe ground (collision loaded, `GET_SAFE_POSITION_FOR_CHAR`). General, no per-mission rules; the phone's "Go to host" stays for anything it misses. |
| **Fail conditions** | Decided by the host script. Client actions can cause failures, e.g. killing a target who must live, or alerting a tail. |
| **Deaths: downed and revive (missions)** | During a mission, a player whose health runs out is **downed** instead of dying: they drop to a wounded state on the ground (crawling, maybe able to shoot a pistol), with a bleed-out timer and a marker for the others. A partner revives them by holding a key next to them: the reviver kneels and plays the **paramedics' CPR animation** (the clips the game's own medics use on injured peds, `Medic_CPR_IN` / `Medic_CPR_LOOP` / `Medic_CPR_OUT`; the dictionary name is found at runtime by probing candidates such as `medic`), and the revive takes **about 6 seconds**. Letting go cancels it. The revived player comes back with **half health**. **The mission fails, and everyone dies for real, only when every player is downed at once** (or a downed player's bleed-out timer runs out with nobody able to reach them, as a setting). The host is kept alive while downed, so the mission script never sees the protagonist die early: their death is only let through, triggering the normal "Wasted" and mission fail, once the whole team is down. Then all players die together. Outside missions, deaths are normal and per player (hospital respawn). |
| **Arrests** | Host arrest behaves as in the normal game. A client's arrest only sends that client to the nearest police station; it never fails the mission. |
| **Wanted level** | Mission-imposed wanted levels apply to every player. Outside missions, wanted level is per player, and police chase whoever earned it. |
| **Saves / autosaves / safehouses** | Host only. Client save prompts are suppressed. |
| **Money** | Every joiner has their **own wallet**. Mission payouts are mirrored: whenever a host story script pays the protagonist (`ADD_SCORE` on the host's player), each joiner gets the same amount. Everything else is per player: cash picked up (dropped by peds, robberies, pickups) goes to whoever collects it, and spending (guns, clothes, food, taxis, car washes, hospital and arrest fees) comes out of that player's own wallet. A joiner's wallet is stored per host campaign (`plugins/iv-coop/wallets.ini`, keyed by host and save) and restored when they rejoin that campaign. Their single-player money is put back when they leave. |
| **Friends, dating, activities, minigames, TV, internet, taxis** | Host-driven. Joiners can come along as passengers or spectators but can't start them. |
| **Ambient world** | Each client has its own traffic and peds around itself. Where players overlap, ambient entities near more than one player become networked (sim owner = nearest). |
| **EFLC (TLAD / TBoGT)** | Same system. The host's episode is the session's; a joiner in another episode is told which one to start and leaves (message 67). |

### Radio: listen together (beyond the original MP)

The original multiplayer never synced radio. We will, so players hear the same station and song at the same
moment:

- **Downgraded radio is expected.** Players install a community radio downgrade, which restores the tracks
  removed from the CE in 2018; it is installed separately, and we don't ship audio. Track lists must be identical
  across the session, so the join handshake compares a hash of each station's track list. A mismatch gets a
  clear "radio versions differ" warning, and radio sync turns off rather than playing different songs.
- **Station sync.** In a shared vehicle, the driver's station applies to all passengers. Each player in their own
  vehicle keeps their own station. Natives exist for both directions: get the current station index, retune to
  a station index.
- **Song/position sync.** IV's stations run on a continuous virtual timeline. What plays on a station is derived
  from a radio clock, not from when you tuned in. If every client's radio clock and station state match the
  host's, the same station plays the same song at the same offset everywhere. Plan: reverse-engineer the radio
  timeline (`audRadioStation`), replicate the host's radio clock/seed on join and correct drift periodically.
  Talk-show/DJ/ad selection must come from the same seed too.
- **Independence.** A client's own station choice never retunes anyone else, except as driver of a shared vehicle.

Milestone: after M5 (it needs vehicle seats), before the mission audit.

### Concessions (resolved per mission during the audit)

When a section can't include joiners, the options in order of preference are:

1. **Let them participate anyway.** Most missions need nothing more than replicated entities and blips.
2. **Mirror a vehicle or seat.** Joiners follow in their own vehicle when the story vehicle is full.
3. **Spectate.** A camera is attached to the host for the section.
4. **Isolate.** Joiners are held outside an interior or area the script locks to the protagonist.

Every mission gets an entry in `docs/missions.md` saying which option applies and why.

## What gets synced

| Tier | Data | Rate / channel |
|------|------|----------------|
| Player | pos, heading, velocity, pad state, aim target, weapon, health/armor, vehicle+seat | 20–30 Hz unreliable, snapshot interpolation (~100 ms) |
| Vehicle | model, pos, rotation quat, lin/ang velocity, inputs, health, colors, door/damage deltas | 20 Hz from sim owner |
| Networked entity | create/delete, model, sim-owner handoff, transform, health, task summary | 10–20 Hz from sim owner; lifecycle reliable |
| Combat | hit events → sim owner applies; deaths/destroys → host script copy | reliable |
| Story/HUD | blips, objectives, subtitles, dialogue lines, cutscene/camera control, mission wanted | reliable, on change |
| World | clock, weather, IPL/interior/unlock state | on join + on change, reliable |
| Session | join/leave, names, character data, chat | reliable |

## Engine access

- **Hooks:** MinHook detours, plus IAT patches where the exe imports the target (e.g. `Direct3DCreate9`).
- **Addresses:** byte-pattern scanning only. One `game::` layer owns all patterns and validates them at startup;
  a missing pattern turns that feature off with a log line. No crashes.
- **Natives:** our own native invoker, which finds the script native table by pattern.
- **Script layer hooks:** we hook script thread creation (to suppress scripts on clients) and the natives that
  create/delete entities, start cutscenes, print text, add blips and play dialogue (to replicate them on the
  host). This interception layer is what makes "replicate what the story does" generic rather than
  per-mission.
- **FusionFix coexistence:** check FF's source before hooking anything it already patches.

## Source layout

```
src/
  dllmain.cpp     startup (loader-lock-safe only)
  core/           log, config, version check, IAT/pattern utils
  render/         D3D9 hooks + ImGui overlay/menus
  game/           patterns, native invoker, script-thread & native interception, entity wrappers
  net/            Transport (ENet), protocol, serialization
  sync/           players, vehicles, networked entities + ownership, world state
  story/          host-side capture & client-side playback of mission/HUD/cutscene/dialogue events
  ui/             host/join menu, player list, character creator
docs/
  ARCHITECTURE.md
  missions.md     per-mission audit & concessions (created at M8)
```

## Milestones

- **M0:** the plugin loads, logs, checks the exe version, hooks D3D9 and draws an overlay. ✅
- **M1:** native invoker, script-thread hooks (list running scripts, suppress on demand), and a loopback
  "ghost" of your own ped through the real serializer. This tests sync with no network. ✅ The tick runs from
  `scrThread::UpdateAll` in a live script's context; the interpolator learns the remote clock from arrivals.
- **M2:** ENet host/join with 4 players, and on-foot player sync. ✅ for the host side: verified in-game with
  three `iv-coop-bot` clients (join/leave, relay, interpolation, "session is full", dead-peer timeout).
  Remote players are still frozen puppets that slide (no walk animation). The joiner's in-game side needs a
  second PC to verify: the launcher stub allows only one copy of the game per machine.
- **M2b:** animated remote players: drive puppets with movement tasks (walk/run/sprint by speed) as an
  interim, then real player-slot peds with synthetic pad input (aiming, cover, vehicle entry for free).
- **M3:** vehicles, including passengers in any vehicle and seat reservation. ✅ base, verified in-game with bots:
  the owner (driver, else lowest-id player inside) sends `VehicleState`, and others drive a stand-in kinematically
  (0.0 m error, 0 snaps). Remote players sit in the right car and seat, including as your passenger.
  **Physics-follow ✅ (default):** the stand-in stays a physical car, steered toward the owner's track with
  mass-scaled impulses (`APPLY_FORCE_TO_CAR` type 1, layout read from its implementation). The controller
  learns its gain (measured 0.3–0.8: tyre grip absorbs the rest) and is limited to 2 g, so walls and traffic win
  contacts. When it's blocked (>3 m off and not closing for 0.75 s) it stops pushing until the 10 m snap. It
  steers horizontally only, eases orientation 15%/frame, and turns the front wheels from the owner's yaw rate.
  Verified with bots: grounded, stops at walls then pops, recovers from bumps. Tight turns screech (realistic).
  Tried and dropped: driver `TASK_CAR_TEMP_ACTION` codes (no effect); re-asserting forward speed each frame
  ("wheel sync"), which fights the suspension so the car sinks. Kinematic mode remains behind
  `[Net] PhysicsFollow=0`.
  Follow-ups:
  - enter/exit animations instead of warps.
  - deformation/damage sync.
  - seat reservation for script-tasked seats (with M6).
- **M4:** networked entities with **script owner vs sim owner** and migration. ✅ Verified in-game with a bot:
  - A host-spawned ped/car migrates to the nearer player once the host walks away, and the bot simulates it over
    the network.
  - When the owner leaves, ownership returns to the host in the same frame, and the ped resumes AI locally.
  - The handover margin (25 m) means a player standing next to an entity keeps it. Ownership returns by distance
    only once they move away.
  - Generic game-message lane (types ≥ 16) carries it.
  Follow-ups:
  - followers are kinematic (peds slide; reuse animated puppets);
  - the live copy's AI is a placeholder (M6 replays the host script's tasks on the sim owner);
  - no streaming radius yet (every client spawns every entity).
- **M5:** combat, damage, deaths/respawns, wanted level rules. ✅ Verified in-game with bots:
  - The victim's owner applies damage: the attacker reports hits it detects on its copy, using a `Damage` game
    message.
  - Copies never die locally: puppets and follower peds sit on a refilled 2000 HP buffer (cars 1000).
  - One hit per shot; damage from a puppet's local bullets is reverted.
  - Melee registers.
  - Puppets mirror weapon, exact shot count (cumulative counter from ammo) and aim point. They aim while walking
    (`TASK_GO_TO_COORD_WHILE_AIMING`, layout from its handler) and stop shooting while their local copy is
    ragdolled.
  - Death and respawn follow the owner, and entities die on the owner's signal.
  Follow-ups:
  - armour isn't drained by remote hits (no set-armour native found yet);
  - melee/punch damage reads low against the 2000 HP buffer (2–3 per punch), so check whether damage scales with
    max health;
  - wanted level: free roam is per player already (each game runs its own police), and shared mission wanted
    comes with M6;
  - remote players' vehicles can't be damaged yet (stand-ins are damage-proof).
- **M5b:** ambient world sync: **city pedestrians and traffic** near more than one player become networked
  entities (sim owner = nearest player), so everyone sees the same people and cars, including the same ped
  getting hit, fleeing or fighting. Ambient entities near only one player stay local (cheap). Builds on M4's
  ownership/migration.
  - First slice built (`sync/ambient_sync`, 2026-10-27; compiled, not yet tested in game; session setting
    "Ambient sync", default Off):
    - Host: every 0.7 s, per partner within 130 m, samples the overlap zone with
      `GET_RANDOM_CHAR_IN_AREA_OFFSET_NO_SAVE` / `GET_RANDOM_CAR_IN_SPHERE_NO_SAVE` and registers ordinary city
      peds and driven cars within 60 m of the host and a partner (not missions, players, in-car peds, parked
      cars, or already-networked ones). Caps 16 peds (drivers included) and 12 cars, 3 new per scan. A car's
      driver is registered with it and always follows the car's sim owner (`evaluate_ownership`). Entities
      are released (`EntitySync::host_release`: clients delete copies, the host copy wakes up, then
      `MARK_*_AS_NO_LONGER_NEEDED`) when fewer than two players are within 90 m or the entity vanished.
    - Ambient ids carry bit 23 (`EntitySync::is_ambient`), so every machine knows which entities are ambient
      without a protocol change: they stream at `ambient_send_hz` (10) with a 50 ms deeper jitter buffer.
    - Joiner: while the host is within 100 m (off at 130 m, no wanted level) density multipliers drop to 0.3
      and off-screen unowned peds/cars in the overlap zone are deleted (3 per scan; never mission entities,
      networked copies or player-occupied cars). Density is restored when apart, off, or the session ends.
    - A migrated ambient driver resumes with `TASK_CAR_DRIVE_WANDER`, a ped on foot with `TASK_WANDER_STANDARD`.
    - Left: verify in game (the area native's extents, density effect); copies lack the ambient ped's
      outfit variation; ambient AI on followers is a placeholder (no flee/fight task forwarding); parked cars
      and passengers are not shared; thinning only deletes off-screen duplicates so a few doubles remain in
      sight; no streaming radius per client (copies spawn on every machine).
- **M6:** story layer: client script suppression, progression-state sync on join, and replication of blips, HUD,
  subtitles and dialogue.
  - Done (verified on Roman 2 via record/replay, 2026-10-20): capture of story calls, mission entity
    replication, blips with routes, PRINT/help text including the mission text blocks (REQUEST_ADDITIONAL_TEXT);
    client mode (joiner model, frozen story scripts, own story blips hidden and restored); the host shown as
    the protagonist through extra player peds (game/player_slots); followed mission cars use physics-follow.
  - Done since (2026-10-22, built, joiner side untested without a second player): late-join snapshot of text
    blocks/blips; world-state ledger (progression) and mirrored payouts; positional dialogue and phone calls
    (story/dialogue); cutscenes for joiners (story/cutscene, M7 basics); duplicate mission car for partners;
    downed/revive (sync/downed); Go to Host (phone); script-drawn mission HUD (story/hud); clock and weather
    (story/environment, 2026-10-26); radio sync - every station's item and position follow the host, the
    passenger follows the driver's station (sync/radio_sync, game/radio; docs/research-radio.md sections 8-10);
    joiner wallets per host campaign (sync/wallet; first visit $1000); lethal hits on missions held at the downed
    level by hooking the player's health setters (sync/downed).
  - Left: task forwarding for mission entities, objects, blips on non-networked entities, the free-cutscene
    option, fire not visible on player-ped partners (START_CHAR_FIRE on an extra player
    ped: the ragdoll mirrors but no flames show; cosmetic), molotov/fire damage between players, matching the host's outfit in cutscenes.
- **M7:** cutscenes and scripted cameras, mission teleport/"join host".
- **M8:** mission audit: play IV start to finish with 4 players and fill in `docs/missions.md`, applying
  concessions as needed.
  - Static groundwork: `tools/mission_analyser.py` reads the decompiled IV/TLAD/TBoGT scripts
    ([GTAIVMissionScripts](https://github.com/0x52617665/GTAIVMissionScripts), public domain) and generates
    `docs/missions.md` (per mission: entities and models, cutscenes, text blocks, cameras, car recordings,
    stealth checks, script-drawn HUD, interiors, world changes, wanted, pass/fail, co-op notes). The runtime
    audit (Ctrl+Shift+A, `story/mission_audit`) spot-checks behaviour in game.
  - **Sync coverage** (2026-10-27): `tools/coverage_natives.py` keeps one list of natives co-op doesn't sync yet,
    by category (NPC anims, attachments, explosions/fire, particles, sounds, cameras/fades, player control,
    vehicle state, ped looks, object state, pickups, drunk effects, minigames, area clears). The mod counts every
    call to them per script while you play (`story/coverage`, `plugins/iv-coop/coverage/coverage.md`, with
    sample anim/sound/effect names), and the tool ranks the decompiled missions in `docs/coverage.md`.
  - **Playlist:** `docs/coverage.md` lists the ~26 missions that between them use nearly every world-changing
    native any IV mission uses (greedy cover, each adds 5+ new ones), then one of each friend activity. They're
    played from real saves: `tools/mission_saves.py` has a GTASnP save just before each (picked by the save's 100%
    checklist), installed in batches of 10 into free slots. Starting a mission script directly (tried 2026-10-27)
    skips the story flow: no on-mission state (non-scripted calls still arrive) and the map isn't unlocked.
- **Friend activities** (added 2026-10-27): drinking, eating, shows, strip clubs, darts, pool and bowling with
  Roman, Jacob, Brucie, Dwayne and Packie (`<friend><activity>` scripts), and dates. The host's friend comes
  along as a networked entity; partners join the outing as extra company. Being **drunk** is synced to everyone
  who drank (`SET_PED_IS_DRUNK`, `SET_DRUNK_CAM`: each player's own wobble and camera), and the drive home works
  with drunk partners as passengers.
  - **Drunk sync (phase 1, `sync/drunk_sync`, message 51):** hooks `SET_PED_IS_DRUNK` (on the host's player ped
    only) and `SET_DRUNK_CAM` (any script; the drink scripts and `go_on_date` set it with `GET_ROOT_CAM`, 0.2 to
    0.7 over 180-300 s, and 0 to end). The host sends `{seq, drunk, mask, cam intensity, cam ms}` reliably on each
    change and every 4 s while drunk. *Present for the outing* = within 30 m of the host's player (3D) when it
    becomes drunk; the host checks its partners' copies and sends a bitmask of player ids. A partner in the mask
    gets `SET_PED_IS_DRUNK` on their own player ped and the same `SET_DRUNK_CAM` on their root cam, both undone
    when the host's end (or when they leave the session). Everyone's copy of the host also gets
    `SET_PED_IS_DRUNK` (re-applied every 2 s in case the copy was recreated). Not synced yet: `SET_PED_IS_DRUNK`
    on other peds (the date in `go_on_date`), partners' own copies on other machines, and `SHAKE_PAD` /
    `SET_TIME_SCALE` (the drink scripts don't call them; they're local feel only).
  - **Findings (activity scripts):** the friend/date peds are made by `CREATE_CHAR` inside the activity
    scripts (`<friend><activity>` use a wrapper `sub(model, &ped, x, y, z, heading)`; `date_food` has
    `CREATE_GIRLFRIEND_ON_FOOT`, a script function over `CREATE_CHAR`), none of the names match the default
    `LocalScripts` (`amb*, vendor*, foodserver*, binco*, *help, carfood*`), so the existing story_sync hooks
    already register them as networked entities for joiners. `go_on_date` and `date_activity` create no
    peds themselves (the date comes from the `dating_*` scripts / `date_food`). The friend's car and the
    pickup walk are not created by these scripts (no `CREATE_CAR`), so they come from the engine or a ped
    that already exists; if a joiner doesn't see the friend arrive, look there first (in-game test).
  - **Tenpin bowling, how it works:** `bowl_trigger` is the lane-side ambient script: it creates the lane's
    objects (ball, 10 pins, tracks; `CREATE_OBJECT`, collision toggled with `SET_OBJECT_COLLISION`, pins
    held with `FREEZE_OBJECT_POSITION`), the bowlers on neighbouring lanes (`CREATE_CHAR`), and starts
    `TENPINBOWL` with `START_NEW_SCRIPT_WITH_ARGS`. `tenpinbowl` (9160 lines) is the game itself: the player's
    and friend's run-up, throw and celebration anims (`TASK_PLAY_ANIM*`, `ATTACH_OBJECT_TO_PED` for the ball),
    cameras, the throw as `APPLY_FORCE_TO_OBJECT` on the ball, the pins as dynamic objects
    (`SET_OBJECT_DYNAMIC`), scoring, and `INCREMENT_INT_STAT_NO_MESSAGE`. `bowling_lane` runs the NPC bowler
    on an occupied lane. `<friend>tenpin` only does the outing around it (drive there, walk in, hand over).
    The pins' physics is run by the engine on whoever owns the objects, scoring reads pin positions locally.
    Multiplayer bowling would need: (1) a turn owner: only the player whose turn it is runs the throw anim and
    ball force on their machine (the host's script stays authoritative for the score, so it needs the
    thrower's input: release angle, power and spin sent to the host, which applies the force to the
    host's ball); (2) lane objects (ball and pins) as networked objects with positions streamed from the host,
    with pins' `FREEZE_OBJECT_POSITION` mirrored; (3) partners' puppets teleported to the lane and given the
    run-up anim by message (not through the frozen script); (4) the scorecard HUD (from `tenpinbowl`'s
    own drawing) with a player-per-frame list; (5) the camera for the thrower, a spectator camera for the rest.
    Darts and pool follow the same split.
- **Multiplayer bowling** (added 2026-10-27): the tenpin minigame (`tenpinbowl`, `bowling_lane`) becomes a
  game for every player present, not just the host and their friend: turns rotate between players, each
  bowls on their own machine with the lane's pins synced, and the scorecard shows everyone. Darts and pool
  (`darts`, `pool_game`) can follow the same pattern.
  - **First slice (`sync/bowling_sync`, message 59, 2026-10-27, untested in game):** option (a). The host's
    `tenpinbowl` stays authoritative for the lane (ball, pins, physics, cameras, anims); a partner's turn is
    played by *answering the script's input reads* with the partner's. The script's throw is a stick gesture
    (`sub_50770`/`sub_51642`: pull back then push forward, read with `GET_POSITION_OF_ANALOGUE_STICKS` or
    `GET_MOUSE_INPUT`, steering while the ball rolls in `sub_58290`), with buttons `IS_CONTROL_PRESSED/JUST_PRESSED`
    (2,4) (2/0,77) (2,87) (2/0,78) (2,3) and `IS_USING_CONTROLLER` choosing the branch. The partner samples those
    natives on their machine (our own tick, so our hooks don't see them) and sends sticks, mouse, a pressed mask and
    latched just-pressed bits (reliable when an edge is in it) at ~30 Hz; the host's `native_hooks::on` hooks on
    those natives overwrite the results (and out pointers) when the caller is `tenpinbowl` and it is a partner's
    turn (stale input >0.5 s is neutral; the host's own input is ignored for the turn, control 23 = quit stays the
    host's). The host's character does the run-up and throw, the partner aims and releases it.
  - **Turns and score:** the host plus partners within 20 m of the host's player at the first throw (none: vanilla
    for the rest of the script). Rotation per frame (strike or second ball; the 10th frame has fill balls), absent
    players are skipped. The script's single card can't hold them, so the mod scores itself: it hooks
    `CREATE_OBJECT_NO_OFFSET` in `tenpinbowl` for the pin (-190707996) and ball (-1862626618) models,
    `SET_OBJECT_INITIAL_VELOCITY` on the ball as the release, then tracks the rack with the script's own "down" test
    (`sub_65157`: moved >0.2 m or `IS_OBJECT_UPRIGHT` 15 deg) until the pins are quiet (>=2.5 s, 1.2 s still, max
    14 s) and applies standard scoring per player. The host sends the whole view (names, frames, current player)
    reliably on change and every 2 s; every machine draws it as an overlay (`DRAW_RECT` +
    `DISPLAY_TEXT_WITH_LITERAL_STRING`, left of screen), partner gets a "Your turn" line.
  - **Second slice (2026-10-28, `sync/bowling_sync` + `story/scene_cam`, built, statics layout and timing untested in game):**
    - **Full 10 frames each.** The script's card is read and written directly (tenpinbowl statics: `find_thread` + 0x58).
      Layout (`l_U617` array size word at static 617 = 6; element i at 618 + 113 i): word 0 ped, word 1 size 21 of
      `_fU4`, roll j = size word 3 at 2+4j then {pins, frame, kind} at 3+4j.., `_fU344` size 10 at 86, `_fU388` frame at
      97, `_fU396` at 99, `_fU400` size 10 at 100, `_fU444` finished at 111, `_fU448` at 112. Every size word, range
      (frame 0..9, flags 0/1, pins 0..10, kinds 0..4, no gaps in the rolls), `l_U617` = 6, `l_U263` 1..6, `l_U264` <
      `l_U263`, `l_U2146` 0..80 and that the statics block is readable (VirtualQuery) is checked at the first throw and
      every tick; the script must also be at its very start (no rolls). Any failure logs why and keeps the first slice
      (pin counting, frames shared); a failure mid-game (`abandon_script_mode`) falls back the same way.
    - **Method.** Card 0 is a window onto whichever human is bowling. When the script records a roll (roll count of
      card 0 or 1 grows) the mod rebuilds that bowler's frames from the script's rolls (the script's own result, no
      pin counting), and the frame is over when `_fU388` moved or `_fU444` is set (the mod's own frame rules are
      cross-checked, a disagreement is logged). At a frame end it saves card 0's words (1..112, never the ped) for that
      human and writes the next human's saved card (or a fresh one, as `sub_11987` would) over them in the same tick the
      roll was seen, which is at least a few script ticks before the script's own `sub_13200` (turn select + game-over
      test, in state 1 or in state 35 after an AI roll) runs. The last bowler's real `_fU444` ends the game through the
      script's normal path (`l_U1353`). Rotation is one frame per bowler, in the order host, partners, friend; finished
      and absent players are skipped. A partner who leaves mid-frame: the host's own input finishes that frame for them.
    - **AI friend.** `l_U263 == 2` (one friend) adds a card named after the friend's model (Roman, Jacob, Brucie, ...;
      guessed `IG_*` model names, else "Friend") as the last bowler. Their rolls are read from script card 1 and never
      credited to a human; the script still plays the friend's throw. The mod steers `sub_13200` (which picks the turn
      from the cards' frame counters) by faking two fields: card 1 `_fU388` reads 9 while a human is up (restored for the
      friend), card 0 `_fU444` reads 1 while the friend is up (the human's real card is in the snapshot). More than one
      friend (`l_U263` > 2) is not supported: frames shared as before. With a friend but nobody near the host the game
      stays vanilla.
    - **Lane camera.** `tenpinbowl` is a story script (not in `LocalScripts`) and uses `SET_CAM_ACTIVE` /
      `ACTIVATE_SCRIPTED_CAMS` (lines ~409, ~5573), so `scene_cam` already captured it. Now its cameras are tagged: they
      are streamed only while `bowling_sync::lane_cam_wanted()` (a game with a partner) and as op 3, which a joiner only
      applies while `bowling_sync::participating()` (their id is on the scorecard); far-away joiners and the solo host are
      untouched. The camera starts with the first throw (the game is only known then).
    - **Partner freeze and controls hint.** On their own turn a partner's `SET_PLAYER_CONTROL` is switched off (back on
      when the turn, the game or the session ends; the script reads the sticks with the host's control off too). They
      see a "YOUR TURN" block: aim left/right, hold the throw control, pull back then push forward (pad and mouse
      wording by `IS_USING_CONTROLLER`); the game's own `TEN_PIN_*` help also arrives as story text.
  - **Needs an in-game check:** (1) the log line "driving the script's scorecard" (else the warning names the failed
    layout check; if the roll entries have no per-entry size word the layout test fails safely); (2) two or three
    players bowl a full 10 frames each, the script's scoreboard shows the current bowler's card and the game ends only
    after the last bowler's 10th; (3) the swap lands before the script's next turn select (a late swap shows as the same
    player bowling twice, "frame end disagree" warnings or odd turn state numbers in the log); (4) the friend's turns
    land in the friend's card and the order host, partner, friend holds (the faked `_fU388`/`_fU444` steering); (5)
    partners see the lane camera, nobody else does, and control returns after the game; (6) the input mapping (control
    ids, `GET_PAD_PITCH_ROLL` is not answered), and that disabling a partner's control doesn't mute their input reads;
    (7) the lane's pins/ball reaching partners as networked objects is still unchecked (they watch from the host's
    camera). "Play again" after a game is vanilla (the 10 s over timer locks the mod out of the replay).
- **M9:** MP character creator (built 2026-10-27, `sync/character`; untested in game). It doesn't depend on the other
  milestones.
  - **Data:** a character = model (`M_Y_MULTIPLAYER` / `F_Y_MULTIPLAYER`) + drawable and texture for components 0..8 +
    two props (hat, glasses; index and texture, -1 = none). Saved per user in `plugins/iv-coop/character.ini`
    `[Character]` (`Configured`, `Female`, `Drawable0..8`, `Texture0..8`, `Hat`/`Glasses` as index+1 with 0 = none,
    `HatTexture`, `GlassesTexture`); without a saved character the joiner plays as `joiner_model` as before.
  - **UI:** phone > Multiplayer > **Character** (shown when idle or joined, not while hosting) opens a list in the same
    phone menu: Model (Male/Female), then per component `Name: n/count` and `Name tex: n/count` (only where the model
    has more than one), Hat / Glasses (+ their texture), Save, Back. The phone only reports "select" on a row, so
    select steps that value to the next and wraps (no left/right). Labels are made at run time (`phone.cpp`
    `dynamic_key`, one never-replaced game-text key per distinct text). The preview is on the player's own ped: idle,
    the player is swapped to the model (CHANGE_PLAYER_MODEL) and put back when the editor closes (Back, Save, or the
    phone closing; the original model and its 11 components are restored); joined, the ped already wears it. Save
    writes the ini; Back discards.
  - **Applying:** the joiner's own ped uses the saved character: `remote_players` `swap_player_model` (client mode
    entry and the end of a cutscene) calls `character::apply_own` after CHANGE_PLAYER_MODEL, which sets each component
    (checked against `GET_NUMBER_OF_CHAR_*_VARIATIONS`) and the props. `character::joiner_model` replaces
    `joiner_model` when a character is saved.
  - **Sync:** message 61 (reliable): `[61][sender][model hash u32][drawable x9][texture x9][prop index x2 (i8)][prop
    texture x2]`. A joiner whose ped wears its character sends it when it joins, when it saves, whenever the set of
    players changes (so late joiners learn everyone's within a frame) and every 5 s. Anyone relays through the host
    (`send_game`). Receivers keep the latest per player and apply it to that player's puppet (player slot ped) when it
    spawns or the character changes (`character::on_puppet`, only once the copy wears the right model). The per-frame
    outfit sync (`apply_outfit`, components only) still runs; the message adds props and the exact selection.
  - **Limits / to verify in game:** there is no native for prop counts, so cycling stops when `GET_CHAR_PROP_INDEX`
    doesn't read back what was set (hard cap 24 indexes, 4 textures); the argument order of
    `SET_CHAR_PROP_INDEX_TEXTURE` (ped, type, index, texture) is a guess; the phone list window is scrolled by
    setting the visible-window slots (`rebuild_display` `select_item`) and its capacity is unknown (rows are cut to
    fit); restoring single-player Niko after an idle preview restores his components, not other state.
- **M10: TLAD and TBoGT** (first slice 2026-10-28). The Complete Edition runs all three from one GTAIV.exe.
  - `game/episode`: `GET_CURRENT_EPISODE` (0 IV, 1 TLAD, 2 TBoGT), re-read every 2 s; names, protagonist; the
    protagonist model is the generic `player` unless the running player is measured wearing another real ped model
    while nothing of ours has changed it.
  - `sync/episode_sync`, message 67 (reliable): `[67][sender][op][episode][protagonist model u32]`. Op 0 host ->
    everyone on each join; a joiner in another episode leaves and the phone says which episode to start. Op 1 joiner
    -> host on connecting; the host only warns. No protocol bump.
  - Phone: the phone global is 91 / 94 / 95, and the episodes' `spcellphonemain` statics are IV's shifted up by 5
    (same struct sizes; checked in the decompiled scripts), so `main_statics()` returns the shifted base. If the main
    menu's list doesn't look right for 90 frames the co-op menu turns off (`phone::unavailable()`) and F10 hosts with
    a random password / F11 joins `[Net] JoinAddress`.
  - The joiner model is checked against the episode's image (`IS_MODEL_IN_CDIMAGE`), falling back to
    `M_Y_MULTIPLAYER`; the cutscene swap uses the episode's protagonist model; blip sprite scan up to 256.
  - Bowling's script card swap works in all three games with a per-episode static-slot table (`Layout` in
    `bowling_sync.cpp`, matched from the decompiled `tenpinbowl`; TBoGT's card array holds 2, not 6; unmapped/unknown
    episode or a failed layout check falls back to shared frames). The mission runner and mission saves are IV-only.
  - **Self-test** (`story/self_test`, `tools/self_test.py`, 2026-10-28): one launch switches IV -> TLAD -> TBoGT the
    way the frontend's Episodes menu does (action 0x3E with `[0x1160C08]` = episode; `[0x11D6FD0]` requested,
    `[0x11D6FD4]` current episode, applied by 0x5C1980), opens the phone (a short "up" for the phone scripts, a
    short accept, pad A / control 77, for the rest, which skips the episodes' scripted intros), checks the menu
    items, then hosts the bot, joins a bot host in the same episode (client model swap and restore) and one in
    another (turned away). The bot's `--episode N` plays message 67. All 27 checks passed on 2026-10-28; one
    earlier run crashed once in the renderer (`CDrawPedDC` allocation failed, TLAD intro, a frame after a model
    swap). Cause (found in 0.9.0): a ped whose model just changed can reach the ped draw-list builder (0xC645A0)
    before its render data (CPed+0x310) is built; the draw command (0x8DAFC0) copies 0x370 bytes from it and read
    null at 0xDF969A. `game/model_swap` hooks the ped add-to-draw-list (0xC64520) and skips such a ped for the frame.
    That only moves the TBoGT join crash on: after the swap from Luis, a stale `rage::crCreatureComponentBlendShapes`
    registration stays in the render-callback list (0x19F2488, filled by 0x6A1A60, run as draw command 0x8DDE30), so
    the render thread calls a freed object (guarded: skipped when its vtable isn't in the image) and the main thread
    then faults at 0x6A1ADB. Not caused by a nearby puppet or by the old model streaming out (both tried). Open.
  - **Episode sync gaps (0.7.0):** `sync/player_mirror` (message 69) repeats the host story's calls on the host's
    player (GIVE_WEAPON_TO_CHAR, REMOVE_WEAPON_FROM_CHAR, REMOVE_ALL_CHAR_WEAPONS, ADD_ARMOUR_TO_CHAR,
    SET_CHAR_BULLETPROOF_VEST, SET_CHAR_HEALTH, SET_PLAYER_DISABLE_JUMP/CROUCH) on every partner's player;
    `sync/race_sync` (message 71) mirrors CREATE/DELETE_CHECKPOINT and the GPS race track (START_GPS_RACE_TRACK,
    ADD_POINT_TO_GPS_RACE_TRACK, RENDER/LOOP_RACE_TRACK) with a full resend to late joiners; `story/hud` also replays
    DISPLAY_TEXT_WITH_2/3_NUMBERS, DRAW_CHECKPOINT(_WITH_ALPHA) and sprites (DRAW_SPRITE / DRAW_TOPLEVEL_SPRITE: the
    host records each texture handle's dictionary and name from LOAD_TXD/GET_TXD/GET_TEXTURE/
    GET_TEXTURE_FROM_STREAMED_TXD; partners load the same texture). `parachute*` and `*stattracker` always run
    locally (the engine starts parachutelauncher for whoever boards a helicopter). Natives were picked by diffing
    the episodes' decompiled scripts against IV's.
  - **Pool (`sync/activities/pool`, on `activity_seats`):** 1v1, each player their own character; the team mode was
    removed. When pool_game runs on the host, the first partner near the table (the head of a wait queue) takes seat 0
    (the friend/stranger seat) and the host keeps seat 1. The adapter writes the partner's puppet into the script's
    args/live copy (not pool_table's block), sets seat 0's human flag and pad word 1, seats it (`activity_seats::seat`,
    original opponent ped kept hidden as the stand-in so the script's DELETE_CHAR hits it), and sets the active seat
    from the turn word every frame. Statics are checked (size words, flags 0/1, turn, winner) per episode; any doubt
    leaves the game single player. The winner stays on across games (a winning partner keeps seat 0; a loser queues
    behind the others, longest wait first); the next challenger sits when pool_game starts again. Notifications are
    host-side only. **To verify in game:** whether START_NEW_SCRIPT_WITH_ARGS copies the args before the first tick
    (the adapter handles both), that the init's `l_U56[0] = 0` write is followed by our re-assert before any read,
    seat 0's pad-1 stick reads, the puppet surviving state 0's CLEAR_AREA_OF_CHARS, and the partner staying within
    10 m of the table. Table cameras (tenpinbowl, pool_game, darts) only stream to the seated players.
  - **Darts (activities/darts, message 77):** up to four humans, each their own 301 (double out). The script has
    two hard-coded seats (0 the local player, 1 the AI), so all humans play through seat 0: at each seat flip (3 darts
    or a bust; seen as seat 0 -> 1 in state 1) the window's {score, history row, turn-start score} is saved for the
    thrower, the next human's are loaded, and seat/dart/points/hint/sub-state are reset so the AI never throws.
    A partner's turn is `activity_seats::set_active(partner)` (input + GET_PLAYER_CHAR = their puppet at the oche);
    the host's turn is `set_active(0xFF)`. Win = state 2 with seat 0 at 0 (announced via the phone box and the log).
    The script's own two-row board is replaced by the mod's 2-4 row board (drawn on the host, sent to partners as
    message 77). Statics are layout-checked (array count words, ranges); on doubt the game stays vanilla 1v1.
    **To verify in game:** the rotation on 3 darts, on a bust and on a checkout; the extra dart the script counts
    after a bust (undone until the first throw); a rematch (scores reset, host first); a partner leaving mid-game;
    that the partner's character stands at the oche.
  - **Pool and darts (0.7.1, `sync/minigame_sync`, message 73):** bowling's approach without a scorecard of our
    own. pool_game and darts are the same script in all three games; the human side is played by everyone at the
    table (host and partners within 15 m when the script starts) as a team against the friend. The controlling
    player's sticks, mouse, keyboard move (pool aims with it), controls (2: 0 1 2 4 23 77 78, 0: 77 78), pad buttons
    0-19 and mouse buttons answer the script's reads on the host. A go ends after 1.5 s without aim reads (sticks,
    mouse, keyboard move) and the next read hands it on, so pool alternates shots and darts alternates darts.
    Table cameras (tenpinbowl, pool_game, darts) only stream to the players at the table. **To verify in game:** that
    the scripts don't read the aim during the friend's turn or while balls/darts are in flight (otherwise a go
    would never end), and that pool's per-player pad index (`l_U318._fU32[l_U243]`) is 0 for the human side.
  - **Golf (TBoGT, `sync/activities/golf`):** up to 4 humans (host plus partners within 15 m when the script starts),
    each their own golfer with their own 10 shots and score, turn by turn. Every human takes the script's P side:
    `activity_seats::set_active` makes `GET_PLAYER_CHAR` return the shooter, and the script's own arrays are the
    shooter's scorebook (`l_U2884` shots / `l_U2887` score written before their turn, read back after; `l_U2885` is
    kept above 0 until the last human's last shot so the game doesn't end early, `l_U2888` shows the best of the
    others). The AI opponent's half-turn between two shots is bounced through the script's own skip
    (`IS_CONTROL_JUST_PRESSED(2, 77)` after 1.25 s), and the golfer changes during it, so the script's hand-over
    parks the last shooter and puts the next on the tee. The club (`l_U3100`, created once and attached to the first
    P ped) is re-attached to each new golfer; non-shooters are parked beside the waiting spot. The statics are
    checked against the research (size words 2 and 2, `l_U2882` = 10) before anything is written, else vanilla.
    The mod draws a 2-4 row scoreboard on the host and announces the winner with the phone message (partners
    seeing the board comes later). `activity_seats` relays three more controls for the swing ((2,137), (0,1),
    (0,137): the strike reads pad 0). **To verify in game:** the swing release from a partner, that the O skip
    fires once per half-turn and the tee hand-over uses the new golfer, club position after the swap, the host not
    walking off while a partner shoots, that puppets tolerate the tee attach / `MINI_GOLF` anims, rematch and quit.
  - **TLAD arm wrestling and hi-lo cards (`sync/activities/tlad_games`):** the host's own scripts stay the game.
    Arm wrestling has an unused second-human mode: static `l_U116 = 1` makes seat B read `GET_POSITION_OF_ANALOGUE_STICKS(99)`
    through the flick helper. A partner within 15 m when a match starts becomes the opponent (puppet in `l_U220`, `l_U116 = 1`,
    `activity_seats::seat(.., 99)`, the script's own opponent kept hidden as the stand-in so its DELETE_CHAR / MARK_...
    on `l_U220` land on it); the pad-99 stick read flags a context so the helper's mouse / keyboard / IS_USING_CONTROLLER
    fallbacks are answered from the partner too (`activity_seats::sample`). A seat on a pad above 2 is live all the time (the
    partner streams input without a turn). Hi-lo cards seats up to three partners in `l_U253[1..3]` (group friends' seats
    are left alone); the script plays them as AI, so on a partner's turn (`l_U214 == 3`) the adapter holds the AI off
    (reverting its `l_U214 = 4`) and writes the AI's own results (`l_U116`, `l_U221`, `l_U216`, `l_U214 = 4`) from the
    partner's higher / lower (`activity_seats::control`, controls 93/130/79, 94/131/78); after 20 s the AI decides.
    The table's exit (`SET_PLAYER_CONTROL(.., 1)` in the script's cleanup) puts the original peds back first. Stakes
    and winnings stay the host's (announced for partners). **To verify in game:** that seat B's power moves for a flicking
    partner (the script's AI block after the pad-99 branch, arm_wrestling.txt:1848-1869, must not overwrite it; a log
    warning says so after 12 s), that mouse partners' per-frame deltas are enough to flick, that the puppet shows in the
    seat and plays the match animations, hi-lo turn flow and reaction animations after a partner's call, and the
    cleanup (host and puppets back to normal; the ambient AI diners return).
  - **Activity physics (0.8.0):** GTA IV's physics isn't deterministic across machines (frame timing, floating point
    state), so the activities don't try to simulate the same shot everywhere. The host's copy is the only simulation:
    objects the activity scripts create (tenpinbowl, pool_game, darts, golf, air_hockey) are networked as *precise*
    entities (`EntitySync::kPreciseBit`), pinned to the host like every story entity; partners' copies are frozen and
    only posed. The host sends a precise entity at 60 Hz while it moves and its exact rest pose (reliable, twice)
    when it stops, then a 2 Hz keep-alive; copies blend with a cubic Hermite through positions and velocities, with
    half the usual delay and no extrapolation past the newest snapshot. So the path between snapshots follows the
    motion, nothing overshoots, and every ball, pin and dart comes to rest where it did on the host. The rules,
    scores and turns are the host script's, and the host reads its own simulation.
  - **Air hockey (`sync/activities/air_hockey`, TLAD + TBoGT):** 1v1 host vs one partner, each as their own character. The
    script has two seats: seat 0 reads pad 0 (`sub_14382`), seat 1 is `l_U524` driven by an AI (`sub_15445`) that is skipped
    while `l_U486 > 0`. When a game is under way (`l_U245 == 4`) the partner's puppet replaces `l_U524` (original kept hidden
    as the stand-in, same anchor `l_U538[1]`), `l_U486` is held at 1e9, and every frame (states 4/8) paddle 1's velocity
    statics (276/277) get what the seat-0 helper `sub_14423` would write, mirrored: stick / mouse divisors and table-bound
    gates from the script's own statics, right-stick slam, and the `sub_14980` wall pinch. The partner's sticks come from
    `activity_seats::sample` (now carries lx/ly/rx/ry) on a seat on pad 97, no native hooks. TBoGT = statics -113. Layout
    fingerprinted (divisors 1650/560/512/256, extents, AI tuning) else vanilla; scores announced via the phone.
    **To verify in game:** slot numbers (paddle array element size 12, velocity 276/277, scores 518/519), that the
    far-seat sign/mirroring feels right, input latency (velocity commands are one frame + network old; the partner's
    view is the host's entity stream), the slam, the mouse divisor (fixed at the sensitivity midpoint), rematch/quit and
    the stand-in surviving cleanup.
  - **Races (TBoGT `raceSP`):** partners race as themselves in their own car (`sync/activities/races.cpp`, message 79).
    When the script's grid is populated (stage word 874 = 0, sub-stage 875 = 5, racer array l_U1129 with 4 slots, player in
    slot 3) the first three partners near the start take the AI slots 0-2: each builds a car of that slot's model on its
    grid spot (held until the host's "go"), the slot's AI car and driver are hidden in the sky as stand-ins, and once
    the partner's puppet sits in the host's copy of their car the slot's ped and car words are replaced by them. The script's
    own checkpoint, lap and place code then tracks them (`LOCATE_CHAR_ANY_MEANS_3D` on the puppet); everything it does to
    that slot (`TASK_CAR_MISSION*`, `CLEAR_CHAR_TASKS`, the rubber-band `SET_CHAR_COORDINATES`/`APPLY_FORCE_TO_CAR`/
    `WARP_CHAR_INTO_CAR`, `DELETE_*`) is aimed at the stand-in by before-hooks. Racers are not seated (activity_seats would
    stop the puppet following the partner's driving). Places come from lap, checkpoint, distance as in `sub_2125`; each
    partner gets theirs by message and phone text. Car/bike types 0-2 only; any layout mismatch leaves the race vanilla.
    TLAD RoadRash is not handled (6 AI racers, different struct, combat).
  - **Hook safety:** `native_hooks::install` skips any handler inside the protected region; no-op natives such as
    SET_PLAYER_INVISIBLE_TO_AI point at a shared engine stub (0x4016A0) that the engine calls everywhere, and
    hooking it crashed the game at startup.
  - **To verify in game:** Johnny/Luis cutscene swaps with a real partner, and the
    episodes' own HUD elements (gang/club meters) and minigames (TBoGT club, base jumps, fight club, TLAD gang wars).
- **Co-op settings** (phone: Multiplayer > Co-op Settings; done 2026-10-26, sync/session_settings): the host's
  settings are the session's, saved in `iv-coop.ini [Session]` and sent to joiners. Friendly fire: **Off** (no
  lock-on, no damage), **On** (lock-on and damage, with the engine's own melee between players: partners' copies are
  damageable player peds, docs/research-native-netcode.md 7c), **No aim** (no lock-on, damage; the default); downed and
  revive; shared mission pay; radio sync; clock and weather sync; partner blips; invasions (M11, Off by default). Later:
  free cutscenes, max players.
- **M11: invasions** (Dark Souls style, added 2026-10-26). A group (1-4 players) can invade another group's
  session while it is **on a mission** or at **3+ wanted stars** (the invaded group's host decides when the
  window is open; it closes when the mission ends or the wanted level drops below 3). Invaders spawn as **FBI
  agents** (FBI model, FBI weapons) and their goal is to **hunt down the invaded group's protagonist** (the
  host's Niko). Sessions grow to up to 8 players (4 + 4); co-op stays 4 (cars seat 4, so an
  invading group spawns together in one FBI car). Needs:
  - Matchmaking: a Steam lobby flag "open to invasion" set by the host while the window is open, and an
    "Invade" item in the phone's Multiplayer menu that finds such a lobby and joins it as an invading group.
  - Sessions above 4 players: our ENet session (not the native MP session) raises its player cap; extra
    player slots for puppets (player_slots has 3 now), bandwidth/interest management for 8 players.
  - Invader mode: invaders don't take part in the host's story (no story replication of objectives/HUD, no
    payouts), spawn together in one FBI car at a distance from the target, with an FBI loadout, see the target's blip, and do not respawn: they go down and are revived
    like co-op partners (see the rules slice below).
  - Outcomes: protagonist killed -> the invaded group fails (mission failed / wasted) and invaders get a
    reward; all invaders down or dead at once, or the window closes -> they're sent home. Invaders return to their own
    session/world afterwards.
  - Friendly fire rules between invaders and the invaded host's police (police ignore FBI invaders or treat
    them as allies).
- **M7 cutscene findings and design (`story/cutscene`, `sync/cutscene_double`).** The native list has no way to keep the
  local player out of a `.cut` or to substitute another ped: `START_CUTSCENE_NOW` / `INIT_CUTSCENE`+`START_CUTSCENE` take only the
  name, and the player-related natives (`MAKE_PLAYER_SAFE_FOR_CUTSCENE`, `IS_PLAYER_READY_FOR_CUTSCENE`, `SET_CHAR_IN_CUTSCENE`,
  `SET_CAR_IN_CUTSCENE`, `SET_CUTSCENE_EXTRA_ROOM_POS`, `GET_CUTSCENE_PED_POSITION`) only prepare the player or flag *other*
  entities (scripts register mission peds/cars with `SET_*_IN_CUTSCENE`); they cannot re-cast the protagonist. Changing that
  would mean patching the executable's cutscene code, which is off limits. So the player ped is always the actor, and the joiner
  cutscene is: (1) a double of the joiner's character (same model, outfit, health, armour, weapon) is created where they stand, (2) the
  player ped is switched to the protagonist model and the cutscene starts on it, the cutscene camera is the player's view,
  (3) `sync::cutscene_double::tick` steers the double (left stick or WASD relative to the camera yaw, run when the stick is pushed
  or while moving on keys, via `TASK_GO_STRAIGHT_TO_COORD`), (4) `sync::capture_ped`/`capture_local` stream the double instead
  of the actor, so nobody ever sees the joiner's position as the protagonist's, (5) when the cutscene ends the player ped is
  moved to where the double stands and the double is deleted. The double is invincible and, if it is >200 m from the actor
  (scene streaming has moved away so the ground may be unloaded), frozen in place. The host's own machine needs nothing but not
  hiding partners' puppets. Limits: the double is only visible if the cutscene's shot covers where the joiner stands (otherwise
  they walk about off camera, in a world the cutscene may have cleared of peds); no sprint, jump, cover or combat input on the
  double yet; interiors loaded only by the cutscene's own extra room are not available to it. Untested in game.
  - **First slice built (2026-10-27, untested in game; sync/invasion, net/session, net/steam, game/phone):**
    - *Capacity/roles:* protocol 6. `net::Role {Coop, Invader}` is in Hello, the Welcome roster and PlayerJoined. The ENet
      host takes up to 7 clients (ids 1..7): co-op group max 4 (host + 3), invaders max 4, otherwise `Reject::Full`;
      an invader is refused with `Reject::NotOpen` unless the host called `Session::set_invasion_open(true)`.
      `Session::kick_invaders()` disconnects them. `player_slots` kSlots 3 -> 7 (the engine has 32). Steam lobby
      max members 8. The bot has `--invader`.
    - *Window (host, sync/invasion):* new setting "Invasions: On/Off" (default Off, `iv-coop.ini [Session] Invasions`,
      bit 7 of the settings flags; the phone's settings list now shows as many rows as the script array holds).
      Open = setting on AND (GET_MISSION_FLAG or 3+ wanted stars), polled every 0.5 s; closes 5 s after the reason ends
      (at once if the setting is switched off). Opening admits invaders, sets the lobby data `ivc_open=1`
      (`steam::set_lobby_open`) and sends message 65 (Open); closing sends Result(window closed) and kicks the invaders
      1.5 s later. The host gets a help message when invaders arrive. Message 65: `[65][sender][sub][value]`, sub 0 Open,
      sub 1 Result (1 = the target died, 2 = the window closed).
  - **Rules and banners slice (built 2026-10-28, untested in game; sync/invasion, sync/downed, render/banner):**
    - *Invaders go down, not out:* `downed` treats an invader as always "on a mission": a lethal hit is held at the
      floor and they lie injured; they never bleed out and the mission-ended get-up doesn't apply. Only same-role
      players are teammates, so invaders revive invaders (hold E, same CPR clip and 6 s) and the invaded revive the
      invaded; the host's AllDown (co-op group) is ignored by invaders. A real death (not just the floor) still sends
      that invader home.
    - *Failure:* the host checks every tick that all invaders (with a received state) are downed or dead
      (`Player::downed` comes from the kDowned state flag). Then it sends Result 3 (InvadersDown), shows "FBI VANQUISHED",
      and kicks any invader still connected 2.5 s later; the invaders' own client sees Result 3 and takes the
      leave/restore path with the "INVASION FAILED" banner. Sub 2 Arrived (host -> all) is sent when the first invader
      enters.
    - *Banners (render/banner):* invaded group: "FBI INVADING" on arrival, "FBI VANQUISHED" when the invaders all fall,
      "DEFEATED BY THE FBI" when the target dies; invaders: "TARGET ELIMINATED" (win), "INVASION FAILED" (all down),
      "YOU DIED" (a real death). Drawn by the imgui overlay's foreground draw list (also when the F8 debug overlay is
      hidden): full-width translucent dark band with feathered edges, Cinzel capitals with letter spacing that drifts
      wider, gold for victories and red for defeats, fade in 0.3 s, hold 3 s, fade out 1 s. The font is Cinzel (SIL OFL
      1.1, from google/fonts ofl/cinzel; `src/render/assets/Cinzel.ttf` + `OFL.txt`) embedded in the .asi as an RCDATA
      resource (`banner.rc`), so nothing else is installed. Sound: an optional WAV in `plugins/iv-coop/ui/`
      (`banner_invading.wav`, `banner_victory.wav`, `banner_defeat.wav`, PlaySoundW async), else a game sound: victory =
      `TRIGGER_MISSION_COMPLETE_AUDIO(50)` (mission-passed stinger), defeat = `PLAY_SOUND_FRONTEND`
      `R4_BLEED_OUT_BODY_SPLASH` + `POLICE_COMPUTER_SEARCH_FAIL`, invading = `PLAY_AUDIO_EVENT` `FRONTEND_OTHER_RACE_321`
      (names from the decompiled mission scripts; no FromSoftware assets).
    - *Invading:* phone Multiplayer > "Invade" (`steam::find_open_lobby`: any lobby with `ivc_open=1`, worldwide) joins
      through the Steam relay with `Role::Invader`. Dev path: F9 (`[Net] InvadeKey`) joins `JoinAddress` as an invader
      (only while that host's window is open). On connect the invader's position, health and weapons are saved;
      the client-mode swap puts the FBI agent model (`M_M_FBI`, else the usual joiner model) on the player; story
      scripts and own blips are frozen/hidden as for any joiner. The lowest-id invader picks a bearing 200-400 m from the
      host's protagonist, teleports there (LOAD_SCENE), takes the nearest road node (retries other bearings if it lands
      too far/near, e.g. water), creates an `FBI` car (fallback POLICE, WASHINGTON) and drives; the others warp into
      its free passenger seats (they wait 20 s for the lead's car, then spawn their own). Loadout: carbine, shotgun,
      pistol, full health; the police ignore them (SET_POLICE_IGNORE_PLAYER). Goal text via the help box.
    - *Dropped for invaders:* all story messages (text, blips, payouts, cutscenes, HUD, world ledger, clock/weather),
      sounds, pickups, scenes, fx, npc anims, regroup, radio, drunk and bowling sync; no wallet swap (they keep their
      own money), no character-creator outfit, no downed/revive. The world (entities, vehicles, players) still
      replicates, so the invader sees and fights the host's world.
    - *Blips:* invaders see the host (the target) and their own team only; the invaded group sees its partners per the
      Partner blips setting and never the invaders.
    - *Team fire:* `invasion::hostile(a, b)` (roles differ) forces lock-on, engine damage and outgoing/incoming damage
      messages on between invaders and the invaded; co-op vs co-op keeps the setting. Melee sync's session-wide
      switch is on whenever any invader is present (so co-op melee between partners also applies while invaders are in).
    - *End:* invaders leave (disconnect, model back, position/health/weapons restored, their FBI car deleted) when the
      window closes (Open 0, Result 2, or the kick), when the target dies (host's Result 1 or its puppet's dead flag;
      "invasion successful"), or when they die: their health is held at the downed floor (110) by the same
      lethal-damage guard, and reaching it ends the invasion instead of killing them. The invaded group's downed/revive
      works as normal; invaders aren't their teammates. The host's death on a mission is the existing mission fail.
    - *Left:* invader respawn near the car (they only leave on death); invader rewards and a result screen; stealth
      for the invaded (nothing warns them of the approach besides the arrival message); coloured/red target blip
      (the puppet's normal partner blip is used); a spawn point guaranteed on road/safe from walls (no ground probe:
      the node's z is used); AI cops treating invaders as allies is only "ignore" (no cop shooting back is not
      checked); the host-side clock/weather for invaders (they keep their own); interest management/bandwidth tuning
      for 8 players (everything is still broadcast to everyone); Steam search shows only one lobby (the first match)
      with no browser or invite; a host can't pick who may invade; the melee switch above; all of it needs in-game
      testing (two Steam accounts, or the bot with `--invader` for the admission rules).
- **Late: "free cutscenes" option** (host toggle, off by default). During cutscenes, joiners see the cutscene from
  its camera as usual, but their characters stay visible and controllable in the scene instead of being hidden and
  frozen. Needs: keep remote peds streamed and unfrozen during `.cut` playback, keep sending/applying their state, and
  keep the local joiner's input live while the cutscene camera holds the view. The default stays as in the
  cutscene row above.
- **Connectivity:** UPnP port mapping, then Steam invites/relay (see Shape). This can land any time after M2.
- **Final: one-click installer.** It finds the Steam install, checks that `GTAIV.exe` is 1.2.0.59, and installs
  FusionFix, the radio downgrade and iv-coop (plus a default ini). It also offers to uninstall and restore.
  Third-party pieces are fetched from their official release sources at install time rather than bundled, so we
  never redistribute other people's files or Rockstar's audio. Their versions are pinned and verified by hash.
  Concretely (`tools/installer`, `iv-coop-setup.exe`, one file to send):
  1. Find GTA IV: Steam's install path (registry), every library in `libraryfolders.vdf`; folder picker fallback.
  2. Check `GTAIV.exe` is 1.2.0.59 and the game isn't running.
  3. TJGM's Drag n' Drop Archive (FusionFix, radio restoration, fixes, HD textures; the pack the mod ships
     with): skipped when already installed; otherwise downloaded from its official Mega mirror (Mediafire is
     behind a bot check) and extracted into the game folder with Windows' `tar`, after backing up every file it
     would overwrite. Official mirrors only (no rehosting: the pack states no redistribution terms). If Mega
     fails, the installer opens TJGM's Mediafire page in the browser and asks for the downloaded zip; "Install
     from a downloaded zip..." does the same any time (the zip is checked to be TJGM's archive). The pack is
     4,194,040,357 bytes, sha256 5ee1baa3d830dd8af1e95f4b21c0df1355264fbbf787ff6391109da6d8d83ce8.
  4. `iv-coop.asi` is embedded in the installer; the ini writes itself on first launch, with the player name
     defaulting to the Steam name.
  5. Uninstall removes the mod and restores the backups (`GTAIV/_iv-coop_backup`).
