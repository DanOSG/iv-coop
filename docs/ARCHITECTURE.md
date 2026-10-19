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
  2. **Steam join:** invites and "join game" through Steam friends, using Steam's networking with its free relay
     as the fallback for strict NATs. No server of ours is involved. It still needs checking which Steamworks
     networking interfaces the game's `steam_api.dll` exposes under app 12210.
  3. Direct IP + manual port forward always remains as a fallback.
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
- Only the host is the protagonist (Niko / Johnny / Luis).

## Story co-op: how every system is handled

| System | Handling |
|--------|----------|
| **Joining mid-campaign** | The client receives the host's **world progression state**: island/bridge unlocks, IPL and interior state, the open/closed state of story buildings, time and weather, and the active mission. Its own save is never loaded or touched. |
| **Mission scripts** | Run on the host only. On clients, script launching is suppressed apart from a whitelist of HUD/system scripts. Mission start blips are replicated, but only the host can start missions. |
| **Mission entities** (peds, cars, objects, pickups) | Networked entities: the host is script owner, the nearest player is sim owner (principle 2). Clients can shoot, ram and kill them; damage goes to the sim owner, and death/destroy events go back to the host's script copy. |
| **Riding with story characters** | Seat occupancy is synced. Joiners can take any **free** seat in any vehicle, including NPC-driven mission cars. When a script tasks a ped into a specific seat, that seat is **reserved**. A joiner sitting in a seat the script needs is moved to another free seat or gently ejected. |
| **Own vehicles** | Joiners drive whatever they like, anywhere. Driver is sim owner, passengers receive. |
| **Cutscenes** (`.cut`) | Host start/skip/end are replicated. Every client plays the same cutscene locally **wherever they are**. Remote players are never inserted; their peds are hidden, frozen and invulnerable meanwhile. Afterwards each client goes back to where they were, and a "go to host" option is available. |
| **Scripted cameras / in-engine scenes** | Same as cutscenes: the host's script camera state is streamed, and clients either watch it or keep control. Configurable per scene during the audit. |
| **Dialogue, phone calls, subtitles** | Replicated as "play line / show subtitle" events, so all players hear and read the same thing. Only the host's phone takes calls. |
| **Objectives, HUD text, blips, GPS route** | Mirrored from host script state. |
| **Mission teleports / fades** | The host is moved as normal. Clients are **not** pulled along (no range limit) but get a one-key "join host" teleport. |
| **Fail conditions** | Decided by the host script. Client actions can cause failures, e.g. killing a target who must live, or alerting a tail. |
| **Deaths / arrests** | Host death or arrest behaves as in the normal game. A client's death or arrest only respawns that client at the nearest hospital/station; it never fails the mission. |
| **Wanted level** | Mission-imposed wanted levels apply to every player. Outside missions, wanted level is per player, and police chase whoever earned it. |
| **Saves / autosaves / safehouses** | Host only. Client save prompts are suppressed. |
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
- **M3:** vehicles, including passengers in any vehicle and seat reservation.
- **M4:** networked entities with **script owner vs sim owner** and migration. Testbed: entities spawned on the
  host while a client is across the map.
- **M5:** combat, damage, deaths/respawns, wanted level rules.
- **M6:** story layer: client script suppression, progression-state sync on join, and replication of blips, HUD,
  subtitles and dialogue.
- **M7:** cutscenes and scripted cameras, mission teleport/"join host".
- **M8:** mission audit: play IV start to finish with 4 players and fill in `docs/missions.md`, applying
  concessions as needed.
- **M9:** MP character creator. It doesn't depend on the other milestones, so it can be pulled earlier as a side
  task.
- **M10:** TLAD and TBoGT.
- **Connectivity:** UPnP port mapping, then Steam invites/relay (see Shape). This can land any time after M2.
- **Final: one-click installer.** It finds the Steam install, checks that `GTAIV.exe` is 1.2.0.59, and installs
  FusionFix, the radio downgrade and iv-coop (plus a default ini). It also offers to uninstall and restore.
  Third-party pieces are fetched from their official release sources at install time rather than bundled, so we
  never redistribute other people's files or Rockstar's audio. Their versions are pinned and verified by hash.
