# Golf (TBoGT) - research for per-human seating

Source: `research/GTAIVMissionScripts/TBOGT/{golf,golf_launcher}.txt` (line numbers below are those files; statics
`l_U<slot>`, array size word at the slot, element i at slot+1+i, struct field `_fU<off>` = slot + off/4).
There is no separate friend version of golf: the friend hangout (`dominicanfriend.txt` :5703 "GOLF", :8710
`statAct_golf`) only runs the outing and reads results from `g_U28807`; the game is always `golf` started by
`golf_launcher`. `main.txt` :2606 registers the activity (`sub_28321(10, 2, 1, 15, "GOLF", ..., "golf")`).

## What the game is
Not 18-hole golf. It is a 2-sided **target-shooting range**: each side has 10 shots (`l_U2882` = 10, :109),
the ball (`l_U3074`, model 819995114) is hit toward a randomly chosen target flag (`l_U3103`, model -1153917558,
positions `l_U2898[21]`), score per shot = 10 down to 0 by distance to the flag (:2379-2427, 2605-2647), +5
(`l_U2896`) for a hole-in-flag touch (:1930-1936). Sides alternate P, O, P, O. After 20 shots the result
screen shows win/lose/draw (:3217-3227) and a rematch prompt (`GOLF_REMATCH`, :3214).

## Start (ambient vs friend)
`golf_launcher` (569 lines) is a static launcher thread in the world:
- Trigger `sub_244` (:129-175): `g_U15811[30]` unlocked, not wanted, not ragdoll, game timer past `g_U22959`,
  and `LOCATE_CHAR_ON_FOOT_3D(player, -571.03, -13.52, 6.31, 2.5)`. It only looks at the **host player's ped**
  (`sub_680` = `GET_PLAYER_CHAR(CONVERT_INT_TO_PLAYERINDEX(GET_PLAYER_ID()))`, ~:300).
- Help prompt `GOLFPRESSLB`; `IS_CONTROL_JUST_PRESSED(2, 23)` (LB) :46-61. With no buddy (`l_U24 == 0`, buddies counted
  by `sub_895` :273 from `g_U28807._fU0[i] != 34`, `g_U28807._fU12[i]` = the friend peds, i < 2) -> `sub_1235(2)` (:318, called :61)
  = ambient; with buddies `GOLFBUDDIES` is shown and `IS_BUTTON_JUST_PRESSED(0,16)`/`(2,115)` or `(0,17)`/`(2,116)`
  picks which buddy plays (`sub_1235(1)` :75 / `(0)` :80).
- `sub_1474` (:357-380): `SET_PLAYER_CONTROL(player, 0)`, `START_NEW_SCRIPT_WITH_ARGS("golf", &l_U25, 3, 8192)`
  (:377). Args = 3 words `l_U25 {_fU0 = opponent ped, _fU4 = bystander ped, _fU8 = mode}`, which arrive in
  golf's statics at `l_U3143` (golf :192-194: `l_U3093 = l_U3143._fU0; l_U3094 = l_U3143._fU4; l_U3082 = l_U3143._fU8`).
  - Ambient: `{nil, nil, 2}`. golf then creates a random stranger as opponent (`sub_5440` :767, `CREATE_CHAR` :790 type 25,
    three models -671350069 / -719053654 / 1639961459) and deletes it on exit (`l_U3082 == 2` -> `DELETE_CHAR`, :310-312).
  - Friend: mode 0/1; the buddy matching `l_U28[0]` 3/7 becomes the opponent (`_fU0`) and the other buddy the
    bystander (`_fU4`): it is taken out of the group, warped to the bench `l_U2858` and `TASK_SIT_DOWN` (:648),
    heckles/cheers (`TASK_PLAY_ANIM_SECONDARY_UPPER_BODY`, :3063-3075). Friend mode keeps the peds alive at the end
    and puts them back in the group (`SET_GROUP_MEMBER(l_U3113, ...)`, :314-334).

## Players and per-player state: TWO sides, one human-input path
The script supports exactly **2 competitors** (side 0 "P" = the local player, side 1 "O" = the opponent ped) plus one
bystander. Nothing is indexed by player beyond the 2-element arrays.

| What | Static (golf) | Lines |
|---|---|---|
| Opponent ped (side O) | `l_U3093` | :192 set, used :262-334, 592-654, 734-737, 1351, 2306-2335, 2444-2449, 2675, 2717-2801 |
| Bystander ped | `l_U3094` | :193, 326-334, 635-654, 2817-2820, 3063-3075 |
| Mode (0/1 friend, 2 ambient) | `l_U3082` | :194-197, 310 |
| Whose turn (0 = P, 1 = O) | `l_U2890` | flips :2765 (P->O) and :2788 (O->P), reset :2435 |
| Shots left [P, O] | `l_U2883` size word, **2884 (P), 2885 (O)**, init `l_U2882` = 10 | :699-700, decrement :2376, :2751 |
| Total score [P, O] | `l_U2886` size word, **2887 (P), 2888 (O)** | :701-702, += :2427, :2647-2657 |
| Last shot points / bonus | `l_U2889` / `l_U2896` | :2385-2425, :2752 |
| Target index (flag) | `l_U2892` (positions `l_U2898[..]._fU0`) | :704-705, `sub_8088` :912 |
| Round counter (restarts) | `l_U2881` | :763 (`l_U2881++`) |
| Main state (0 init, 1 setup, 2 intro, 3 aim/play, 4 ball flight/result, 6-7 end/rematch, 8 exit) | `l_U3136` | :181, 566, 747-751, switch :198-251 |
| Per-shot phase (0 aim, 1 flying, 2 scoring, 3 celeb, 5 hand over) | `l_U3138` | :2552-2900 (`sub_23611`) |
| Swing state (0 idle/aim, 1 back-swing, 3 abort, 4 swing/follow) | `l_U3140` | `sub_15239` :1664-1960 |
| Aim cursor, power, spin | `l_U2983` (vec3), `l_U3054` power, `l_U3056` hook | `sub_15239` :1664-1960 |
| Camera mode / cams | `l_U3139`; cams `l_U3095-3099`, `l_U3130`, `l_U3131[]` | `sub_8254` :926 |
| Club objects | `l_U3100` (P, bone 1219), `l_U3101` (O) | created :660-670; ball `l_U3074`, tee-attach dummy `l_U3102` |

The scoreboard is not a struct: it is the two arrays above, drawn by `DISPLAY_TEXT_WITH_NUMBER` `GOLF_P_SCORE` /
`GOLF_O_SCORE` / `GOLF_STATS_1` (:2519-2525). The P-vs-O comparison at game end (`l_U2886[0]` vs `[1]`, :2679-2692,
3217-3227) decides win/lose speech, stat and the friend result.

## Input natives (all read through the side-P path only)
The P path runs when `l_U2890 == 0` (`sub_13171` :1293-1352: `sub_15239()` aim/swing, `sub_10491(sub_1732())` anim);
the O path (`l_U2890 == 1`) runs `sub_21073` (AI) and reads no input except the skip. All stick reads are **pad 0**,
button presses **pad 2** except the swing release (pad 0). `l_U2893` is a constant 1 (:114), so the button swing mode
is used and the right-stick flick branches (`l_U2893 == 0`, `iVar7`) are dead code.

| Native | Use | Lines |
|---|---|---|
| `GET_POSITION_OF_ANALOGUE_STICKS(0, &LX,&LY,&RX,&RY)` | aim cursor moves with LX/LY; RX/20 = hook (`l_U3056`) | :1678, 1700, 1790 |
| `GET_MOUSE_INPUT(&x,&y)` | overwrites LX/LY when not on controller | :1681 |
| `IS_USING_CONTROLLER` | picks mouse vs stick and keyboard vs pad reads | :1250, 1441, 1592, 1679 |
| `IS_CONTROL_JUST_PRESSED(2,137)` or `(2,1)` | start back-swing (`l_U3140` 0 -> 1) | :1746 |
| `IS_CONTROL_JUST_PRESSED(0,137)` or `(0,1)` | strike (back-swing release, `l_U3140` -> 4) | :1758 |
| `IS_CONTROL_JUST_PRESSED(2,23)` | help toggle / quit confirm (state 8) | :1309, 1324, 2350-2361, 2884-2895, 3230, 3252 |
| `IS_CONTROL_JUST_PRESSED(2,137)`/`(2,98)` (pad only if not controller), `(2,77)`, `(0,77)` | `sub_11945` :1248: skip the opponent's swing; also confirm rematch (2,137)/(2,77) :3236, 3258 | :1248-1256 (def), 2344, 2864 (callers) |
| `GET_CONTROL_VALUE(0,4)` (controller) / `(0,87)` (keyboard), `GET_KEYBOARD_MOVE_INPUT` | camera toggle / pan in `sub_13180` | :1439-1444, 1591-1594 |
| `SHAKE_PAD(0, 250, power)` | rumble on swing (output only) | :1695 |

## What it does to the shooter's ped
`sub_1732()` (:359) is `GET_PLAYER_CHAR(CONVERT_INT_TO_PLAYERINDEX(GET_PLAYER_ID()))` and is the P ped everywhere
(`sub_1786()` the player id). Setup (`sub_5014`, state 1, :570-765): fade out, `SET_PLAYER_CONTROL(...,0)`, load
models and anims `MINI_GOLF`/`MINI_AIR`, `GIVE_WEAPON_TO_CHAR(P, 0)`, `SET_BLOCKING_OF_NON_TEMPORARY_EVENTS`,
`BLOCK_CHAR_GESTURE_ANIMS`/`AMBIENT_ANIMS`/`PED_WEAPON_SWITCHING`, creates the club and attaches it to the ped
(`ATTACH_OBJECT_TO_PED(l_U3100, P, 1219, l_U2861={0.03,0.017,0}, l_U2864={4.75,3,1.25}, 0)`, :666; values :96-97), moves P to the
tee `l_U2831` (-641.57,-14,11.38) and `ATTACH_PED_TO_OBJECT(P, l_U3102 [invisible tee object], 0, 0,0,0, 1.5708,0,0,0)`
(:739), plays `TASK_PLAY_ANIM_NON_INTERRUPTABLE(P, "Idle", "MINI_GOLF", ...)`.
- Each shot's anims run in `sub_10491(ped)` (:1148-1250) keyed on `l_U3140`: Idle / Swing_Intro (speed 0.6-0.8, frozen
  at the back-swing time `l_U3076`) / Swing_Action then Swing_Outro as a sequence, `SET_CHAR_ANIM_CURRENT_TIME/SPEED`.
  **It takes the ped as a parameter and is already called with the opponent** (`sub_10491(l_U3093)`, :1351), so the script
  animates a non-player ped as a golfer today. Results: `Celeb_A/C`, `Curse_A/C`, `MINI_AIR celeb` (:3082-3100, 2903ff).
- Turn hand-over (`sub_23611` case 5, :2742-2830): P->O: O ped `ATTACH_PED_TO_OBJECT` to `l_U3102` (tee), P ped
  `DETACH_PED`, `CLEAR_CHAR_TASKS_IMMEDIATELY`, `SET_CHAR_COORDINATES` to the waiting spot `l_U2855` (-637.86,-12.02,11.40),
  heading `l_U2879`; O->P is the mirror (and the same on the AI-skip path `sub_21570` :2433-2462, which also re-grants
  `SET_PLAYER_CONTROL`). The shooter is always the one attached to the tee.
- Cameras: all scripted (`CREATE_CAM` 14, `ACTIVATE_SCRIPTED_CAMS`), `SET_CAM_BEHIND_PED(P)` at exit (:259).
- Exit (`sub_1709` :246-345): re-enable control/gestures, `DETACH_PED`, `SET_CHAR_COORDINATES(P, l_U2834 = -570.75,
  -13.38, 3.81)`, delete objects/cams/blip, O deleted if mode 2 else teleported to l_U2834+(0,2,0) and returned to the
  group, bystander to l_U2834-(0,2,0).
- Side effects on the host: `INCREMENT_INT_STAT(212)` per flag hit by side P (:1930), `AWARD_ACHIEVEMENT(58)` at
  stat 212 > 3 (:1933, 2815), `INCREMENT_INT_STAT_NO_MESSAGE(245)` (:707), `SET_MINIGAME_IN_PROGRESS`, `g_U28807._fU44 = 34`
  / competitor id (`sub_7404`, `sub_7608` :845-910: only meaningful for a friend ped in `g_U28807._fU12`).

## Minimal safe approach for up to 4 humans (recommended: "everyone uses side P")
Constraint: only side P reads input, so every human has to take the P path. Seat humans one at a time as the P ped.
1. **Launch** (host): hook `START_NEW_SCRIPT_WITH_ARGS` for `"golf"`; leave mode 2 (ambient; the script's stranger
   O ped stays as a shooter-less dummy) or in friend mode keep the friend as O. Only the host's player triggers the
   launcher, so the host must be at the tee spot; partners are teleported/watch via `scene_cam` like bowling.
2. **Identity of side P**: hook `GET_PLAYER_CHAR` (native) when the caller is thread `golf` and the player id is the
   local one: return the *active human's* ped (host's own player, or a partner puppet). All ~60 `sub_1732()` uses
   (shooter anims, tee attach, hand-over, celebration, exit) then follow the active human with no static writes.
   `GET_PLAYER_ID` can stay as is.
3. **Input**: during a partner's P turn answer from golf only: `GET_POSITION_OF_ANALOGUE_STICKS`, `GET_MOUSE_INPUT`,
   `IS_USING_CONTROLLER`, `GET_CONTROL_VALUE(0,4/87)`, `GET_KEYBOARD_MOVE_INPUT`, `IS_CONTROL_JUST_PRESSED` for pads/controls
   (2,137) (2,1) (0,137) (0,1) (2,77) (0,77) (2,98); keep (2,23) (help/quit) host-only. Same latch/stale pattern as
   `bowling_sync` (`on_sticks`/`on_control`); the partner samples with its own tick so hooks do not see it. Edge presses
   must be latched until read (back-swing press, release press).
4. **Scorebook**: the mod keeps `{shots_left, score}` per human and writes it into side P's statics just before that
   human's turn (`l_U2884` shots, `l_U2887` score) and reads them back when the shot is scored
   (`l_U3138` reaches 3, or `l_U2883[0]` changes). Keep `l_U2885` (O shots) > 0 while any human has shots so the
   script's `(l_U2883[1] > 0 || l_U2883[0] > 0)` loop test (:2433, :2759) keeps playing; for the last human's last shot
   set `l_U2884 = 1`, `l_U2885 = 0` so the normal game-over path runs; write the winner/runner-up scores into
   `l_U2887/2888` before `sub_23611` case 2 final compare (:2679-2692) or ignore that speech. Draw the 2-4 player
   card as an overlay (like `bowling_sync`), the script's own HUD (`GOLF_P_SCORE`, :2519) only shows slots 2887/2888.
5. **Turn rotation without O**: the script always hops P -> O -> P. Use its own skip path to bounce the O half-turn:
   while `l_U2890 == 1` and the active human is not the O ped, answer `IS_CONTROL_JUST_PRESSED(2,77)`/`(0,77)` true once
   after `GET_GAME_TIMER - l_U3081 > 1000` (:2344, `sub_11945` :1248); the script then runs `sub_21570` (:2369): resolves a fake
   O shot (random score, `l_U2886[1]` += -> ignore it), `l_U2890 = 0`, hands the tee back (:2433-2462). Change the
   active human (the `GET_PLAYER_CHAR` hook's answer) **during the O half-turn**, so the O->P choreography detaches/
   re-attaches the new human and the previous one is parked at `l_U2855`.
6. **Club**: the P club `l_U3100` was attached to the first P ped at creation (:670). On each human change re-attach
   with `ATTACH_OBJECT_TO_PED(l_U3100, newPed, 1219, {0.03,0.017,0}, {4.75,3,1.25}, 0)`.
7. **Parking**: only one waiting spot (`l_U2855`) and the bystander bench (`l_U2858`); park extra humans (not the
   shooter) next to `l_U2855`/`l_U2858` offsets, frozen. Partner views = the host's scripted camera (scene_cam stream).
8. **Alternative for 2 humans (more invasive)**: seat the partner as O by writing `l_U3093` (arg word 0, mode 0/1 so
   it is not deleted/regrouped) and synthesize O's shot from the partner's aim/power by writing `l_U3077` (target),
   `l_U3054` (power), `l_U3140 = 4` as `sub_21073` does (:2297-2342). Reimplements the swing meter; not recommended.

## Risks / unknowns
- Puppets are net-driven. The script attaches them to `l_U3102`, plays `MINI_GOLF` anims, `CLEAR_CHAR_TASKS_IMMEDIATELY`,
  `SET_CHAR_COORDINATES`; the puppet driver must suspend (like the bowling run-up) or these fight each other. Whether the
  partner's own machine shows its character swinging (anims are not mirrored) is unknown; likely it only sees the host
  puppet if puppets are replicated with their anims.
- Hooking `GET_PLAYER_CHAR` per thread must not change `GET_PLAYER_CHAR` for other scripts; `SET_PLAYER_CONTROL`,
  `SET_CAM_BEHIND_PED`, `DISPLAY_RADAR`, `GIVE_WEAPON_TO_CHAR` etc. target the host player's id/ped (the host ped may
  be the hooked shooter or not); the host human must not be controllable while a partner shoots (the script enables
  control at the start of every P turn, :2452, 2805).
- Statics layout of the arrays (size word then elements) is inferred from adjacency (2883+3 -> 2886, 2886+3 -> 2889);
  verify at runtime (size words 2 and 2) like bowling's `readable_range`/`script_ok` check, bail to vanilla otherwise.
- The O half-turn hop adds >= 1 s per shot and leaves the camera set for an O turn (`sub_8254(0)` runs while
  `l_U2890 == 1`, :2765, so the aim camera heading is not pre-aimed, minor).
- Stats/achievements/`g_U28807` friend result land on the host save. `l_U3094` bystander is only a friend; in ambient
  mode it is nil.
- Mode 2 deletes `l_U3093` on exit; do not write a puppet into `l_U3093` without setting mode != 2 and dropping the
  `SET_GROUP_MEMBER` (:322, 334) that would put a puppet in the host's group.
- Unverified in game: all of the above is from decompiled code.
