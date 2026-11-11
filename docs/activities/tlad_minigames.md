# TLAD / TBOGT ambient minigames: arm wrestling, air hockey, hi-lo cards

Source: decompiled scripts in [GTAIVMissionScripts](https://github.com/0x52617665/GTAIVMissionScripts) `TLAD/` (and `TBOGT/air_hockey.txt`).
Static slot = `l_U<n>` literal slot n; struct fields `_fU<off>` are byte offsets so slot = base + off/4.
Arrays: the decompiler's `l_U254[2]` has a count header in slot 254, elements start at 255 (verified by
spacing of `l_U291/308/325` = 17 = 1 + 2*8 and puck at 279 = 254 + 1 + 2*12). Treat slot numbers
derived this way as "inferred, verify at runtime".

Common facts for all three:
- None are started with `START_NEW_SCRIPT_WITH_ARGS`; none take arguments. All three are ambient: engine-launched by
  world-point brain / object allocation, never by the mission flow. The only explicit launch strings are the debug
  menu rows in `main.txt:2665-2667` (`arm_wrestling`, `hi_lo_cards`, `air_hockey_trigger`).
- The "player" is ALWAYS `GET_PLAYER_CHAR(CONVERT_INT_TO_PLAYERINDEX(GET_PLAYER_ID()))` (arm_wrestling `sub_5037`,
  air_hockey `sub_4304`, hi_lo `sub_865`). Seat 0 is hard-wired to the local (host) player. There is no seat table
  of "player indices"; the opponents are plain peds.
- Friend path = friends who are members of the player's group (`GET_PLAYER_GROUP`/`GET_GROUP_MEMBER` +
  `IS_CHAR_MODEL(friendModel)`), not a script argument. Non-friend path = nearby/spectator peds or a freshly
  created ped. Friend results are reported to the friend system through `g_U26739` (see "Friend hooks").
- Score = cash: `ADD_SCORE(player, n)`, `IS_SCORE_GREATER(player, n)` for the human; `GET/SET_CHAR_MONEY(ped)` for
  AI seats. There is no per-seat score static for the human besides that.
- `g_U10460` = "game fully initialised / not in mission-forbidden state" gate; `g_U12303` = mission active
  (blocks some sites); `g_U10486` = "player misbehaved, abort" flag (set on assault/vehicle intrusion, scripts
  watch it and quit); `g_U12304` = minigame-in-progress mirror of `SET_MINIGAME_IN_PROGRESS`.

---------------------------------------------------------------------------------------------------------

## 1. arm_wrestling (TLAD\arm_wrestling.txt, 4597 lines)

### Start
- `startup.txt:699`: `REGISTER_WORLD_POINT_SCRIPT_BRAIN("arm_wrestling", 100.0)`. Streamed script, one instance
  (`e1lostclubblips.txt:259` checks `GET_NUMBER_OF_INSTANCES_OF_STREAMED_SCRIPT("arm_wrestling") < 1`).
- Site chosen at boot by which AABB the brain point lies in (`arm_wrestling.txt:~72-86`, `l_U202`):
  0 = Lost MC clubhouse (-1713,349,31.7, interior key `E1_Lost_1stfloor`), 1 = bar at (-650,1205,5.9),
  2 = bar at (-237,1656,18.7). Site 0 waits for `g_U10460`; sites 1/2 abort if `g_U12303`.
- `e1armwrestleblips.txt`: only a blip toggler (hides blips 40/41 in `g_U11583/g_U12169` while a friend activity
  is selected, `g_U18635[i]._fU0/_fU20`). No gameplay, safe to ignore.
- Not in TBOGT.

### Seats / who is human
- Two seats at the table. Seat A = local player. Seat B = ped `l_U220` ("opponent").
- At the clubhouse the challenger is picked from up to 3 spectators `l_U229/l_U230/l_U231` (`sub_19061`, line ~3627):
  group friend with model of friend id 1/6/7 (`sub_19083`), else nearby gang ped (`sub_19302(0..2)`), each re-evaluated EVERY
  frame (nil'd if `sub_19556` fails), so writing into them is racy. Selection: `IS_CONTROL_JUST_PRESSED(2,115/116/117)`
  or pad buttons 16/17/14 when `IS_USING_CONTROLLER()` (lines 1220-1290) -> `l_U228` (chosen) -> state 2 copies
  `l_U220 = l_U228` (line ~1370). Sites 1/2 have no spectators: `l_U220` nil -> `sub_6715` (line 3104) creates a ped.
- Stakes `l_U203` (20 with spectators, else 100/200/300 by site). Entry fee `ADD_SCORE(player, -l_U203)`
  (`sub_22738`, line 4081); `l_U184 = 2` = "loading/start".
- Remaining spectators go to `l_U221[0..3]` (3 cheerers + `l_U221[3]` referee), then play cheer/ref anims.

### State statics
- `l_U184` main state: 0 idle/wait-for-prompt, 1 ambient (idle AI wrestle show, `l_U198` substate), 2 loading/setup,
  3 match, 4 cleanup (`sub_4986`). `l_U186` match substate (case 0 wait stand_wait, 1 idle_to_neutral+ref, 2 "ARM_H01"
  press-to-start, 3 countdown/setup, 4 wrestling loop, 5 pause menu, 10/11 round end). `l_U188` saved substate for pause.
- `l_U199` = player rounds won, `l_U200` = opponent rounds won; first to 2 (lines 2040-2100, `PRINT_NOW ARM_WIN/ARM_LOSE`).
  Win pays `ADD_SCORE(player, l_U203*2)` + stats 180/181 (line ~2110); loss does `SET_CHAR_MONEY(l_U228, +l_U203)`.
- Physics model: `l_U135` = seat A power (slot 135, float 0..`l_U139`=20), `l_U137` = seat B power, `l_U134` = tug position
  -1..+1 (>=1 A wins round, <=-1 B wins), `l_U132` = anim phase, `l_U139`=max power, `l_U141`=-15 decay,
  `l_U167` = AI strength (`sub_28164(l_U220)` +-0.1 by rounds won, line ~1621). `l_U142[2]`, `l_U145[2]`, `l_U148[2]`,
  `l_U113[2]` (toggle: last flick direction per seat), `l_U190[2]` (last flick timestamp).

### Input reads (the key finding)
Per seat, every frame in `l_U186 == 4` (lines ~1767-1880), using `sub_29735(pad)` (= "flick right") and `sub_30043(pad)` ("flick left"):
- `GET_POSITION_OF_ANALOGUE_STICKS(pad, lx, ly, rx, ry)`; uses RIGHT stick X (`iVar5`): >64 right, <-64 left.
- If both right-stick axes are 0: `GET_MOUSE_INPUT(x,y)`, threshold +-5.
- If left stick also 0 and not `IS_USING_CONTROLLER()`: `GET_KEYBOARD_MOVE_INPUT(x,y)`, threshold +-64 on X.
- Power: time between alternate flicks dt -> `l_U145 = 1/dt` capped at 20 -> acceleration `l_U148 = (l_U145 - l_U135)/dt`;
  `l_U135 += frameTime*l_U148` clamped 0..20. i.e. you must alternate L/R at high frequency. Rumble: `SHAKE_PAD(0,...)`.
- Seat A: pad 0 (lines 1767-1810). Seat B: `if (l_U116)` read pad 99 with the same helper (lines 1811-1865); else AI:
  `l_U148[1] = SIN(TIMERA/l_U194*360)` with `l_U194 = 0` => `l_U148[1] = 0`, so B power is the constant
  `l_U137 = l_U139 * l_U167` set at the start (line 1731, only `if (NOT l_U116)`).
- **`l_U116` and `l_U117` are read but never assigned anywhere in the script (default 0, set only by `main` init
  `l_U120..`, no write of 116/117).** They are vestigial dev flags: `l_U116 = 1` = "second human on pad 99" (and
  on `l_U116` the AI-strength init is skipped so both start at 0), `l_U117 = 1` = cheat (`l_U135 = l_U137`, line 1877).
  So the script DOES have a latent 2-human mode: just set static 116 = 1.
- Other human input: `IS_CONTROL_PRESSED(2,23)` (cancel/pause), `(2,137)/(2,77)` (start/confirm). Host only; partner seat doesn't need these.

### Ped treatment
- Setup (`l_U184` 2, lines ~1395-1520): `CREATE_OBJECT(-195381842 invisible, table pos) -> l_U207 (seat B anchor, heading l_U130)` and
  `l_U208` (seat A anchor, heading `l_U129`); both peds: `SET_CHAR_COORDINATES`, `SET_CHAR_HEADING`, `FREEZE_CHAR_POSITION`,
  `ATTACH_PED_TO_OBJECT(ped, anchor, 0, 0,0,0, heading, 360, 0, 0)`, `SET_CHAR_COLLISION`, `SET_ROOM_FOR_CHAR_BY_KEY`,
  `BLOCK_CHAR_GESTURE_ANIMS`, `SET_BLOCKING_OF_NON_TEMPORARY_EVENTS`. A clone ped `l_U226` is created for seat A (`l_U120` swaps visible
  one: `SET_CHAR_VISIBLE`). Player also `SET_PLAYER_CONTROL(0)`, `HIDE_CHAR_WEAPON_FOR_SCRIPTED_CUTSCENE`, `SHAKE_PLAYERPAD_WHEN_CONTROLLER_DISABLED`.
- Anims (dict `mini_arm`, `l_U212`): `stand_wait`, `idle_to_neutral`, then during play both peds `attack`/`defend` (A: `attack`
  mirrored `SET_CHAR_ANIM_CURRENT_TIME(.., l_U132)`, B: `defend`, speed 0 + manual time: lines 1960-2010), result anims
  `Celeb_A`/`Curse_Intro/Loop/Outro`. Opponent seat B is therefore fully script-driven: no ped input needed, it just needs to exist,
  be a valid ped (`IS_CHAR_INJURED` false) and survive `DELETE_CHAR`.
- Camera: `CREATE_CAM(14)`, `SET_CAM_POS/ROT/FOV` from `l_U246[0..3]/l_U259/l_U157` (4 preset angles, `l_U197`), `ACTIVATE_SCRIPTED_CAMS`. Local only.
- Abort checks every frame (lines ~790-890): `HAS_CHAR_BEEN_DAMAGED_BY_CHAR(any seat/spectator, player)`, car touching table,
  player within 0.3 of a seat ped, `g_U10486`. Any of these => spectators hostile + `g_U10486 = 1` + `sub_6062(1)`. With a puppet in
  seat B, puppet damage events from the real partner will abort the match: veto/ignore `HAS_CHAR_BEEN_DAMAGED_BY_CHAR` for puppets.

### Cleanup deletes the opponent
State 2 (`DELETE_CHAR(ref l_U220)` if `l_U228 != l_U220`, line ~1361), `sub_895` (`DELETE_CHAR/MARK_NO_LONGER_NEEDED l_U220`, line 2639-2643,
guarded by `sub_1295(ped)` which exempts mission/friend peds), `sub_6715` (line 3119). Hook `DELETE_CHAR` / `MARK_CHAR_AS_NO_LONGER_NEEDED`
for puppet handles and no-op them.

### Minimal mod approach (1v1 human vs human)
1. Prompt/stake: unchanged (host player interacts; the host-side puppet-selection UI = a partner whose puppet is near the table
   becomes the "chosen challenger"). To avoid fighting the per-frame spectator rewrite, do NOT inject into `l_U229-231`.
2. Swap at the start of match: in the frame where `l_U184` becomes 3 (first frame, `l_U186 == 0`), after the script built the AI ped in
   `l_U220`/anchor `l_U207`: teleport the puppet to `l_U237`/heading `l_U130`, `ATTACH_PED_TO_OBJECT(puppet, l_U207...)`,
   `FREEZE_CHAR_POSITION`, then write static 220 := puppet handle (save the old handle, hide/delete it). Restore static 220 at state
   change 3 -> 1/0 so cleanup does not delete the puppet. Alternative that needs no swap: leave the AI ped as a visual stand-in and
   instead play the puppet's own anims via the same `attack`/`defend` calls (the script drives anim time through handle `l_U220`, so swapping the handle is the real fix).
3. Input: write static 116 := 1 for the match (seat B reads pad 99). Hook `GET_POSITION_OF_ANALOGUE_STICKS`: when `pad == 99` set
   context flag SEAT_B, return the partner's right-stick-X (other outputs 0); clear the flag when a call with pad 0 arrives. While SEAT_B is
   set, also answer `GET_MOUSE_INPUT` / `GET_KEYBOARD_MOVE_INPUT` with the partner's mouse X / A-D, and make `IS_USING_CONTROLLER` return partner's
   value; otherwise seat B would also react to the HOST's mouse/keyboard (pad 99 gives zero sticks so the fallbacks fire) (lines 4289-4340).
   Call order inside the helper is fixed (GPOAS -> [GET_MOUSE_INPUT] -> [GET_KEYBOARD_MOVE_INPUT]) so the flag approach is reliable.
4. Net input: send discrete flicks `{dir, partnerLocalTimestamp}` (or the raw X sample at >=60 Hz); host plays them back through the hook with
   a small jitter buffer. Power depends on inter-flick time measured with `GET_GAME_TIMER`, so jitter directly changes balance;
   timestamp-based replay beats per-frame sampling. Rumble/`SHAKE_PAD` is host-only; mirror to partner by sending `l_U135/l_U137/l_U134`.
5. Results: win/loss scoring is `ADD_SCORE(player)` on the host only. Give the partner their winnings explicitly from `l_U199/l_U200`
   transitions (state 11 -> 10); host pays/gets stake via native hooks or the coop wallet.
6. Queue: after a round ends (`l_U186 == 10/1`) and when match over (`l_U199>=2 || l_U200>=2`, then `l_U184`=4 cleanup), the winner stays
   in a seat; swap the loser out by performing step 2 for the next challenger. Loser puppet goes back to normal puppet driving.

Real-time: the loop is frame-rate coupled (`GET_FRAME_TIME` integration) but low-bandwidth (one scalar per seat). Easy.
Unknown: `sub_19556/sub_19302` details, `sub_28164` (AI strength per ped), `l_U120` (clone swap, never assigned in decomp) so partner
visibility of the swapped ped should be tested.

---------------------------------------------------------------------------------------------------------

## 2. air_hockey (TLAD\air_hockey.txt, 3838 lines; TBOGT\air_hockey.txt, 3815 lines)

### Start
- Object-bound ambient script: `startup.txt:687` `ALLOCATE_SCRIPT_TO_OBJECT("air_hockey", 1261730469, 100, 50.0, -1)` (TLAD; table model
  hash). TBOGT: `startup.txt:832` hash 1948842064. Script `TERMINATE`s if the object (`l_U594`) doesn't exist, caches `l_U342 = l_U594`
  (table) and room key `l_U343`. One instance per table prop; all tables share the same logic.
- `air_hockey_trigger.txt` (61 lines, identical in TBOGT) is a debug warp, not gameplay: fade out, `GET_INTERIOR_AT_COORDS`
  (-584.75,78.02,5.22), `LOAD_SCENE_FOR_ROOM_BY_KEY(E1_BOWL_ROOM)`, teleport + freeze 3 s, fade in. Launched from the debug menu `main.txt:2667`.
  Ignore for co-op.
- `clayairhockey.txt` / `jimairhockey.txt` / `terryairhockey.txt` (22201 lines each, identical template, differ by friend): friend-activity
  driver scripts (labels `E1F7FAIRH` Terry, `E1F8FAIRH` Clay, `main.txt:2653/2662`): arrive at the venue, speech, wait for result. They contain NO
  hockey logic (no puck/paddle statics, no `MINI_AIR`); they only drive travel/arrival and read the outcome via `g_U26739` (see friend hooks).
  The game itself is always the ambient `air_hockey` script with the friend as a group member (the opponent candidate list `l_U541`).
- TBOGT: same script, statics shifted by -113 (TLAD 245->132 state, 254->141 paddles, 279->166 puck, 486->373, 517->404 scores,
  523/524->410/411 opponents, 541->428 friends). Only two friend candidates (`l_U428[0..1]`, label help AH_START_07/08) vs three in TLAD.
  Same natives and same table-local physics.

### Seats / who is human
- Exactly two seats. Seat 0 (paddle `l_U254[0]`, the near side, +y) = local player `sub_4304()`. Seat 1 (paddle `l_U254[1]`, far side) = ped `l_U524`
  = opponent. `l_U523` = a spawned ambient "guard" ped standing by the table (`sub_3876`: `CREATE_CHAR(25, rnd model)` next to the
  table, `TASK_STAND_GUARD`), the default AI opponent when no friends are present.
- Prompt (`sub_4481`, line 646-722, state `l_U245 == 1`): player within 5 m of table, `sub_7205(2,0)` (help-text gate), `IS_CONTROL_JUST_PRESSED(0,23)`.
  `l_U545` = number of friends in the player's group near the table (`sub_5159`, line 892: `sub_5175(1/6/7)` = friend id model lookup,
  else `sub_5724(0..2)` fallback, each filtered by `sub_5935`). `l_U545 == 0` -> `l_U524 = l_U523` directly (state 3). `l_U545 == 1` -> that friend
  directly (state 3). `l_U545 >= 2` -> state 2 chooser (`sub_8624`, lines 1340-1420) using controls 115/116/117 / pad buttons 16/17/14.
- Wanted level > 0, `g_U10959` bit 4, `g_U12303` (mission) block entry (`sub_4490`).

### State statics
- `l_U245`: 0 load models/objects, 1 idle/prompt, 2 choose opponent, 3 setup (`sub_9857`: fade out, clear area, cameras, seat peds, text `AIRHOCK`, anims
  `MINI_AIR`, audio bank `EP1_SFX\AIR_HOCKEY`), 4 play, 5 goal (`sub_25265`), 6 after-goal (>=7 wins), 7 rematch ("AH_REMATCH"), 8 quit menu,
  9 cleanup (`sub_30939`), 10 table gone. Lines 130-311.
- `l_U517[0]` (slot 518) = seat 0 score, `l_U517[1]` (slot 519) = seat 1 score, win at 7 (lines 236/243); `l_U520` = last scorer; `l_U470` games played;
  `l_U471` game-start timer; `l_U546` early-game flag (first 10 s); `l_U592` debounce.
- Paddle struct `l_U254[i]` (12 slots each, inferred slots): +0 object handle (255 / 267), +4 pos vec3 (256-258 / 268-270; x then y),
  +16 rot vec3, +28 radius (262 / 274), +36 velocity vec3 (264-266 / 276-278). Puck `l_U279`: handle 279, pos 280-282, radius 286, vel 288-290.
  Table half extents `l_U455` (x 0.53, y 0.992), goal half-width `l_U458` 0.204.
- Result reporting to the friend system: `sub_25106(0 win / 1 loss / 3 quit)` -> `g_U26739._fU16/_fU20` (line 3040); `sub_29322(2)` on a win.
  Stat 136 (games), 137 (wins) via `INCREMENT_INT_STAT`.

### Input reads
- `sub_14382` (line 2066), seat 0 only: `GET_POSITION_OF_ANALOGUE_STICKS(0, lx, ly, rx, ry)`. Left stick moves the paddle:
  `sub_14423(lx, ly, l_U237=1650)` sets `l_U254[0]._fU36.x = -lx/1650`, `.y = ly/1650`, gated so the paddle can't leave the bounds (`l_U455`).
  Right stick flick: if `rx^2+ry^2 > 12100` within 150 ms of rest (`l_U501`) -> "slam": `l_U497 = 1`, velocity from `(rx,ry)/l_U504=560` for `l_U503=30` ms, SFX
  `AIR_HOCKEY_SLAM`. If all four axes are 0: `GET_MOUSE_INPUT` scaled `l_U238 + (l_U239-l_U238)*GET_MOUSE_SENSITIVITY()`.
- No second-pad code at all (no pad argument other than literal 0). Seat 1 is a hard AI (`sub_15445`, line 2164; writes `l_U254[1]._fU36` from puck
  position with tuning `l_U472-476`, `l_U477-484`).
- AI pause: `if (l_U486 > 0) l_U486 -= dt*1000 else sub_15445()` (line 2027-2036). **Setting static 486 to a large value disables the AI entirely** without
  touching the rest of the sim; `l_U486` is only ever decremented otherwise.
- Other human controls: `(2,0)` toggles camera `l_U550` (6 presets `l_U552`), `(2,23)` quit menu, `(2,137)/(2,77)` rematch/continue. Host only.
- Sim step: `sub_14331` (line 1945) per frame -> `sub_14373` (paddle update, includes both seats) -> `sub_17154` (collision: paddles, puck, walls; dt scaled
  `GET_FRAME_TIME*30`) -> goal check line 1982-2002. All in the script thread on the host; the table objects are script `CREATE_OBJECT`s (`l_U254[i]` handles etc.).

### Ped treatment
- Seat 0 (`sub_10554`, line 1584): `CLEAR_CHAR_TASKS_IMMEDIATELY`, invisible anchor object `l_U538[0]` (`CREATE_OBJECT(l_U231 model)` at table offset (0,1.3,0.19)),
  `ATTACH_PED_TO_OBJECT(player, anchor, 0,0,0,0,0,0,1,0)`, `SET_PLAYER_CONTROL(0)`, `TASK_LOOK_AT_OBJECT(player, puck object, 9999999)`.
  While playing: `SET_PED_ALPHA(player, ...)` fades with paddle y when camera 1 is the first-person-over-table cam (`sub_13114`, line 1817).
- Seat 1 (`sub_10865`, line 1633): if `l_U524 == l_U523` and injured/missing -> respawn it; `CLEAR_CHAR_TASKS_IMMEDIATELY`, `SET_BLOCKING_OF_NON_TEMPORARY_EVENTS`,
  `SET_CHAR_COORDINATES` (table-relative (0,-1.3,0.15)-ish), `FREEZE_CHAR_POSITION`, anchor `l_U538[1]`, `ATTACH_PED_TO_OBJECT`, `TASK_LOOK_AT_OBJECT(puck)`.
  If `l_U524 != l_U523`, `sub_9857` calls `sub_1242` and DELETES the ambient guard.
- Anims dict `MINI_AIR`: `strike` (line 1898), `celeb`, `minor_celeb*`, `minor_curse*`, `cheer_on_fight_A..D` for spectators (friends not playing: `sub_11337`
  places them frozen at two side spots `l_U505`).
- Spectators: `l_U541[i] != l_U524` friends are `CLEAR_CHAR_TASKS_IMMEDIATELY`, `SET_CHAR_COORDINATES_NO_OFFSET`, `FREEZE_CHAR_POSITION`. Other partners' puppets could be
  used as spectators here (up to 2 positions).
- Cleanup (`sub_30939`, lines 3703-3796): `FREEZE_CHAR_POSITION(l_U524,0)`, a `TASK_PERFORM_SEQUENCE` (look at player, stand still, `TASK_FOLLOW_NAV_MESH_TO_COORD`,
  `TASK_STAND_GUARD`) on `l_U524`; guard deleted if the script terminates (`sub_1242` in `sub_1014`). Guard fleeing/`MARK_CHAR_AS_NO_LONGER_NEEDED` when wanted (`sub_4256`).
  Veto `DELETE_CHAR`/`MARK_CHAR_AS_NO_LONGER_NEEDED`/`TASK_*` on puppets you put in `l_U524` and re-impose puppet control afterward.

### Minimal mod approach (1v1 human vs human)
1. Opponent seat: when the host presses the prompt and the state becomes 3 (`l_U245 == 3`, set inside the same frame as the button press), in the next frame BEFORE
   `sub_9857` runs (hook the script's `WAIT(0)` / a per-frame native on the table script, check static 245 == 3), write static 524 := partner puppet handle.
   `sub_9857`/`sub_10865` will seat it exactly like a friend (and delete the guard). Same for TBOGT (static 411 and 132). `l_U545` does not matter after the prompt.
2. Disable AI: each frame while `l_U245` in {4,5,6} write static 486 := 1e9 (TBOGT 373).
3. Drive seat 1: each frame after the script's step (hook at the next `WAIT`), write partner movement into paddle 1 velocity: static 276 (vx) and 277 (vy)
   using the same formula as `sub_14423` mirrored for the far seat (`vx = +lx/1650`, `vy = -ly/1650`, respecting the wall clamp lines 2040-2063; slam same as seat 0 with
   `l_U504=560`, `l_U503=30` ms). Hook `GET_POSITION_OF_ANALOGUE_STICKS(0, ...)` calls to separate seat 0 reads (the host's) from seat 1 injection; there is no pad-per-seat in the
   script so seat 1 must be driven by static writes, not by natives. Alternatively set paddle 1 pos directly (slots 268/269) at partner's tracked position and zero velocity, but then
   puck-vs-paddle impulses lose momentum; velocity writes keep the physics honest.
4. Partner view: the host's world holds the table objects and the puck; the partner needs (a) paddle/puck pos stream: statics 256-258, 268-270, 280-282 at 30-60 Hz plus score statics 518/519,
   (b) camera on the partner machine rotated 180 degrees relative to the host's for seat 1 (host cams `l_U552` are fixed per script). Input must be sent in table-local axes.
5. Scores/winnings: win at 7 (`l_U517`). Hooks on `sub_25106` equivalents (`g_U26739` writes) are friend-only; treat partner winnings as coop wallet entries. Stats 136/137 count only for the host.
6. Queue: on state 7 ("AH_REMATCH") the script waits for host input (`(2,137)/(2,77)` rematch, `(2,23)` leave). Drive the next queued partner by writing 524 again + `l_U245 = 3` when the loser
   leaves; the winner keeps their seat. If seat 0 is the partner and seat 1 the host, the script structurally cannot do it (seat 0 = local player): the host must always be a seat unless statics 255.. (paddle 0)
   are also written from partner input and `GET_POSITION_OF_ANALOGUE_STICKS(0)` is blanked. Recommend host-always-seated first; "4 players" = host + 1 active partner + 2 spectators; others queue.

Real-time: both paddles integrate every frame (`dt*30`), puck collisions are 2D; latency of the partner's input directly feels like paddle lag. The partner paddle must be locally
predicted on the partner machine (it knows its own commanded velocity) and reconciled to host statics. Keep host as sole simulator.

Unknown/risks: slot numbers inferred from array headers; unknown whether `l_U546` (rules help, `AH_RULES_1`) pauses sim; `sub_21887`/`sub_30489` (spectator reaction/rematch) call `TASK_*` on `l_U524` and `l_U541[]`;
the 400 ms gate at line 1952 (`l_U467` init-only) just delays first physics tick; frame-time scaling means host stutter alters physics (not network-sensitive but host-FPS-sensitive).

---------------------------------------------------------------------------------------------------------

## 3. hi_lo_cards (TLAD\hi_lo_cards.txt, 5197 lines; Lost clubhouse only)

### Start
- `startup.txt:700` `REGISTER_WORLD_POINT_SCRIPT_BRAIN("hi_lo_cards", 100.0)`, single site: the card table in the Lost clubhouse (player seat (-1725.2,334.9,30.8), seats `l_U268[0..3]` at ~(-1724.5,335.8,31.1),
  (-1726.0,334.8,31.1), (-1725.6,335.6,31.1), (-1724.0,334.9,31.1), headings `l_U133[]`; interior check `sub_5047` = `GET_INTERIOR_AT_COORDS(-1731.77,331.04,32.24)`). Waits `g_U10460`. `e1lostclubblips.txt:147,328` manage its blip.
  Debug launch row `main.txt:2666` `HILOCARDS`. No arguments. Not in TBOGT.
- `l_U128` = "ambient AI table active" (player within 10 m (20 if `billy1` mission running) of seat 0 position); while on, up to 4 AI peds `l_U253[0..3]` (created at 2747-2751, models 1737188996 / -2130437771) play among themselves
  (state machine `l_U219`, turn marker `l_U215`). `l_U157` = top state: 0 idle/load assets, 1 ambient + prompt, 3 player joining, 4 player seated (`l_U214` interactive state machine).

### Seats / who is human
- 4 seats. Seat letters/anims: A = `l_U253[3]` (`player_a_*`), B = player (`l_U261 = sub_865()`, `player_b_*`), C = `l_U253[1]`, D = `l_U253[2]`. Turn index `l_U215` 0..3 where 0 = player seat (`l_U253[0]` is the demo-mode
  occupant of the player's chair and is replaced when the player sits (to verify)), 1,2,3 = AI. Order via `sub_37281` (line 5077), skipping eliminated seats.
- AI opponents (`l_U258/259/260`): group friends of id 1/6/7 (`sub_21470`) else nearby (`sub_22023(0..2)`), filtered by `sub_22181`; they replace `l_U253[1..3]` (lines ~895-960). Without friends the ambient peds stay.
- Player seat eliminated flag `l_U117`; AI eliminated flags `l_U108[1..3]` (lines 5054-5070 count survivors via `sub_32261`). Win when one survivor (`l_U217` = winner index; 0 = player). Pot: ante `ADD_SCORE(player,-10)`,
  `SET_CHAR_MONEY(ai, -10)`; winner `+40` (`ADD_SCORE(player,40)` line 1998 or `SET_CHAR_MONEY(l_U253[l_U217], +40)` line 2032); stats 178/179.

### Turn/state statics
- Deck `l_U160[]` with index `l_U159` (card 0..51, rank = `sub_17205(card)` 1..13). Previous rank `l_U216`; guess `l_U116` (1 = higher, 0 = lower; both humans and AI set it); anim/reaction code `l_U221`;
  `l_U220` camera/seat-focus id; `l_U214` interactive state (0 deal/ante, 1 wait anims, 2 pick first, 3 AI turn, 5 player turn, 6/7 reveal, 8 next seat, 15 pay/celebrate, 16 end, 18 pause menu).
  `l_U219` = same role for the ambient demo. Pause: `sub_29583` (`IS_CONTROL_PRESSED(2,23)` -> `l_U214 = 18`).

### Input reads
- Human guess only on seat 0's turn (`l_U214 == 5`, lines 1552-1620): HIGHER = `IS_CONTROL_PRESSED(2,93)` or `IS_CONTROL_JUST_PRESSED(2,130)` (keyboard) / `IS_CONTROL_PRESSED(2,79)` (controller);
  LOWER = `(2,94)`/`(2,131)` / `(2,78)`. Sets `l_U116`, `l_U216`, `l_U214 = 7`. Also `IS_CONTROL_PRESSED(2,137)/(2,77)` skip/confirm, `(2,23)` pause. All pad 0 / host.
- AI seats decide in `l_U214 == 3` (lines ~1220-1480) after `TIMERA() > 2000`: rank-based deterministic/random guess writing `l_U116`, `l_U221`, `l_U216` then `l_U214 = 4`. No natives involved: pure script logic.
- No two-human support, no pad argument.

### Ped treatment
- Player: `SET_PLAYER_CONTROL(0)`, `SET_CHAR_COORDINATES` to `l_U265`, `FREEZE_CHAR_POSITION`, `SET_ROOM_FOR_CHAR_BY_KEY`, `HIDE_CHAR_WEAPON_FOR_SCRIPTED_CUTSCENE`, `SET_CHAR_COLLISION(0)`,
  `ATTACH_PED_TO_OBJECT(player, l_U229 anchor, ...)` (lines 996-1008). AI seat peds attached to anchors `l_U224[i]` (line 2259).
- Anims (`mini_cards`): `player_{a,b,c,d}_place_bet`, `_decide`, `_celeb`, etc., via `TASK_PLAY_ANIM_NON_INTERRUPTABLE`; card props attached with `ATTACH_OBJECT_TO_PED(card, ped, bone 1219 / 13744, ...)` (lines 3305-3470).
  Camera `CREATE_CAM(14)` + `SET_CAM_POS` per phase (lines 1065, 2191-2236). Spectators speak (`CARDS_*` ambient speech).

### Minimal mod approach
- Seat 0 is the host. Each other seat 1..3 can be a puppet: at the moment `l_U157 == 4 && l_U214 == 1` (after setup lines ~895-960 filled `l_U253[1..3]`), swap `l_U253[k]` handle for the partner's puppet
  (teleport to `l_U268[k]`, `ATTACH_PED_TO_OBJECT(puppet, l_U224[k]...)`, freeze) and hide the old ped. Veto `DELETE_CHAR`/`MARK_NO_LONGER_NEEDED`/`CLEAR_CHAR_TASKS*` on it (the script re-plays anims through the handle, which is what we want).
- Replace the AI decision at `l_U214 == 3 && l_U215 == k`: either block the AI's `TIMERA()` gate (hook `TIMERA` to return 0 for k) until the partner's choice arrives, then write `l_U116 := choice`, `l_U216 := rank(l_U160[l_U159])`,
  `l_U221 := 6/7 (or 2..9 per rank table lines 1262-1300)`, `l_U214 := 4`; or on partner choice set those statics at once and skip. Which approach is cleaner depends on the unreadable `l_U221` side effects: test.
- Money: partner "score" must be mapped to the partner's wallet (`GET/SET_CHAR_MONEY` on a puppet are local to the host's world); ante/pot is `-10`/`+40`.
- Turn order is purely host statics, so network sync is cheap (a few ints: 215, 214, 116, 159, 160[], 108[]). The partner client displays cards via synced statics + its own UI.
- Queue: eliminated seats (`l_U108[k]=1`) simply aren't skipped by `sub_37281`; for a queue, swap a waiting partner into an eliminated seat at the next round start (`l_U214 == 0`, `l_U108[k] := 0`).
- Turn-based, no real-time sync issues. Timeouts: script has none for the player (only pause), so add a partner decision timeout in the mod.

Unknown: exact meaning of `l_U253[0]` once the player sits; precise `l_U221`/`l_U220` code semantics; `sub_21470/sub_22023` friend lookup; whether `ADD_SCORE` ante is skipped for friend-mission state.

---------------------------------------------------------------------------------------------------------

## Cross-cutting recommendations
1. A per-script "seat adapter" with: (a) handle swap in a hook point keyed on a state static, (b) veto list for `DELETE_CHAR`, `MARK_CHAR_AS_NO_LONGER_NEEDED`, `TASK_*`, `CLEAR_CHAR_TASKS*`, `HAS_CHAR_BEEN_DAMAGED_BY_CHAR`, `IS_CHAR_INJURED` normalisation for puppet handles.
2. Input: arm wrestling = native hook with a pad-99 context flag; air hockey = static writes (no pad-per-seat); hi-lo = static writes at the decision frame. All are host-authoritative.
3. Wallet: all scripts mutate only the host's `ADD_SCORE`/char money. Co-op needs a mod wallet and per-game payout hooks on the win states: arm wrestling `l_U199>=2/l_U200>=2`, air hockey `l_U517[i]>=7`, hi-lo `l_U217`.
4. Friend hooks: if no friends are in the group the scripts use ambient AI; a puppet that is not in the player's group never enters the friend-candidate scans. Do not try to add puppets to `GET_PLAYER_GROUP` to make them selectable (models won't match, and friend-activity result reporting `g_U26739` fires).
5. Detect/skip site entry with the existing gates: `g_U12303` (mission active) blocks sites 1/2 of arm wrestling and air hockey; wanted level blocks air hockey.
