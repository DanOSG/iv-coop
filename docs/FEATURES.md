# iv-coop: GTA IV story co-op

iv-coop turns **GTA IV: The Complete Edition**'s single-player story into a co-op game for **up to 4 players**
(a host + 3 partners). The host plays their own save and story; partners join that world as their own characters
and play the missions alongside, and their own saves are never loaded or changed. It's one ASI plugin, set up by a
one-click installer. Status: version 0.6.1; much of it is new and still being tested in play (see the end).

## Getting in
- **Installer:** finds GTA IV, checks it's the supported version (1.2.0.59), installs TJGM's Drag n' Drop pack
  (FusionFix + fixes) from its official mirror if it's missing (backing up anything it overwrites), installs the mod,
  and can uninstall it. Optional: **original Vladivostok FM songs in GTA IV, club music only in the Episodes**
  (Tomasak's Radio Restoration "split", from its official GitHub release).
- **Phone menu:** Multiplayer → Host / Join / Leave / Go to host / Co-op Settings / Character / Invade.
- **Steam:** join with a 6-digit password through Steam lobbies and Steam's relay (no open ports needed);
  joining by IP also works, with automatic router port opening (UPnP).

## Players
- **Partners are real player characters**, not stand-in NPCs: they take real damage, react to hits, ragdoll, catch
  fire (with visible flames), can be locked on to (per settings) and use the game's own melee between players.
- **Character creator:** male or female, every clothing piece, hat and glasses, live preview; saved locally, seen by
  everyone.
- Smooth walking, running, aiming and shooting; cars with passengers in any seat; health bars.
- **Downed and revive:** on missions a lethal hit leaves you down in an injured pose; a partner can revive you.
- **Wallets:** each partner keeps their own money per host; mission pay can be shared.
- **Scream key (J)** for fun.

## Story co-op
- **Story display:** mission text, objectives, help, blips, routes and the mission HUD (timers, counters, bars) for
  everyone.
- **Mission characters, vehicles and objects** exist for everyone (the nearest player simulates each one).
  Enemies and allies walk, aim, shoot, take cover, ragdoll and ride vehicles as on the host, including scripted
  animations, chained action sequences, sitting, scenarios, hands up and cowering. Their clothes and props match the
  host's; props stay attached to hands and cars.
- **Vehicles:** colours, liveries, extras, dirt, doors, windows, tyres, sirens and lights (from scripts and crashes).
- **Script explosions, fires and particle effects**, without damage applied twice.
- **Sounds:** positional mission sounds, jingles and music streams (the host's own phone and menu sounds stay
  local).
- **Pickups:** any player can grab a mission item and it counts for the host's mission.
- **Dialogue** plays in 3D from the speaker; phone calls reach partners too.
- **Cutscenes:** everyone sees them from the cutscene camera, and partners can walk around inside them as their own
  character.
- **Scripted camera scenes:** everyone sees the host's shot; partners keep control.
- **Regroup:** when the host's story moves on (the host is moved by a script, a scene ends, a mission starts),
  partners who are far away are brought next to the host on safe ground, or into a free seat of the host's car (or
  their own car right behind it) when the host is driving.
- **World state:** joiners get the host's progress: island unlocks, buildings, interiors, roads, doors, time and
  weather.
- **Radio:** every station plays the same song, ad, DJ line and talk segment at the same moment for everyone. In a
  car only the front seats choose the station (like GTA Online); the back follows.
- **Friend outings:** the friend comes along for partners too, and partners who drink with the host get drunk with
  the wobbly walk and camera.
- **Multiplayer bowling:** partners take turns on the host's lane, each playing a full 10 frames on a shared
  scorecard, watching the lane camera; the AI friend plays its own turns.

## The Lost and Damned and The Ballad of Gay Tony
- Co-op works in the Episodes too: everyone plays the host's episode (a partner in another one is told which to
  start), with the episode's own phone menu and protagonist. Bowling there uses shared frames.

## Combat
- **Team fire** setting: Off / On / No aim (default No aim). Damage from guns, cars, explosions, melee and fire
  (molotovs) between players follows it.
- Partners can be locked on to like anyone else, without becoming the preferred target.

## Optional world sync
- **Ambient sync** (off by default): pedestrians and traffic near two or more players are shared, and each joiner's
  own duplicates are thinned out.

## Invasions (Dark Souls style)
- While the host is on a mission or has 3+ wanted stars, and Invasions is on, another group of up to 4 can invade.
  Sessions grow to 8 players.
- Invaders arrive together in an FBI car as FBI agents with an FBI loadout and hunt the host's character. They are
  kept out of the host's story, and damage between invaders and the invaded is always on.
- No respawns: invaders go down and can be revived by each other (only by each other). If all invaders are down at
  once the invasion fails and they return to their own world; if the host dies, it succeeds.
- **Elden Ring style banners** ("FBI INVADING", "FBI VANQUISHED", "TARGET ELIMINATED", "INVASION FAILED") in the free
  Cinzel font, with stinger sounds (drop your own WAVs in `plugins/iv-coop/ui/`).

## Settings (phone → Co-op Settings, decided by the host)
Team fire · Revive · Shared pay · Radio sync · Weather sync · Partner blips · Scenes (Host cam / Own cam) ·
Ambient sync · Invasions

## Networking
Packet size and flood guards, handshake timeout, clean drop-outs, a clear message on version mismatch, and a
per-player traffic log every 10 s.

## Developer tools (debug builds)
- **TestBot:** a headless partner (stands still, drives, rides, melees, replays a recording, invades, or stress-tests
  the host with `--chaos`).
- **Recording and replay** of sessions (Ctrl+Shift+R).
- **Mission runner:** loads a queue of mission saves in one game session behind your windows, answers the phone,
  walks to mission starts, auto-plays, records, and logs which game actions still aren't synced.
- Static analysis of the decompiled mission scripts, the game's text, and a mission-save fetcher.

## Known gaps
- Most 0.6.x sync features, invasions, bowling, the character creator, ambient sync and the cutscene stand-in are
  untested in play.
- The Episodes support is new and untested in play; their own minigames and HUD meters aren't synced yet.
- Not done yet: the small radio hiccup at song changes.
