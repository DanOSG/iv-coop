# GTA IV Co-op

A co-op mod for **Grand Theft Auto IV: The Complete Edition**. Up to **4 players** play the story together across
**GTA IV**, **The Lost and Damned** and **The Ballad of Gay Tony**.

One player hosts and plays their own save and story. Everyone else joins that world as their own character and plays
the missions alongside the host. Your own saves are never loaded or changed when you join someone else's game.

> **Early access.** Much of the mod is new and has not had much testing in real play. Expect rough edges, and please
> report what you find (see "Reporting bugs").

## What you need

- GTA IV: The Complete Edition on Steam, version **1.2.0.59**, on Windows.
- **Every player owns their own copy** of the game, and plays on their own PC.
- The Complete Edition includes both episodes, so nothing else is needed for The Lost and Damned or The Ballad of
  Gay Tony.

## Install

1. Download `iv-coop-setup-<version>.exe` from the releases page and run it.
2. The installer finds your game automatically and checks it is the supported version.
3. If you don't have **TJGM's FusionFix pack** yet, the installer downloads it from its official mirror and installs
   it (anything it replaces is backed up first). The mod needs it to run.
4. Optional: tick **Radio Restoration** to get the original Vladivostok FM songs in GTA IV and the club music in the
   Episodes.
5. Click install, then start the game normally from Steam.

Everyone in the group should install the same version.

## Host or join a game

Use your in-game phone and open **Multiplayer**.

- **Host:** choose **Host**. You get a **6-digit password**. Share it with your friends.
- **Join:** choose **Join** and enter the host's password. The connection goes through Steam, so nobody has to open
  router ports.
- **Leave** when you are done, or **Go to host** if you got separated.

If the phone menu isn't available, you can connect directly by IP address instead: the host presses **F10**, and a
joiner presses **F11** (it joins the address written in `plugins/iv-coop.ini` under `[Net] JoinAddress`). Pressing
the same key again leaves the session. The host's router port is opened automatically when the router allows it.

The host's story is what everyone plays. In The Lost and Damned or The Ballad of Gay Tony, everyone must be in the
same episode as the host. If you are in a different one, the mod tells you which to start.

### Your character and the host settings

- **Character:** choose male or female and any clothes, hat or glasses, with a live preview. Everyone sees your look.
- **Co-op Settings** (decided by the host): Team fire (Off / On / No aim), Revive, Shared pay, Radio sync, Weather
  sync, Partner blips, Scenes (host camera or your own), Ambient sync (shared pedestrians and traffic, off by
  default) and Invasions.

### What to expect

Partners are real characters: they take damage, ragdoll and catch fire. On missions a deadly hit leaves you down, and
a friend can revive you. Mission characters, cars, sounds, cutscenes, the radio and the weather are the same for
everyone. Each partner keeps their own money for each host. Press **J** to scream.

## Invasions (optional)

When the host is on a mission or has three or more wanted stars, and Invasions is switched on in the settings,
another group of up to four players can invade as FBI agents and hunt the host's character. Invaders can't respawn
and can only revive each other; if they all go down, the invasion fails. Start one from phone > Multiplayer >
**Invade**.

## Reporting bugs

Please attach the logs. They only contain the mod's own information.

- In the game: phone > Multiplayer > **Save logs**, or press **Ctrl+Shift+L**. A zip appears on your **Desktop**.
- If the game won't start: open the installer and click **Collect logs**.

Send the zip with a short description of what you were doing (which mission, what went wrong).

## Uninstall

Run the installer again and click **Uninstall**. It removes the mod. The FusionFix files it installed are left in
place, and the backups it made stay in your game folder in case you want to restore anything.

## Known limitations

- Early access: invasions, bowling, pool and darts, the character creator, shared pedestrians and traffic, and the Episodes support
  have had little play-testing.
- The Episodes' own minigames and some HUD meters aren't fully shared yet.
- The radio can hiccup briefly when a song changes.

## Disclaimer

This is an unofficial fan project. It is **not affiliated with, endorsed by or sponsored by Rockstar Games or
Take-Two Interactive**. Grand Theft Auto and related names belong to their owners. You need your own legal copy of
the game. Use the mod at your own risk.

## For developers

See [docs/FEATURES.md](docs/FEATURES.md) and [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

Needs Visual Studio 2022 with the "Desktop development with C++" workload, and GTA IV CE with
[FusionFix](https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix) installed (it provides the ASI loader).

```
cmake --preset x86
cmake --build --preset debug
```

The post-build step copies `iv-coop.asi` into `<game>/plugins/` (a running copy is renamed to `*.old` first). Use
`-DIVCOOP_GAME_DIR=...` if the game is elsewhere, or `-DIVCOOP_DEPLOY=OFF` to skip the copy. On first run the plugin
writes `plugins/iv-coop.ini` with all settings; check `plugins/iv-coop.log`. F8 toggles the debug overlay.

Only one game runs per PC, so `build\Debug\iv-coop-bot.exe --name Alice --radius 3 --retry` stands in for another
player after you host with F10. For local tests set `[Net] BindAddress=127.0.0.1`.
