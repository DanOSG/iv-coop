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
- **Listen server, 4 players max.** ENet over UDP (reliable + unreliable channels) behind a `Transport`
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
| **Cutscenes** (`.cut`) | Host start/skip/end are replicated. Every client plays the same cutscene locally **wherever they are**. Remote players are never inserted; their peds are hidden, frozen and invulnerable meanwhile. Afterwards each client goes back to where they were, and a "go to host" option is available. |
| **Scripted cameras / in-engine scenes** | Same as cutscenes: the host's script camera state is streamed, and clients either watch it or keep control. Configurable per scene during the audit. |
| **Dialogue, phone calls, subtitles** | **Positional for joiners:** they hear the host's dialogue from where it is said in the world, not as flat 2D audio. The host's conversations (`NEW_SCRIPTED_CONVERSATION` / `ADD_NEW_CONVERSATION_SPEAKER` / `ADD_LINE_TO_CONVERSATION` / `START_SCRIPT_CONVERSATION`, and their phone variants) are replicated as the speaker table plus the lines. Each joiner rebuilds the conversation with every speaker mapped to their **local copy** of that character: the protagonist becomes the host's player puppet, and story characters become their networked entity copies, with the original voice names. The game then plays each line in 3D from that ped, with normal distance falloff. **Phone calls:** only the host's phone rings. For joiners, the caller's lines play from the host's puppet in the caller's voice through a phone-style filter (as if heard from Niko's handset), and Niko's own lines play from Niko. Subtitles follow the lines; beyond earshot they're optional (setting, default on during missions). Speakers with no local copy fall back to the host puppet. Mission scripts rarely call the conversation natives themselves: they post the line they want through globals and the shared **`speechcontrol`** script plays it (`NEW_SCRIPTED_CONVERSATION`, `ADD_NEW_CONVERSATION_SPEAKER`, `ADD_LINE_TO_CONVERSATION`, `START_SCRIPT_CONVERSATION`, `NEW_MOBILE_PHONE_CALL`, `ADD_LINE_TO_MOBILE_PHONE_CALL`), so capturing speechcontrol on the host covers almost all mission dialogue. |
| **Objectives, HUD text, blips, GPS route** | Mirrored from host script state. Mission timers, counters and bars are **drawn by the script every frame** (`DISPLAY_TEXT*`, `DRAW_RECT`, `DRAW_SPRITE`; the engine's on-screen timer is almost unused), so they need their own replication: the drawing calls, or the values behind them. `docs/missions.md` flags the missions that do this. |
| **Mission teleports / fades** | The host is moved as normal. Clients are **not** pulled along (no range limit) but get a one-key "join host" teleport. |
| **Fail conditions** | Decided by the host script. Client actions can cause failures, e.g. killing a target who must live, or alerting a tail. |
| **Deaths: downed and revive (missions)** | During a mission, a player whose health runs out is **downed** instead of dying: they drop to a wounded state on the ground (crawling, maybe able to shoot a pistol), with a bleed-out timer and a marker for the others. A partner revives them by holding a key next to them: the reviver kneels and plays the **paramedics' CPR animation** (the clips the game's own medics use on injured peds, `Medic_CPR_IN` / `Medic_CPR_LOOP` / `Medic_CPR_OUT`; the dictionary name is found at runtime by probing candidates such as `medic`), and the revive takes **about 6 seconds**. Letting go cancels it. The revived player comes back with **half health**. **The mission fails, and everyone dies for real, only when every player is downed at once** (or a downed player's bleed-out timer runs out with nobody able to reach them, as a setting). The host is kept alive while downed, so the mission script never sees the protagonist die early: their death is only let through, triggering the normal "Wasted" and mission fail, once the whole team is down. Then all players die together. Outside missions, deaths are normal and per player (hospital respawn). |
| **Arrests** | Host arrest behaves as in the normal game. A client's arrest only sends that client to the nearest police station; it never fails the mission. |
| **Wanted level** | Mission-imposed wanted levels apply to every player. Outside missions, wanted level is per player, and police chase whoever earned it. |
| **Saves / autosaves / safehouses** | Host only. Client save prompts are suppressed. |
| **Money** | Every joiner has their **own wallet**. Mission payouts are mirrored: whenever a host story script pays the protagonist (`ADD_SCORE` on the host's player), each joiner gets the same amount. Everything else is per player: cash picked up (dropped by peds, robberies, pickups) goes to whoever collects it, and spending (guns, clothes, food, taxis, car washes, hospital and arrest fees) comes out of that player's own wallet. A joiner's wallet is stored per host campaign (`plugins/iv-coop/wallets.ini`, keyed by host and save) and restored when they rejoin that campaign. Their single-player money is put back when they leave. |
| **Friends, dating, activities, minigames, TV, internet, taxis** | Host-driven. Joiners can come along as passengers or spectators but can't start them. |
| **Ambient world** | Each client has its own traffic and peds around itself. Where players overlap, ambient entities near more than one player become networked (sim owner = nearest). |
| **EFLC (TLAD / TBoGT)** | Same system. The host picks the episode and it stays fixed for the session. |

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
  - **Limits of the slice (what remains):** (1) The game is the script's 10 frames shared by everyone (2 players: 5
    frames each). A full 10 each needs the script's frame counter held: `phone.cpp` shows statics are reachable
    (`find_thread(..)` + 0x58 -> statics, slot N = `l_U<N>`); `tenpinbowl`'s card is `l_U617[player]` with a
    452-byte stride (`_fU388` frame index, `_fU444` finished, `_fU4[]` rolls, `_fU344[]` frame scores) and the turn
    in `l_U264`, player count `l_U263` (= the player's group size). Swapping the slot-0 card per turn, and clearing
    `_fU444`/`l_U1353`, is the way, but the layout isn't verified against CE and the timing against the script's
    own frame transition needs a live look. (2) Partners have no camera: the lane cams are scripted on the host
    (stream `GET_SCRIPT_CAM` pos/rot/fov to the partner as a spectator cam), and their character isn't frozen
    while they aim. (3) A friend in the party (AI bowler, `l_U263 > 1`) isn't detected: its throws would be
    scored as the current player's. (4) Whether the lane's pins/ball reach partners as networked objects is
    unchecked; they only matter once (2) exists. (5) All of the input mapping (control ids, the stick gesture
    with a partner's pad) is from the decompile and needs an in-game run.
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
- **M10:** TLAD and TBoGT.
- **Co-op settings** (phone: Multiplayer > Co-op Settings; done 2026-10-26, sync/session_settings): the host's
  settings are the session's, saved in `iv-coop.ini [Session]` and sent to joiners. Friendly fire: **Off** (no
  lock-on, no damage), **On** (lock-on and damage, with the engine's own melee between players: partners' copies are
  damageable player peds, docs/research-native-netcode.md 7c), **No aim** (no lock-on, damage; the default); downed and
  revive; shared mission pay; radio sync; clock and weather sync; partner blips. Later: invasions allowed (M11),
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
    payouts), spawn together in one FBI car at a distance from the target, with an FBI loadout, see the target's blip, and leave or
    respawn on death; the invaded group's downed/revive rules apply to them as normal.
  - Outcomes: protagonist killed -> the invaded group fails (mission failed / wasted) and invaders get a
    reward; invaders all killed or the window closes -> they're sent home. Invaders return to their own
    session/world afterwards.
  - Friendly fire rules between invaders and the invaded host's police (police ignore FBI invaders or treat
    them as allies).
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
