# Darts (darts_trigger + darts_launcher + darts) - research for per-player seats

Sources: `research/GTAIVMissionScripts/{IV,TLAD,TBOGT}/darts*.txt`. Conventions as in pool.md: array `l_U<n>[k]` = word
`n+1+k`; struct field `_fU<b>` = word `base+b/4`; `st[n]` = `*(int32**)(find_thread("darts")+0x58)` + n.
**The game is 301, not 501, in all three games** (IV darts :1334-1344, TLAD :1386-1396, TBOGT :1425-1435): one leg,
no sets, double-out (last dart must be a double or the bull). Strictly 1v1 in the script: seat 0 human, seat 1 AI.

## 1. How it starts

- `darts_trigger` (IV, 37 lines) is **not** a launcher: it is a mission helper (loads the DARTS text, teleports the
  player to the bar at 1153,735,35, prints "GO_TO_BAR", terminates). Ignore it. (TLAD/TBOGT have the same stub.)
- **`darts_launcher`** is an object brain (one instance per dartboard; the board object arrives in a static:
  IV `l_U35` -> `l_U11 = l_U35` at :14; TLAD `l_U43` -> `l_U13`; TBOGT `l_U44` -> `l_U15`). It detects the context
  button and starts `darts`. Launch: `REQUEST_SCRIPT("darts")`, `l_U9._fU4 = board`,
  **`START_NEW_SCRIPT_WITH_ARGS("darts", &l_U9, 2, 1024)`** (IV :1013-1020 in `sub_6436`; TLAD :1262, args `l_U11`;
  TBOGT :1144, args `l_U13`). Friend scripts (`romandarts`, `jacobdarts`, `packiedarts`, `claydarts`, `jimdarts`,
  `terrydarts`) never start darts; they only bring the friend to the bar.
- Argument block, **2 words**: `_fU0` = opponent identity index (picks the name texture and voice strings:
  IV 0 Roman, 1 Packie, 2 Jacob, 3 Brucie, 4 Dwayne, 5 Michelle, 6 Kate, 7 Alex, 8 Kiki, 9 Carmen, other =
  "PLAY_BAR_GUY"/`name_opponent`, `sub_5415` darts.txt; launcher default 11 :13; TLAD default 4, 0/1/2 =
  Clay/Jim/Terry by model via `sub_6793`; TBOGT default 2, 0/1 = Gay Tony/Luis via `sub_6447`), `_fU4` = board object.
  darts copies it at start: IV `l_U399 = {l_U424}; l_U257 = l_U424._fU4;` (:23-24: args words 424/425, copy 399/400, board
  `l_U257`); TLAD `l_U410 = {l_U435}` (copy 410/411, args 435/436), `l_U257`; TBOGT `l_U411 = {l_U436}` (copy 411/412,
  args 436/437), board `l_U259`.
- **Opponent ped is NOT in the args.** IV: `GET_GROUP_MEMBER(player group, 0, ref l_U262)` (darts :444-446); the
  launcher puts the opponent in the player's group before starting (friend already is, group size == 1 and
  `sub_1143(friend)` :99-112; stranger via `SET_GROUP_MEMBER(group, l_U12)` :150). darts then `REMOVE_CHAR_FROM_GROUP`,
  `CLEAR_CHAR_TASKS_IMMEDIATELY`, `SET_CHAR_COORDINATES(l_U262, l_U271 - 0.18 z)`, heading `l_U260+25`,
  `FREEZE_CHAR_POSITION(l_U262,1)` (:447-454) and sets `l_U396 = 1`. TLAD/TBOGT: opponent = **global**
  `g_U9218` (TLAD, read at darts :433-441) / `g_U9267` (TBOGT :443-451), written by the launcher just before the start
  (TLAD launcher :108-128, :207-237, :257-277; TBOGT :106-121, :194-199): one friend -> that friend; several ->
  the player picks (keys 115/116/117 or pad 16/17/14, help DRT_01K/L/M); none -> the stranger.
- **Ambient path has an AI stranger**: `CREATE_CHAR(4, l_U32 = 1844702918, ...)` as `l_U12` (IV launcher :562;
  TLAD :694 `l_U14`; TBOGT :577 `l_U16`), "PLAY_BAR_GUY". It must be alive and not busy (launcher :102-110).
- TLAD/TBOGT darts also seat up to 2 more friends/group members as **spectators** (TLAD :445-478, TBOGT :455-490:
  `REMOVE_CHAR_FROM_GROUP`, `SET_CHAR_COORDINATES` to `l_U374[]` spots) - IV has none.

## 2. Seats and input

Two seats, hard-coded by index, not by a flag: **seat 0 = the local player = human; seat 1 = AI**.
`sub_14797` (IV :1369-1390): `if (l_U331 == 0) sub_14968() /*human*/ else { sub_20425(); sub_20924(); } /*AI aim+throw*/`.
The human's ped is always `GET_PLAYER_CHAR(GET_PLAYER_ID())` (`sub_215`), no ped/pad argument exists, so a partner
can only play by **answering the generic reads**. All reads are pad 0 / (2, control):

| native | IV line (TLAD / TBOGT) | meaning |
|---|---|---|
| `IS_CONTROL_JUST_PRESSED(2,4)` (kb, `!IS_USING_CONTROLLER`) or `(2,77)` | 1412, 1432 (1464, 1484 / 1503, 1523) | start aiming / **throw** (release, `sub_15009`) |
| `IS_CONTROL_PRESSED(2,77)`, `(0,77)` | 380, 1405, 3558 (372, 3527 / 377, 3556) | confirm, hold, "another game" |
| `IS_CONTROL_PRESSED(2,78)` (& not 77) | 330 (322 / 327) | context/back (flag `l_U421`) |
| `IS_CONTROL_JUST_PRESSED(2,23)` / `IS_CONTROL_PRESSED(2,23)` | 343, 357, 3577 (335, 3546 / 340, 3575) | quit |
| `IS_CONTROL_PRESSED(2,1)` + kb, `IS_BUTTON_PRESSED(0,6)` | 1604 (1656 / 1695) | faster aim (`l_U372`) |
| `IS_CONTROL_PRESSED(2,2)` + kb, `IS_BUTTON_PRESSED(0,7)` | 1616 (1668 / 1707) | hold breath (steady) |
| `GET_POSITION_OF_ANALOGUE_STICKS(0,...)` | 1675 (1727 / 1766) | aim (threshold +-40) |
| `GET_MOUSE_INPUT`, `GET_KEYBOARD_MOVE_INPUT` | 1710, 3338 (1762, 3304 / 1801, 3333) | aim fallback; scoreboard cam |
| `IS_CONTROL_JUST_PRESSED(2,77)` | 2940 (`sub_32012`, message wait) | skip message / skip opponent's turn |
| `IS_KEYBOARD_KEY_PRESSED(31)` | 3390 | debug (`l_U0 = 4`, state 4 unused) |

AI seat input: none (it does not read pad). While seat 1 plays, pressing 2,77/2,4 (`sub_14806` :1403-1418) sets
`l_U1 = 3` = "skip": `sub_36776` (:3328, IV) throws the AI's 3 darts instantly (`sub_20425`/`sub_20924` + `sub_23198`/`sub_26709`).

## 3. State and score

Mapping by aligning identical code; **TLAD is IV's numbering up to ~395 and +11 after; TBOGT is IV +2 for everything
from the start** (its `l_U0=55,l_U1=50` extra statics). `[n]` = array length.

| meaning | IV | TLAD | TBOGT | notes |
|---|---|---|---|---|
| game state | l_U0 | l_U0 | l_U2 | 0 setup, 1 play, 2 game over ("ANOTHER_GAME"), 4 debug |
| per-dart sub-state | l_U1 | l_U1 | l_U3 | 0 aim, 1 dart in flight, 2 scoring, 3 skip AI turn |
| **current seat** | l_U331 | l_U331 | l_U333 | 0 human, 1 AI |
| dart # in the turn (0-2) | l_U332 | l_U332 | l_U334 | |
| last dart multiplier 1/2/3 | l_U333 | l_U333 | l_U335 | set :2104-2118 (IV) |
| **score remaining `[2]`** | l_U328 (words 329,330) | l_U328 | l_U330 (331,332) | init 301 each :1343-1344 / 1395-1396 / 1434-1435 |
| score at turn start `[2]` (bust revert) | l_U252 (253,254) | l_U252 | l_U254 | read :2355; written :2986; set -1 :2481-2482 (!) |
| turn history `[2][7]` | l_U336 | l_U336 | l_U338 | word of seat s, col c = base+2+8s+c; col0 = 301, rows of end-of-turn scores :3016-3033 |
| points scored this turn | l_U354 | l_U354 | l_U356 | 180/140 detection :2352+ |
| human darts thrown (stat) | l_U355 | l_U355 | l_U357 | |
| "checkout hint shown" | l_U369 | l_U369 | l_U371 | |
| dart structs `[2][3]` x 26 words | l_U2 | l_U2 | l_U4 | dart(s,d) = base+2+79s+26d; +23 `_fU92` points, +24 `_fU96` landed, +25 `_fU100` in flight |
| opponent ped | l_U262 | l_U262 | l_U264 | |
| has opponent | l_U396 | l_U407 | l_U408 | |
| board object | l_U257 | l_U257 | l_U259 | |
| args copy | l_U399 | l_U410 | l_U411 | `_fU0` opponent idx |
| scoreboard render target id | l_U375 | | | scoreboard is drawn onto the blackboard texture |

Score flow (IV): `sub_26709` (:2327-2420) after each dart: `l_U328[seat] -= dart points`, `l_U354 += `; if score == 0 and
(bull or double) -> `sub_26890` (:2422) = **win**: `l_U331 == 0` -> "PLAYER_WIN", stat 319 (:2431), `sub_2187(0)`, else "PLAYER_LOSE",
`sub_2187(1)`; then `l_U0 = 2`, `l_U331 = 0` reset (:2466-2479). Bust (score < 0, == 1, or 0 without double):
`l_U328[seat] = l_U252[seat]` (:2355) and the turn ends. After 3 darts / bust `sub_32182` (:2957-3000) clears the
dart structs, `l_U252[seat] = l_U328[seat]`, records history (`sub_32621`), `l_U332 = l_U354 = 0` and **flips the seat**
(0->1 prints SWITCH_OPPONENT + help HELP_SKIP then waits `sub_32012`; 1->0 prints SWITCH_PLAYER). A rematch
(`(2,77)` at state 2 :3558) sets `l_U331 = 1; sub_14422()` which resets both 301s and flips to seat 0 (the human always starts).
**Winner** in state 2: the seat whose `l_U328[seat] == 0` (`l_U331` was reset to 0 after the win, so do not read it).
No legs/sets exist. TLAD/TBOGT lines for the same blocks: score apply 2382/2421, bust revert 2407/2446, game over
`l_U0=2` 2518/2557, seat flip 2945-2967/2974-2996, init 1386/1425, rematch 3527/3556.
Quirk: after a bust, `sub_26709` continues past `sub_32182` and does `l_U332++` (IV :2409), so the next seat starts at dart index 1
(looks like 2 darts for the next turn); not verified in game.

## 4. What the script does to the peds and the world

Almost nothing during turns: **there are no throw/idle animations**; darts are script objects (`CREATE_OBJECT` per dart in
`l_U2[s][d]._fU0`, flight simulated by moving the object each frame `sub_21945`, a cursor object `l_U258`, scoreboard drawn
with `DRAW_SPRITE`/`DISPLAY_TEXT_WITH_NUMBER` into the in-world blackboard via `SET_TEXT_RENDER_ID`, rows for exactly
seat 0 and seat 1: name textures `name_niko` (seat 0) and `name_<opponent>` by `l_U399._fU0`, `sub_5415`, :511-523).
Setup (state 0 `sub_2726` :388-): fade out, `SET_PLAYER_CONTROL(player,0)` (:400), `DISPLAY_RADAR(0)`, `DISPLAY_HUD(0)`,
`HIDE_CHAR_WEAPON_FOR_SCRIPTED_CUTSCENE(player,1)`, **`SET_CHAR_COORDINATES(player, l_U268)` + heading `l_U260-35`**
(:557-558; TLAD :595, TBOGT :607) = the oche; opponent at `l_U271` heading `l_U260+25`, frozen (:452-454);
`CLEAR_AREA(l_U300, 4 m)` (:41). Board position/heading comes from the object (two bars: 1151.5,731.4,34.9 and
-439.2,457.6,8.9, :412 and the `else` below it). Cameras: scripted `CREATE_CAM(14)` `l_U291/292/293` + interp cam `l_U294`
(:1291-1328 `sub_14xxx` setup; :972-978 intro cams) - host-local. At the end `TASK_LOOK_AT_CHAR` both ways (:2457-2464), then
`SET_PLAYER_CONTROL(player,1)`, `SET_GROUP_MEMBER` back for friends (:118-145) and the stranger walks away. So **a partner's
character must only stand at the oche (puppet position) and look at the board; the throws are objects**.

## 5. Minimal, safest mod approach

Primitive: the host script keeps seat 0 as the single "human window"; the mod swaps **per-player score data** in and out
of seat 0 (and keeps seat 1 unused) exactly like bowling_sync's card swap, so any number of humans can take turns:

Per human `p` store `{ score, history row (7 words), turn-start snapshot }` = `st[329]`, `st[338..344]`, `st[253]` (IV; add
the offsets from the table for TLAD/TBOGT). Initialise each to 301 / zeros / 301.

1. **Start (all humans).** After `darts` starts (poll `find_thread("darts")`; the script has not read the opponent yet
   when it is first seen, it yields at its first `WAIT(0)` :29): IV - make the group's member 0 the partner's puppet
   (`REMOVE_CHAR_FROM_GROUP` stranger, `SET_GROUP_MEMBER(player group, puppet)`) or let it keep the stranger and poke
   `st[262]` later; TLAD/TBOGT - write the global `g_U9218` / `g_U9267` before the start (hook the launcher's
   START_NEW_SCRIPT_WITH_ARGS for "darts"), or poke `st[262]` / `st[264]` after init. Position is done by the script
   (`l_U271`). For a 1v1 with a partner there is no need for an AI opponent: seat 1 is never used (below) and the
   opponent ped is only a spectator/look-at target. The opponent identity word `_fU0` can be poked at `st[399]`
   (IV) / `st[410]` / `st[411]` before `sub_2726` reads it (:511-523) to choose the name texture ("name_opponent" is default).
2. **Rotate players.** Each frame while `st[0] == 1` (state play; `st[2]` TBOGT): detect end of a turn = seat flipped 0 -> 1
   with `l_U332 == 0` and `l_U1 == 0` (`sub_32182` has just run; the script is parked in `sub_32012`'s wait loop or
   the main `WAIT`). Then: save seat 0's `{l_U328[0], l_U336[0][0..6], l_U252[0]}` into player `cur`; `cur = (cur+1) % n`; load the next
   player's data into those words; **write `l_U331 = 0`, `l_U332 = 0`, `l_U354 = 0`, `l_U369 = 0`, `l_U1 = 0`**; set
   `l_U252[0] = loaded score` (the script just overwrote it with the previous player's score :2986); `CLEAR_HELP()` to drop
   HELP_SKIP. The seat-1 AI therefore never throws; the helper `sub_36776` is never entered. Also reset a partner's turn:
   `SET_PLAYER_CONTROL` is not touched by darts per turn, so the host's own input must be gated by the hook.
3. **Input.** While `cur` is a partner, answer the table's reads (all of `(2,4) (2,77) (0,77) (2,78) (2,1) (2,2) (2,23)`,
   `IS_BUTTON_PRESSED(0,6/7)`, `GET_POSITION_OF_ANALOGUE_STICKS(0)`, `GET_MOUSE_INPUT`, `GET_KEYBOARD_MOVE_INPUT`,
   `IS_USING_CONTROLLER`) with the partner's input; that is `activity_seats::set_active(partner)` (pad -1 = generic only).
   Game-over prompt (`(2,77)` rematch, `(2,23)` quit) is answered by the host. The existing minigame_sync team mode
   (everyone feeds the host's single seat) already provides the input transport.
4. **Winner / end.** When `st[0]` becomes 2: winner = player `cur` (that is the one whose `st[329] == 0` after the swap).
   Before the rematch reset (`sub_14422` writes 301/301 and zero history) reload every player's data to 301; clear the saved
   scores on `(2,77)`. Win/loss side effects are the host's (stat 319 "wins" :2431, `REGISTER_BEST_POSITION(323, l_U355)`,
   `sub_2187(0/1)` friend-activity result and friend mood globals `g_U64664-64671` in `sub_27627/28181`).
5. **Scoreboard.** The script's board has only 2 rows: seat 0 (labelled NIKO) and seat 1 (opponent name). With the swap, row 0
   shows whoever is up and row 1 stays at 301 (or whoever you park in seat 1: write `l_U328[1]`/history row 1 with the
   *previous* player's data so the board shows the last two scores). A real N-player board must be drawn by the mod (DrawRect/text, as bowling_sync does) and
   hide nothing (the blackboard is a render target on the object).

**Alternative for exactly 2 humans (host vs partner)**: keep seat 1 and write the AI replacement - not recommended: `sub_20425/20924` are
the AI's aim+throw, they create the dart objects and compute `_fU44/_fU32` points, so replacing them means reimplementing
`sub_15939`/`sub_17758`/`sub_15009` flow for seat 1. The window approach reuses the human code path.

## 6. Risks / unknowns

- Seat flip timing: the flip happens inside `sub_32182`, which can run mid-`sub_26709` (bust branch) and is followed by
  `l_U332++` and message waits; always rewrite `l_U332`/`l_U331` and do it only when the script is parked at a WAIT; test
  bust and checkout turns and the "skip" path (`l_U1 == 3`).
- `l_U252[0]`/`[1]` start-of-game value: the script only writes -1 after a win (:2481) and the turn-end value; the initial
  value is the static's compiled initialiser (unknown, not in the decompile). Always write it when loading a player, and set both to 301 at game start.
- Darts, scoreboard and cameras are host-only objects: partners need the scene camera/pose stream (minigame_sync already
  streams table cameras for `darts`) and object sync of the dart models (937660317, 2063078697, -172899370, `l_U261`).
- The thrower is always the host's ped at `l_U268`; a partner's puppet should be teleported there for their turn (script never moves it).
  The host's ped is hidden in a scripted-cam context only by the camera position.
- Spectators (TLAD/TBOGT) and the stranger are group members re-added at cleanup (`SET_GROUP_MEMBER`, :118-145); a partner puppet must not be left in the
  player's group (IV: the opponent selection reads group member 0) after the game or the story group logic adopts it.
- TLAD/TBOGT global `g_U9218`/`g_U9267` is reset to `nil` by the launcher every frame (launcher :195 / :184); write it only
  in the same tick as the start. Locating the global address needs the mod's globals access; poking `st[262]/[264]` after
  init avoids it but the opponent is first placed on the old ped.
- Quit from any human: `(2,23)` ends `darts` (sub_206 cleanup resets `SET_PLAYER_CONTROL`, deletes darts/board objects); make sure
  the host cannot quit mid partner turn by accident (host reads are gated by the hook).
- Unknown: an unnamed PC native (`# -NULL-0xc27bfa()`, darts :348, :1612; TLAD :340, :1664; pool :1104) is called in aim code
  (probably a mouse/raw-input state check); its result is not controlled by our hooks.
