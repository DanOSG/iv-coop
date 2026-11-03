# Tenpin bowling - research for "each human is their own bowler"

Sources: `research/GTAIVMissionScripts/{IV,TLAD,TBOGT}/{tenpinbowl,bowling_lane,bowl_trigger}.txt` and IV's
`{brucie,dwayne,packie,roman}tenpin.txt`. Line numbers below are in the named game's `tenpinbowl.txt` unless stated.
Statics `l_U<slot>`; a struct array has a size word at its slot, element i at slot+1+113*i (card = 113 words);
`_fU<off>` = slot + off/4. Current mod state: `src/sync/bowling_sync.{h,cpp}` (see end).

## How a game starts (the three scripts)
- `bowl_trigger` (IV 3288 lines, TLAD 3243, TBoGT 3247) is the lane's static trigger. It tests only the **host
  player's ped** (`sub_1413` = `GET_PLAYER_CHAR`) with `LOCATE_CHAR_ON_FOOT_3D` at the two foul-line offsets of the lane
  object (`l_U552[0]` IV), `REQUEST_SCRIPT("TENPINBOWL")`, `SET_PLAYER_CONTROL(player, 0)`, then
  `START_NEW_SCRIPT_WITH_ARGS("TENPINBOWL", &args, 1, 8192)` with `_fU0` = lane object (IV :306, TLAD :265, TBoGT :265).
  `GET_GROUP_SIZE(playerGroup)` is read there (IV :218, TLAD/TBoGT :177) only to gate; tenpinbowl re-reads it.
- **Ambient vs friend is not a different script**: `tenpinbowl` itself reads the host's group
  (`GET_GROUP_SIZE` + `GET_GROUP_MEMBER(group, 0, &friendPed)`, IV :676-683, TLAD :715-724, TBoGT :659-668) and removes that
  member from the group (`REMOVE_CHAR_FROM_GROUP`, :661) to bowl as card 1. So friend = "a member is in my group".
  IV friend scripts (`BrucieTenPin` etc., `main.txt` :2498, 2556, 2575, 2609) only run the outing (drive, arrive speech,
  result), they contain no bowling code (no `MINI_BOWLING`); the lane is still started by `bowl_trigger`.
- `bowling_lane` (IV 1102, TLAD 1004, TBoGT 1107) is lane scenery: spawns one ambient bartender/bowler `CREATE_CHAR(4, ...)`
  (IV :127, TLAD/TBoGT :88) and switches ped paths; it is not a participant and holds no scorecard.

## Statics per game (the bowler model is identical in all three)
Card i = `l_Ucards[i]` (113 words, layout as in `bowling_sync.cpp` "the script's card"): word 0 = **bowler ped**,
+1 rolls..., `_fU388` frame (0-9), `_fU444` finished. Turn = card index being played.

| | IV | TLAD | TBoGT |
|---|---|---|---|
| card array (size word) | `l_U617` (6) | `l_U589` (6) | `l_U379` (2) |
| card 0 ped (word 0) | 618 | 590 | **380** |
| card 1 ped | 731 | 703 | **493** |
| card 2 / 3 ped | 844 / 957 | 816 / 929 | (array is 2 long) |
| bowler count | `l_U263` | `l_U235` | `l_U25` (capped to 2 :665-668) |
| current card (turn) | `l_U264` | `l_U236` | `l_U26` |
| friend ped statics (group member 0, 1, 2) | `l_U1296/97/98` | `l_U1268/69/70` | `l_U606/607/608` |
| state variable | `l_U2146` | `l_U2134` | `l_U1068` |
| "is player" helper `GET_PLAYER_CHAR(GET_PLAYER_ID())` | `sub_2397` :525 | `sub_2256` :528 | `sub_2028` :499 |
| friend picked from group | :676-683 | :715-724 | :659-668 |
| card ped slots written (once) | `sub_6538` :855-875 | :897-917 | `sub_6316` :841-861 |
| ball attached to current card ped | :3309, 3598, 3607 | :3595, 3884, 3893 | :3534, 3823, 3832 |
| "player walked away" distance check | :2346 (>15 / >19 m) | :2632 | :2571 |
| AI flag (friend's turn) | `l_U264 != 0` :1452 | `l_U236 != 0` :1746 | `l_U26 != 0`, `l_U250` :3577, 3660 |

Only IV/TLAD size their arrays to 6 cards but only group member 0 is ever read into a ped static
(`l_U1296`, `l_U1268`, `l_U606`), so in practice a game has 2 bowlers: card 0 = host player, card 1 = friend. Cards 2/3
copy `l_U1297/98` (never assigned: ped 0).

Turn selection: `sub_15183` (TBoGT :1613-1668) sets `l_U26` from the cards' frame counters (`_fU388`) and finished
flags (`_fU444`); sets `l_U663 = 1` when all are finished (game over). TLAD/IV have the same function.

## Is the bowler ped really the card's ped? (yes, per card, including AI)
- The run-up, ball and anims are keyed on `l_Ucards[turn]._fU0`, not on the player: TBoGT `sub_40134` :5795 runs
  `TASK_FOLLOW_NAV_MESH_TO_COORD` + `TASK_CHAR_SLIDE_TO_COORD_HDG_RATE` to the foul line `l_U32` as a sequence on
  `l_U379[l_U26]._fU0`; the ball `l_U6[0]` is `ATTACH_OBJECT_TO_PED(ball, card ped, 1232, ...)` (:3534, 3823, 3832); the pick-up
  checks `IS_CHAR_PLAYING_ANIM(card ped, "MINI_BOWLING", "PICK_UP_L/R" | "NIKO_PICK_UP_L/R")` (:3592-3606), the
  throw/celebrate anims `MINI_BOWLING` are all on the card ped (many of the ~317 `l_U379[..]._fU0` uses, e.g. :3650-4119,
  :8120-8940 celebration/anim cases). Camera and pin logic are independent of the ped.
- **The script already animates a non-player ped as a bowler**: on the friend's turn (`l_U26 == 1`) the card-1 ped
  (friend, `l_U606` copy) does the whole run-up/throw/celebrate through the identical code, with random aim/power from the AI
  branch (`l_U250 = 1`). Card 1 is **not input-driven** though: the human-input branch is `l_U26 == 0` (e.g. :3578ff,
  `if (l_U26 == 0) ... SET_PLAYER_CONTROL`), so a human must occupy card 0 (hence the current card-0 swap).
- Only these still use the real player ped (`sub_2028()`, 20 sites in TBoGT :198, 323, 330, 334, 841, 888-889, 1270, 1391,
  2540, 2553, 2570-2571, 3535-3537, 3898, 5900, 5915, 6377, 8918, 8922; IV 19 sites :238 .. :8741; TLAD 21 :227 .. :8992):
  `CLEAR_CHAR_TASKS`/`DETACH_PED` at start/end, `SET_ROOM_FOR_CHAR_BY_NAME` (:889, bowling interior room), the position
  samples (:1270 lane side `l_U38`, :2570 distance to the lane `l_U32`, :6377), `GET_SCRIPT_TASK_STATUS(P, 29)` run-up
  wait (:3898, 5915), `SAY_AMBIENT_SPEECH` heckle (:5900), `TASK_PLAY_ANIM(P, IDLE_RELAXED)` (:3535).
- Sequence per turn (TBoGT state `l_U1068`): case 30 -> 34 (player waits with ball), 1/0 starts a throw: `sub_40134`
  walks the card ped to the line, state 24 checks `GET_SCRIPT_TASK_STATUS == 7` (task done) then teleports card 0's ped to
  `l_U35` and `ATTACH_PED_TO_OBJECT(cardPed, l_U18[3], ...)` (a dummy object that pins the bowler on the line; IV
  `l_U256[3]`, TLAD `l_U216[3]`).

## Minimal safe way: the partner's puppet becomes the bowler on their turn
Keep the existing card-0 swap (the mod saves/loads card words 1..112 per human). Additionally **swap word 0** (the ped) and
make the helper return the same ped:
1. On a partner's turn start (same hook point as `load_card`; the card is written before the next frame start, see header),
   write `card0.ped = partner puppet ped` (TBoGT static 380, IV 618, TLAD 590) in addition to words 1..112. When the host or
   a friend turn returns, write the host's real player ped back (cache it at game start from `sub_2397` / `GET_PLAYER_CHAR`).
   Cards are the single source of the bowler: ball attach, run-up, anim checks, `GET_OFFSET_FROM_CHAR` all follow word 0.
2. Hook `GET_PLAYER_CHAR` when the caller thread is `tenpinbowl` and the arg is the local player: return card 0's current ped
   (the active human). That fixes the 20 `sub_2028()` uses (room set, distance-to-lane, task-status wait, speech, idle anim)
   consistently so the exit-distance check (:2571, >15 m ends the game) measures the bowler, not the host who may be far away.
3. Input stays as today (hooks answer sticks/mouse/controls from the partner's input, which the mod already does).
4. Move the host's real ped out of the way for partner turns (it is no longer the card ped but the script still sets
   `SET_PLAYER_CONTROL(host, 0)` on card-0 turns): park/freeze it near the friend/bench spot (`l_U50` TBoGT, `l_U288` IV,
   `l_U50`-equivalent TLAD) and keep the host's control off; restore on the host's turn. `SET_PLAYER_CONTROL`,
   `DISPLAY_RADAR`, cameras still target the host's player id (cams are scripted; the mod streams them to partners).
5. Puppet driver: while the puppet is the card ped suspend its net-state driver (position, anim, tasks), because the script
   will `CLEAR_CHAR_TASKS_IMMEDIATELY`, `SET_CHAR_COORDINATES`, `TASK_FOLLOW_NAV_MESH_TO_COORD`, `ATTACH_PED_TO_OBJECT` and play
   `MINI_BOWLING` anims on it. Resume the driver when the card ped is switched back (and at game end: the script does
   `DETACH_PED`, `CLEAR_CHAR_TASKS*`, teleports card 0/1 ped to the exit spots, see TBoGT :3493-3510, :8918-8922).
6. Using the friend seat instead (writing `l_U606`/card 1 ped and `l_Uf` slots) does **not** work for a human: card 1 plays
   the AI branch. It can still seat a human as a *spectator/AI stand-in* (e.g. 2nd partner as the friend ped) but their
   throws would be random; avoid.
7. Up to 4 humans: still one script card 0 per turn (rotate ped + card words per frame as today). The bowler who is not
   throwing needs a parking spot; the lane has a single friend spot (`l_U50`/`l_U288`), so offset the rest.
8. Game end/cleanup: restore `card0.ped` to the host ped before the script's end block (TBoGT `sub_1516` :290-345 and
   :8910-8925 use `l_U379[l_U26]`/`sub_2028()`) or the cleanup will `DETACH_PED/CLEAR_CHAR_TASKS` the wrong ped and put the
   puppet back into the host's group (`SET_GROUP_MEMBER` only touches `l_U606`, the friend).
9. Check at runtime like `script_ok`: size word 2 (TBoGT) / 6 (IV, TLAD) at the card array, `DOES_CHAR_EXIST(card0.ped)` before
   and after a write; if the ped write fails to stick (reverted) fall back to the current host-ped-throws behaviour.

## What `bowling_sync` does today (for reference, read, unchanged)
- Host script stays authoritative; partners' input answered through hooks on `GET_POSITION_OF_ANALOGUE_STICKS`,
  `GET_MOUSE_INPUT`, `IS_CONTROL_PRESSED/JUST_PRESSED` (pad,control pairs (2,4)(2,77)(0,77)(2,87)(2,78)(0,78)(2,3)),
  `IS_USING_CONTROLLER` (`bowling_sync.cpp` :451-458, 685-765). The *host's character* throws for the partner.
- Scorecard swap: `l_U617[0]` etc. via `Layout kLayoutIV/TLAD/TBoGT` (card base+1+113 i; `load_card` copies words 1..112 and
  explicitly skips word 0 = the ped, "word 0 is the ped the script bowls with"); slots listed there match this document
  (players IV 263/TLAD 235/TBoGT 25, turn 264/236/26, cards 617/589/379, state 2146/2134/1068).
- Partners standing still at the lane on their turn (`SET_PLAYER_CONTROL(own, !my_turn)` :487) and watching `scene_cam`.
- Missing for the new goal: write word 0 (ped), hook `GET_PLAYER_CHAR` for `tenpinbowl`, a way to name the partner's
  puppet (`Player::ped` exists, host side), park the host's ped, suspend the puppet driver, and a partner-side visual of
  the puppet bowling (see risks).

## Risks / unknowns
- **Puppet drive vs script tasks** (largest): the puppet is net-state driven (kinematic, "slides"); task-based run-up
  (`TASK_FOLLOW_NAV_MESH_TO_COORD`) and anim playback may not run on it. `GET_SCRIPT_TASK_STATUS(P, 29) == 7` also reads
  "finished" when no task exists, so the wait is satisfied immediately (the script then teleports/attaches the ped), which
  may be acceptable (the bowler snaps to the line) but the walk will not be seen. A fallback is to run the walk by
  teleporting the puppet to `l_U32` then letting the script's `ATTACH_PED_TO_OBJECT` pin it.
- **Partner-side visuals**: the partner's own machine renders the host world; whether it shows the host-side puppet bowling
  (anims) or only the host-owned entity copy is unknown; its own character stands still. If anims on puppets are not
  mirrored, send an anim message (e.g. a small "play MINI_BOWLING anim X on my own ped" per script anim) and play it locally.
- `GET_PLAYER_CHAR` hook scope: must apply to thread `tenpinbowl` only; `bowl_trigger`, `bowling_lane` and the friend
  scripts keep the real host ped. Other natives keyed to player id (`SET_PLAYER_CONTROL`, `IS_PLAYER_PLAYING`) stay host.
- Word 0 is also read for `GET_ANIM_GROUP_FROM_CHAR(card ped)` at start (TBoGT :850-852, friend only) and
  `SET_ANIM_GROUP_FOR_CHAR(l_U606, l_U200)` at the end (:303); do not write the puppet there during init (written once at game
  start by `sub_6316`); only change slot 380/618/590 after `sub_6316` ran (state `l_U1067` >= 4 in TBoGT, `main` case 3
  :225).
- Model/room: puppet must be in the bowling interior room (`SET_ROOM_FOR_CHAR_BY_NAME(P, "E2_Bowl_room")` TBoGT :889, IV/TLAD
  `"Bowl_GtaMloRoom"`); the script only sets it for `sub_2028()` once at start, so set it on the puppet when seated.
- The ball model attaches at bone 1232 to a different model/skeleton (partner model may be a different protagonist: Niko / Johnny / Luis / the NPC animations "NIKO_PICK_UP_*" are Niko-specific).
  Puppets of other characters' models may mis-fit the "MINI_BOWLING" anim set.
- Friend group removal: with a friend the script removes it from the host's group (:661); keep that behaviour; puppets must not
  be put in the group at the end.
- Everything here is decompile-only and untested in game; slot numbers for the card arrays are confirmed in `bowling_sync.cpp`'s
  layout notes; ped words 380/493 (TBoGT), 618/731 (IV), 590/703 (TLAD) follow from the same layout (`cards + 1 + 113 i`).
