# Pool (pool_table + pool_game) - research for per-player seats

Sources: `research/GTAIVMissionScripts/{IV,TLAD,TBOGT}/pool_table.txt`, `pool_game.txt` (line numbers below are those
files; "IV/TLAD/TBOGT" columns). Statics: decompiler `l_U<n>`; array `l_U<n>[k]` = word `n+1+k` (word `n` is the count);
struct field `_fU<b>` = word `base + b/4`. `st[n]` in the mod = `*(int32**)(thread+0x58)` + n, as bowling_sync.cpp:290.

## 1. Start: who launches pool_game, with what

pool_game is started by **`pool_table`** only, for ambient and friend play alike. pool_table is an *object brain*: the
engine starts one instance per pool table and passes the table object in a static (IV `l_U36`, the loop is
`while (DOES_OBJECT_EXIST(l_U36))` :17). The friend scripts (`romanpool`, `jacobpool`, `packiepool`, `claypool`,
`jimpool`, `terrypool`) never start pool_game (no START_NEW_SCRIPT in them): they only bring the friend to the venue,
keep him in the player's group, and read the result from `g_U32898` afterwards (below).

Ambient vs friend is decided in pool_table state 2 / case 0 (IV :146-195):
- **Friend path**: player group size == 1, member is a friend (`sub_3096`), player within 2 m of the table
  (:150-163) sets `l_U9 = 1` (friend flag) and the opponent is the group member `l_U30`.
- **Ambient path**: a stranger is created at the table (**yes, there is an AI opponent**): `CREATE_CHAR(4, l_U35, ...,
  ref l_U31)` (:120, model = random gang/pedestrian, `sub_417` list of 23 models; GJAM models inside the room at
  1470,55,21 :90-). Stranger within 3 m of the table and the player within 2 m sets `l_U9 = 0` (:168-186).
  At the table whose object is inside (-421.5,1457.4,36.7)-(-415.5,1463.8,41.0) `l_U19._fU28 = 0` (:100) and no
  stranger is created (:111-114; inference: only the friend path works there).
- Then the context button (`sub_4383(2,0)` / `sub_6007(2, name, 0)`, help "PLH_01" or the friend's string) ->
  `SET_PLAYER_CONTROL(player,0)`, `REQUEST_SCRIPT("pool_game")` (:254) -> next frames fill the block and start (:297-311).

| | pool_table block (11 words) | friend ped | stranger ped | friend flag | START line |
|---|---|---|---|---|---|
| IV | `l_U19` (words 19-29) | `l_U30` | `l_U31` | `l_U9` | :309 |
| TLAD | `l_U490` (490-500) | `l_U501` | `l_U502` | `l_U478` | :581 (fill :569-580) |
| TBOGT | `l_U301` (301-311) | `l_U312` | `l_U313` | `l_U289` | :464 (fill :452-463) |

TLAD/TBOGT additionally pick the friend among 3 (Clay/Jim/Terry `l_U503-505`) or 2 (`l_U314-315`) candidates
(TLAD :310-323, :411-456; TBOGT :276-283, :365-370); the rest is the same.

Call: `START_NEW_SCRIPT_WITH_ARGS("pool_game", &block, 11, 1024)`. Block layout (**same in all three**; word offsets
from the block base `B`):

| word | field | value |
|---|---|---|
| B+0..2 | `_fU0[2]` seat peds (count, [0], [1]) | [0] = friend or stranger, [1] = `GET_PLAYER_CHAR(local)` |
| B+3..5 | `_fU12[2]` seat player indices | [0] = `nil` (=0), [1] = local player index |
| B+6 | `_fU24` | table object handle |
| B+7 | `_fU28` | stranger-created flag (ignored by pool_game) |
| B+8..10 | `_fU32[2]` seat pad indices | [0] = unset (0), [1] = 0 |

In pool_game the block arrives as args `l_U750..760` (IV) / `l_U722..732` (TLAD, TBOGT) and the first statement copies
it: **`l_U318 = {l_U750}`** (IV :120) / **`l_U285 = {l_U722}`** (TLAD :81, TBOGT :81). Live copy slots:
IV 318..328: ped[0]=319, ped[1]=320, pidx[0]=322, pidx[1]=323, table=324, pad[0]=327, pad[1]=328.
TLAD/TBOGT 285..295: ped[0]=286, ped[1]=287, pidx[0]=289, pidx[1]=290, table=291, pad[0]=294, pad[1]=295.
Seat 0 = opponent (AI/friend), **seat 1 = the local (host) player, always**.

## 2. Seats and human/AI

Two seats. pool_game state 0 case 0 hardcodes the flags: **`l_U56[0] = 0; l_U56[1] = 1;`** (IV :310-311; TLAD/TBOGT
`l_U58[0]=0; l_U58[1]=1;` :200-201). Words IV 57/58, TLAD+TBOGT 59/60. These are the only writes to the flag array
in the whole script, and every behavioural fork keys on `l_U56[seat]` (:389, 467, 594, 616, 652, 724, 888, 919, ...,
2704, 6048, 6108, 7445 in IV). AI seat (flag 0): aim/shot are computed (`sub_20137`, accuracy `l_U230` from the ped's
model, :596-632; "Pool AI fTemp" prints). Human seat: the script reads input and does nothing automatic.
Whose turn: **`l_U243`** (IV) / `l_U203` (TLAD, TBOGT); waiting seat `l_U244` / `l_U204`.

Input natives per seat. Only **sticks** and one button read a per-seat pad `l_U318._fU32[l_U243]`:
`GET_POSITION_OF_ANALOGUE_STICKS(pad,...)` IV :1056 (aim), :1281 (stroke), :6112 (helper, + `GET_MOUSE_INPUT` :6115),
`IS_BUTTON_PRESSED(pad,13)` :936. Everything else is pad-less / fixed pad and **not seat gated**:

| native | where (IV) | meaning |
|---|---|---|
| `IS_CONTROL_JUST_PRESSED(2,23)` | 393, 921, 1223, 2178, 2446, 2514 | quit / abandon (human seat's turn) |
| `IS_CONTROL_JUST_PRESSED(2,4)` (kb, `!IS_USING_CONTROLLER`) or `(2,77)` | 471, 841, 1195, 2211, 2415, 2455, 2530 | confirm / place cue ball / next / rematch |
| `IS_CONTROL_JUST_PRESSED(2,78)` | 1034, 1047, 1438 | help toggle / back |
| `IS_CONTROL_JUST_PRESSED(2,0)` or `IS_BUTTON_PRESSED(pad,13)` | 936 | toggle aim view (`l_U66`) |
| `IS_CONTROL_PRESSED(2,2)` / `IS_BUTTON_PRESSED(0,14)` | 1076 | fine-aim modifier |
| `IS_MOUSE_BUTTON_PRESSED(2)` / `IS_BUTTON_PRESSED(0,19)` | 1169 | english / cue mouse button |
| `GET_POSITION_OF_ANALOGUE_STICKS(0,..)`, `GET_MOUSE_INPUT` | 3951, 6219-6222 | english (cue-ball spin) helper `sub_42171`: **pad 0 hard-coded** |
| `GET_KEYBOARD_MOVE_INPUT` | 1056-1062, 1281.. | aim fallback when sticks are 0 |
| `IS_KEYBOARD_KEY_JUST_PRESSED(31/33)` | 2653-2662 | debug: force winner seat 1 / 0 (ignore, but note it) |

`l_U0` (IV) / `l_U2` (TLAD, TBOGT) is a "press already used" latch, cleared by `sub_66027` (:7473-7482) only when
`IS_BUTTON_PRESSED(0, 13/10/11/9/...)` are all false (pad 0, so the hook must answer those reads for the active seat too).
Note the existing minigame_sync claim "pool's pad index is 0 for the human side" holds: pad[1] = 0.

## 3. Turn and score state

Mapped by aligning identical code (difflib on normalised lines); **TLAD and TBOGT pool_game are identical in slots**,
IV differs. Line numbers: IV / TLAD / TBOGT.

| meaning | IV | TLAD, TBOGT | evidence (IV line / TLAD,TBOGT line) |
|---|---|---|---|
| args copy (see 1) | l_U318 | l_U285 | 120 / 81 |
| seat is human `[2]` | l_U56 | l_U58 | 310 / 200 |
| current seat | l_U243 | l_U203 | first set 378 / 375 (`=1`, seat 1 breaks); swaps 1701-1706, 2134-2139, 2423 |
| waiting seat | l_U244 | l_U204 | same sites |
| first breaker (alternates per game) | l_U267 | l_U227 | 379, 2425, 2431 |
| game state | l_U240 | l_U200 | `sub_9759(n)` :3299 |
| sub-state | l_U241 | l_U201 | |
| ball group per seat `[2]` | l_U260 | l_U220 | 0 open, 1 solids, 2 stripes, 3 = on the 8; set 1740-1774, 1809, 1831, 2150-2164 / 1737.., 3267-3272 reset |
| balls still on table `[16]` | l_U5 | l_U7 | read 1655-1670 (index 0-6 solids, 7 = 8-ball, 8-14 stripes, 15 = cue) |
| solids left / stripes left / all left | l_U257 / l_U258 / l_U252 | l_U217 / l_U218 / l_U212 | 1654-1670 |
| potted this shot: total / solids / stripes | l_U254 / l_U255 / l_U256 | l_U214 / l_U215 / l_U216 | used 1700-1830 |
| foul code (0 none, 1-7) | l_U263 | l_U223 | 1647 (`sub_53450(seat)`), 2073-2108 |
| **winner seat** (-1 none) | l_U264 | l_U224 | init -1 :86, 3267; set 1844, 2039 (black potted legally), 2129 (foul on the black -> other seat), 2638/2643 (debug keys) / 1841, 2036, 2126 |
| aim vector / aim angle | l_U353 / l_U108 | l_U320 / ? | 960, 1103 |
| "ball in hand / place cue ball" | l_U78 | l_U80 | 2182 sets, :587 clears |
| "this shot is the break" (inferred) | l_U59 | l_U61 | |
| resume-after-abandon | l_U74 | l_U76 | 2191 |

pool has no legs/sets/points: one frame to the 8-ball. Group assignment happens inside state 6 on the first legal pot
(:1727-1790). Per-seat state is already per seat, so **no data swap is needed for 2 humans**; only the 2-seat limit.

State machine (`l_U240`; IV case lines): 0 init (287; fade, loads "MINI_POOL" anims :355, `LOAD_ADDITIONAL_TEXT("POOL")`,
ends with `l_U243=1,l_U244=0,sub_9759(2)` :378-381), 1 place cue ball (387), 2 approach (570; moves the shooter, plays
anim), 3 aim (911), 4 stroke / power (1218; right stick or mouse pulled back and pushed forward, `l_U250`, :1281-1330),
5 shot animation + balls moving (1515), 6 evaluate pots/fouls, switch seat (1636), 7 game over (2245), 8 quit confirm
(2464). Win text: `if (l_U56[l_U264]) "WON" else "LOST"` :2354-2371 -> `sub_63624(0/1)` writes the friend-activity result
(`g_U32898._fU16`, no-op unless a friend activity runs: `if (g_U32898._fU0 == 54) return`). Rematch: `(2,77)` at
state 7 swaps breaker (:2414-2435); `(2,23)` ends the script (`sub_64741(0,0)` :7415 -> `sub_1032` cleanup).

## 4. What the script does to each seat's ped

Per turn (current seat `l_U243`), state 2 (IV :613-736): `sub_30721(ped, aim, 1)` = `CLEAR_CHAR_TASKS_IMMEDIATELY`,
`sub_1736` (collision on, unfreeze, visible), then **`SET_CHAR_COORDINATES` to the outer-stance spot around the cue
ball, `SET_CHAR_HEADING` along the aim, `FREEZE_CHAR_POSITION(ped,1)`**; then a task sequence
`TASK_PLAY_ANIM_NON_INTERRUPTABLE(0, "SHORT_SHOT_START" | "F_SHORT_SHOT_START" (by `IS_CHAR_MALE`, 4 variants l_U245
0-3 via sub_32983/33111/33235/33361), "MINI_POOL", 8.0, ...)` and, if `l_U78`, a looped `IDLE` of "MINI_POOL".
State 3/aim (956, 1202): `sub_37327(ref ped, aim, 0)` makes the shooter **invisible, no collision, and
`ATTACH_PED_TO_OBJECT` to the script's own invisible cue object `l_U317`** (model -809347846, host-local object; moved with
the aim, :5880-5905, `sub_37425`); states 4/5 show him again (`sub_37327(..,1)` 1242, 1355). State 5: shot anim from
sub_50942/51066/51186/51308 ("MINI_POOL", blend 9999.9) then `TASK_PLAY_ANIM(0,"IDLE","MINI_POOL")` (1521-1550).
Waiting seat: `sub_14835` (teleports to its spectator spot `sub_14846`, loops "MINI_POOL"/"IDLE") and
`sub_33926` (tracks/steps aside); `TASK_LOOK_AT_OBJECT(waiting, cue ball)` (:647, 1100). Both peds
`BLOCK_CHAR_GESTURE_ANIMS(ped,1)` every frame (:277) and are removed from the player group (:333-341).
Player (host) control: `SET_PLAYER_CONTROL(_fU12[seat],0)` on the human seat's turn (:409, 656), `..._ADVANCED(waiting,0,0,1)` (:728),
back to 1 for human seats at cleanup (2704-2709). Init also does `SET_PLAYER_CONTROL(sub_1634(),0)`,
`SET_EVERYONE_IGNORE_PLAYER` (:317-321; IV also `SET_CURRENT_CHAR_WEAPON`), `DISPLAY_RADAR(0)`, `CLEAR_AREA`/`CLEAR_AREA_OF_CHARS`
around the table (:361-364), `SET_USE_POOL_GAME_PHYSICS_SETTINGS`, `OVERRIDE_FREEZE_FLAGS`.
Camera: scripted `CREATE_CAM(14)` `l_U101`, `SET_CAM_POS` at `GET_OFFSET_FROM_CHAR(shooter, 1,4,1)`,
`POINT_CAM_AT_PED(shooter)` (`sub_34482`, :6310); victory cam attaches to the winner `l_U458` (:2340-2350).
So a partner's own character must: stand at the pose the host puppet is given each turn, play the same MINI_POOL
anims, be hidden while attached to `l_U317` during aim (or simply copy the puppet's visibility/pose), and view the host's scripted camera.

## 5. Minimal, safest mod hooks

Already available: `activity_seats::seat(id, ped, "pool_game", pad)`, `set_active(id)`, native hooks for the input
reads (bowling_sync.cpp:20-24 hashes), `script::find_thread(name)` + `+0x58` statics.

**(a) Start with a partner in seat 0 (preferred: partner as the opponent, host stays seat 1).**
Hook `START_NEW_SCRIPT_WITH_ARGS` where name == "pool_game". The block is pool_table's *own static* (`l_U19`...): do not
leave the puppet in it - `sub_1014` (:441, run when `g_U2219` is set) `DELETE_CHAR`s `l_U19._fU0[0..1]`. Either swap
word B+1 around the original call and restore it afterwards (safe iff the engine copies the args synchronously - verify
by reading `st[750..760]` / `st[722..732]` of the new thread), or leave the block alone and poke the live copy
`st[319]` (IV) / `st[286]` (TLAD/TBOGT) from the first tick that finds the thread (the thread has not run yet; the copy
happens at :120/:81 before any `WAIT`, so poking the *arg* slots `st[751]`/`st[723]` is the earlier, cleaner point).
Then mark the seat human: **`st[57]=1` (IV) / `st[59]=1` (TLAD, TBOGT)**, re-asserted every frame once
`l_U240 > 0 || l_U241 >= 1` (the script writes `l_U56[0]=0` once, in state 0 case 0, before any read of the flag).
The stranger `l_U31` stays standing unused; leave it (pool_table deletes it itself and never respawns one).
Also write pad[0] (`st[327]` IV / `st[294]`) = 1 so the partner's stick reads (`GET_POSITION_OF_ANALOGUE_STICKS(1,...)`,
`IS_BUTTON_PRESSED(1,13)`) are distinguishable from the host's pad-0 reads; answer pad 1 with the partner's sticks.
`_fU12[0]` stays `nil`=0: `IS_PLAYER_PLAYING(0)` is the host, so `SET_PLAYER_CONTROL(0,..)` just keeps the host frozen
(harmless). Keep the puppet within 10 m of the table: `sub_64950` (:7435, state 8 only) starts a 5 s abandon countdown
otherwise.

**(b) Seat-0 input.** Poll the turn word (`st[243]` / `st[203]`). While it equals the partner's seat, answer every
read from the table above issued by pool_game (IS_CONTROL_JUST_PRESSED/PRESSED pad 2 and 0, IS_BUTTON_PRESSED,
IS_MOUSE_BUTTON_PRESSED, GET_MOUSE_INPUT, GET_KEYBOARD_MOVE_INPUT, IS_USING_CONTROLLER) with the partner's input;
else the host's. Include state 7/8 (game over, rematch `(2,77)`, quit `(2,23)`): whoever's seat is `l_U243` at the end
answers them. The english helper reads pad 0 explicitly (`sub_42171`), so the partner's right stick/mouse must
answer pad-0 reads of that helper during their turn too. That is what `set_active(id)` does today for generic reads.

**(c) Queue / winner stays on.** Detect game over by `st[264]` (IV) / `st[224]` != -1 and `l_U240 == 7`. End the
script by feeding `(2,23)` once at state 7 (`sub_64741(0,0)` -> `sub_1032` restores control, re-groups friend models).
pool_table state 3 (IV :313-335) sees the thread gone and returns to state 2; the next START (host's context button or a
mod-issued one) is intercepted again and the next challenger takes seat 0. For a partner winner to stay on while the host
steps out, put the partner's puppet in word B+2 (seat 1, `st[320]`/`st[287]`) as well: seat 1 is forced human (`:311`)
and its ped is used only for tasks/camera/`sub_3014()==ped` speech checks. The host player then just stays frozen.
Both seats being puppets was not exercised by the script: verify `IS_PLAYER_PLAYING(_fU12[1])` (word B+4, set it to `nil`).

## 6. Risks / unknowns

- Args-copy timing (above): unverified whether the engine copies the arg block into the new thread's statics inside the
  native; verify with a probe before relying on either poke point.
- Seat-0 ped is a player-slot ped with the protagonist model, not a friend model: `sub_1912` (friend model test :2835)
  is false, so ambient-speech branches take the stranger path; harmless but the AI-accuracy table (`l_U230`, :212-275) is unused.
- Human seat 0 hits stats/achievement code written for the host: `INCREMENT_INT_STAT_NO_MESSAGE(316)` "wins" on a partner
  win (:2356), 317 on a loss, 318 clearances; `AWARD_ACHIEVEMENT(3)` only if the loser has a friend model (partners don't).
  Hook/ignore those from pool_game when the winner is the partner if you care about the host's stats.
- Friend activity result `sub_63624` (:7383) writes `g_U32898._fU16/_fU20` (win/lose against the friend) - a partner
  game inside a friend outing would count as the *host* winning/losing. Prefer the ambient path or suppress.
- The shooter is invisible and attached to a host-only object during the aim state; the partner's copy must not
  mirror that attachment, and the camera (`l_U101`) is the host's local camera: stream it (scene_cam) to the partner.
- Two puppet shooters share one table object `l_U279[15]` (cue ball) physics on the host: only the host simulates
  balls; partners only see them through whatever object sync exists. (pool uses the engine's pool physics settings.)
- State 0 `CLEAR_AREA_OF_CHARS` 10 m around the table also clears our puppets if they are not seated yet; seat them
  inside the 5-10 m and re-teleport after state 0 case 2.
- Debug keys `IS_KEYBOARD_KEY_JUST_PRESSED(31/33)` (:2653-2662) force a winner; a partner on the same keyboard path is not an issue
  (key reads are local) but beware the host pressing S/D... (scancodes 31/33).
- Unknown: exact native hash/arg order for START_NEW_SCRIPT_WITH_ARGS in the mod's native table.
