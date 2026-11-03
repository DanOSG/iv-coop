# Changelog

## 0.9.0

- Air hockey (The Lost and Damned, The Ballad of Gay Tony): one against one, a friend on the far paddle with their
  own controls.
- Races (The Ballad of Gay Tony): up to three friends race as themselves in their own cars, starting on the grid,
  each with their own finishing place.
- Golf, air hockey, arm wrestling and hi-lo cards cameras show only for the players taking part.

## 0.8.0

- Activities with everyone as their own character, each with their own score and turns:
  - Bowling: up to four players, each bowling as themselves (no longer the host's character throwing for you).
  - Darts: up to four players, each with their own 301 and a shared scoreboard.
  - Pool: one against one, a friend against the host; others queue and play the winner.
  - Golf in The Ballad of Gay Tony: up to four golfers.
  - Arm wrestling (one against one) and hi-lo cards (up to four) in The Lost and Damned.
- Balls, pins, darts and pucks are simulated once, on the host, and replayed for everyone at 60 updates a second with
  smoothing that follows their motion, so shots end exactly where they did on the host (no desyncs).
- Fixed a crash that could happen when joining a host far away (a partner's character was moved across the map in
  the same moment its player slot was reused).
- The team version of pool and darts from 0.7.1 is replaced by the above.
- Drinking nights out now make partners drunk too (it never triggered before).
- Street vendors, nightclub dancing (TBoGT) and base jumps (TBoGT) work for every player on their own.

## 0.7.1

- Multiplayer pool and darts, in all three games: everyone at the table plays your side against your friend,
  taking turns shot by shot (pool) or dart by dart (darts). Players at the table watch through the same camera.
- Bowling's full ten-frames-each mode now works in The Lost and Damned and The Ballad of Gay Tony too (TBoGT's
  alley takes two bowlers).
- Fixed some game actions that failed to hook up on every launch.
- The debug overlay shows the mod's version.
- New player README and this changelog.

## 0.7.0

- Co-op in The Lost and Damned and The Ballad of Gay Tony: weapons, armour and health that the story gives or takes
  now apply to partners too, and every player gets their own parachute.
- Race checkpoints and the race route show for everyone, and so do the Episodes' activity screens and minigame
  screens (darts, pool, air hockey, golf, drug wars).
- One-step log saving: phone > Multiplayer > Save logs, or Ctrl+Shift+L, puts a zip on your Desktop. The installer
  has a Collect logs button for when the game won't start. The previous launch's log is kept too.
- Fixed a crash at startup caused by one game action.
- Fixed the debug overlay showing in the banner font.
- Closing the phone while editing your character discards the edits.

## 0.6.1

- Installer: optional Radio Restoration, with the original Vladivostok FM songs in GTA IV and club music in the
  Episodes.

## 0.6.0

- Invasions: another group of up to four can invade as FBI agents while the host is on a mission or wanted. Invaders
  revive each other, the invasion fails if they all go down, and Elden Ring style banners announce the result.
  Sessions can reach 8 players.
- Multiplayer bowling: partners take turns for a full 10 frames each on a shared scorecard, with a lane camera.
- Only the front seats choose a car's radio station; the back seats follow.
- Sturdier networking: automatic router port opening, protection against oversized or flooding traffic, clean
  drop-outs, and a clear message when versions don't match.

## Earlier

0.5.0 and before: the base co-op game, including Steam and IP joining, the character creator, fighting and fire
damage between players, shared mission characters, cars, sounds, pickups and cutscenes, weather and radio sync,
shared pedestrians and traffic, and the phone menu.
