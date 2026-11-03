# Venues, drinking, shows, nightlife and minigames for up to 4 players

Static analysis of the decompiled scripts (`C:\Users\dosgo\dev\research\GTAIVMissionScripts\{IV,TLAD,TBOGT}`). Nothing here was
run in-game; everything marked **[verify]** needs a test. Line numbers are in the named script's `.txt`.
Rule used throughout: a script matching `LocalScripts` (`amb*, vendor*, foodserver*, binco*, *help, carfood*`, plus the
always-local `spcellphone*, parachute*, *stattracker`, `story_sync.cpp:is_story_script`) runs on every machine; all else
runs only on the host and partners see it through the replication layer.

## 0. Headline findings

1. **`drunk_sync` probably never fires for friend drinking.** It starts only on `SET_PED_IS_DRUNK(host player, 1)`
   (`drunk_sync.cpp on_drunk`, `ped != me` returns). In every friend script (IV `<friend>drink|eat|show|strip|darts|pool|tenpin`,
   TLAD `clay|jim|terry*`) `SET_PED_IS_DRUNK(x,1)` is only ever called on the **friend** (IV `romandrink` 9520 `uParam1` = friend,
   3125 reset likewise; TLAD `claydrink` 11470 loops the 3 group peds `g_U38857[I]`). TBoGT never calls it at all. The only
   script that drunks the player ped is `go_on_date` 7721 (`SET_PED_IS_DRUNK(sub_1491(), 1)`, sub_1491 = player ped). What the
   scripts do to the *player* is: `SET_DRUNK_CAM` (camera), timed ragdoll/NM stumble, and `SET_VEHICLE_STEER_BIAS`. So in a
   friend outing `g_host.drunk` stays false, the mask is never computed, and `client_tick` never applies anything (it needs
   `g_in.drunk && mask`). Cam values are recorded but not applied. **[verify]**: do a `romandrink`; the log line
   `Drunk: host is drunk, present partners mask` should be absent. FEATURES.md/ARCHITECTURE.md claim this works; it likely
   only works for dates.
2. **No script has a "drink count".** Drunkenness is one event at the start of the activity stage with a fixed level
   (see 1.1), not drink-by-drink. The only per-drink-ish thing is TBoGT `ambdrinking`'s 3-camera "round" scene (1.3).
3. **IV `ambbar` is not drinking.** It is only the bartender's idle loop (clean glass / wipe counter, `AMB@BAR`), runs locally,
   needs nothing. There is no ambient pay-and-drink in IV. TBoGT `drinkingrect` is just the black-rect + "WHERE_AM_I" line shown
   after a blackout wake-up.
4. **Local scripts that launch non-local children can freeze a partner [verify].** `ambNightclub*` (local) does
   `SET_PLAYER_CONTROL(0)` then `START_NEW_SCRIPT("dancing")` (e.g. `ambnightclubbm2` 4361, `ambnightclubbm_min` 3757) and
   `ambnightclubm92` 154-170 does `SET_PLAYER_CONTROL(0)`, fade out, `START_NEW_SCRIPT("ClubSecurity")`. `dancing`/`ClubSecurity`
   are not local, so if the partner's copy of the club script gets that far and the child is suppressed, the partner is left
   frozen on a black screen. The gates are per-machine globals (`g_U15811[..]` unlock flags, `g_U30105/30106`), so it may
   simply never trigger on partners. Test by walking a partner into Maisonette 9 / Bahama Mamas.
5. **`nutvendor`, `burgervendor`, `magvendor` do not match `vendor*`**, so they are suppressed on partners while plain `vendor`
   is local. Config-only fix: add `*vendor` to `LocalScripts`. (Not checked in-game whether the ped/cart duplicates.)

## 1. Drinking

### 1.1 IV friend drinking (`romandrink`, `jacobdrink`, `packiedrink`, `dwaynedrink`, `bruciedrink`)

All five are the same ~16k-line "friend activity" framework (`romandrink` and `romaneat` share
the framework code; `drink|eat|show|strip` differ in which stage runs). Not local, host-only.

Start path: phone call (the friend scripts own the whole outing) -> stages in `sub_3566` (`romandrink` ~560-640:
1 setup, 2 pickup, 3 drive, 4 `sub_57797` (8349) arrival, 5 `sub_65609` (9407) hang out, 6 pass). Arrival (8395-8545):
fade out 1000 ms, `CLEAR_CHAR_TASKS_IMMEDIATELY`, `SET_CHAR_COORDINATES` for player and friend inside the venue, fade in,
`SET_PLAYER_CONTROL(1)`, friend becomes group member. Activity ids in `sub_45665` (7020-7060): 4 boating, 5 bowling,
6 darts, **7 DRINKING**, **8 EATING**, 9 helirides, 10 lifts, 11 pool, **12 SHOW**, **13 STRIPCLUB**.

How drunk starts (`sub_65609` 9407 -> `sub_65777` 9439 -> `sub_65803` 9493):
- Level from `sub_65635` (9425): drink stage uses 0 -> **level 4**; when a friend script hops to another activity
  (`sub_81361` 11923) it is mapped 3->3, 7->2, 8->1, 13->5 (so eat = mild, strip = heavy).
- `sub_19176` (3130) `SET_DRUNK_CAM(rootcam, i, ms)`: L1 0.20/300 s, L2 0.30/240 s, L3 0.45/210 s, L4 0.60/180 s,
  L5 0.70/180 s; L6 (sober) `0.0, 55536` (= -10 s ramp down; TLAD writes `-60000`).
- `sub_19456` (3176) "ragdoll window": L1 60 s, L2 90 s, L3 120 s, L4 140 s, L5 180 s. For that window `sub_66714`
  (9660) re-triggers `sub_19765` (3250) = `SWITCH_PED_TO_RAGDOLL(ped,0,<=65534,..)` + NM message 79 (drunk balance) on the
  player *and* the friend, and `sub_66877/67478/68660` add fall/stagger logic (9707-10090). Driving: `sub_68785` (10093)
  `SET_VEHICLE_STEER_BIAS(car, bias)` every second, `sub_68925` (10124) makes cops react to drunk driving.
- `SET_PED_IS_DRUNK(friend,1)` only (9520). End: `sub_19153` (3105) cam 0 / level 6, ragdoll off, steer bias reset (3293),
  `SET_PED_IS_DRUNK(friend,0)` (3125). Help `FRDRUNK` 11432, `DRKTAXI` 10051 (get a taxi home).
- Total duration: about 3-5 min of effects after arriving, **not tied to leaving the bar** (the cam keeps ramping and the
  ragdoll window runs from the timer in `l_U466._fU24 = now + window`).
- TLAD `claydrink|jimdrink|terrydrink` (`sub_15324` 2605, level table 2667-2671, `SET_PED_IS_DRUNK` 11470) is the same, but
  the group is up to **3 peds** (`g_U38847[I]`/`g_U38857[I]`), so TLAD scripts already cope with several drunk companions.
- TBoGT `dominicanfriend` has Armando + Henrique as two companions; activities (5694-5730): 6 airhockey, 7 armwrestling,
  8 boating, 9 bowling, 10 DANCING, 11 darts, 12 DRINKING, 13 EATING, 14 GANGWAR, 15 golf, 16 hangout, 17 helirides,
  18 hilocards, 19 lifts, 20 pool, 21 RACES, 22 show, 23 stripclub. Drunk is the shared TBoGT system: a request in
  `g_U9074[5]` (`Make_Ped_Drunk(ped, ragdollMs)`, e.g. `ambdrinking` 1649-1690) consumed by the **non-local**
  `DrunkController` script (started in `TBOGT/initial.txt` 78+, `drunkcontroller.txt` 355) which spawns a `drunk` script per
  ped (ragdoll `drunk.txt` 649, NM 678/744/964, steer bias 1400); camera via `Activate_Drunk_Camera` (`ambdrinking` 1909-1929
  `SET_DRUNK_CAM`, `dominicanfriend` 2423 ramp-down). `g_U9199` marks "the player is drunk".

Current co-op: the whole outing runs on the host. Partners are regrouped into the venue by the mod's regroup, but they get no
drunk effects (headline 1). On partner machines nothing drunk-related runs (DrunkController and the friend scripts are suppressed).

Proposal (value high, risk low):
1. **Fix the trigger in `drunk_sync`.** Treat `SET_DRUNK_CAM(intensity>0)` as "host drunk" (start: compute the 30 m mask; end:
   `intensity==0`), and/or `SET_PED_IS_DRUNK(any non-player ped, 1)` within ~10 m of the host as a start hint. Both
   `ambdrinking`/`dominicanfriend` (TBoGT) and IV/TLAD scripts go through `SET_DRUNK_CAM`. Keep `SET_PED_IS_DRUNK` on the
   host copy for partners as now.
2. **Own drunk level per partner.** Send the host's *level* (cam intensity picks 1-5 by the table above) plus the ragdoll window
   in the message; partner applies `SET_DRUNK_CAM(own level)` with its own timer, `SET_PED_IS_DRUNK(me,1)` for the
   animated sway, and (only while on foot and inside the window) occasionally `SWITCH_PED_TO_RAGDOLL`+NM 79 as `sub_19765`
   does. For variety offset partner level by -1..+1 from a stable hash of the player id; end each on its own timer (partner
   ends by `SET_DRUNK_CAM(0, -10000)` after its own window). No script change needed.
3. Partners who arrive later (not in the 30 m mask at start) get the mask re-checked every 10 s only if they are inside the
   venue's interior (needs a small `host` call; the current mask is frozen at start).
4. Drunk driving for partners: apply `SET_VEHICLE_STEER_BIAS` on the partner's own car using the host's sinusoid parameters
   (`sub_66000`) or skip; passengers need nothing. Cop reaction: skip.

Risk: low (cam + ped flag, already implemented). The ragdoll stumble is the only risky part (partner losing control in a
car/at a ledge); make it optional.

### 1.2 Walking into a bar as a partner (ambbar / TBoGT ambdrinking / drinkingrect)

- IV `ambbar` (230 lines): bartender creation only (`CREATE_CHAR` 403-525, gated by `Roman7` not running (`sub_306` 74-82);
  `main` 14-55; "Bar Script Killed" when out of brain range). Local on every machine, so each partner machine creates its
  own bartender while the host's story also streams the world. No player interaction, no drinking. Nothing to do.
- IV has **no** self-service drinking. To get drunk in IV you must run a friend `drink` outing (host-only) or a date.
- TBoGT `ambdrinking` (1936 lines) / `ambfoodeating` (1942, identical but eating text `E2FA_FOOD*`): local amb* but not
  ambient in practice. Gate (`sub_5912`/`sub_6440` 434-570): `g_U12379` (on-mission flag) AND `g_U28807` (activity queue:
  `_fU24`/`_fU28` requested activity id 5 = drinking, `_fU60[5]>0` count). A `LOCATE_CHAR_ANY_MEANS_3D` radius 2.5 at the
  chosen venue marker (`l_U281[0..17]`, 18 bars, + `l_U339[0..7]` 8 food spots, `sub_1043` 201-230) starts it, then 3
  scripted cameras (Luis, Armando, Henrique "E2FAHA", `sub_8445` 900-1030 cases 0-6), fade out 3 s, `WARP`/`SET_CHAR_COORDINATES`
  to a wake-up location (`uParam0._fU16`), and `sub_13949` (1624): `Make_Ped_Drunk(Luis, 90 s)` + same for both friends +
  `Activate_Drunk_Camera(180 s, 0.30)` (1050253722 = 0.3f). `drinkingrect` handles the black rect.
  Because `g_U28807` is queued by the host's `dominicanfriend` flow, a partner machine's copy has an empty queue, so a
  partner walking into a bar **does not** get a drinking scene (**[verify]**). The host's scene plays for the host; partners
  see the shared cutscene/scripted camera (per FEATURES) and are regrouped.
  Proposal: nothing (the host's scene is the activity); optionally apply 1.1 step 2 using the 90 s + 180 s numbers when the host's
  `SET_DRUNK_CAM(0.3, 180000)` arrives.

### 1.3 Optional new thing: mod-owned "have a drink" at any bar (value medium, risk medium)
If you want ambient drinking per player, build it in the mod, not by reusing scripts: bar/ped-bartender proximity prompt, each
press adds one drink (level 1..5 via the tables above), own ragdoll window/cam/walk, own timers, up to 4 independent. Needs
bartender spots (the `AMB@BAR` idle positions in `ambbar` 18-30 give two; TLAD/TBoGT `l_U281` gives 18) and a model for a
paying-the-bartender animation. Not recommended before the trigger fix.

## 2. Eating, shows, strip clubs, TV, internet

| Thing | Script(s) | Local? | Single-player structure | Partner today | Proposal |
|---|---|---|---|---|---|
| Street food / vendors | `foodserver` (1513 lines), `vendor` | yes (`foodserver*`, `vendor*`) | player walks to cart, `CREATE_CHAR` server ped, pays, health | each partner buys on their own | none |
| Nut / burger / magazine stands | `nutvendor` 1-1528, `burgervendor`, `magvendor` | **no** (names don't match `vendor*`) | same as vendor, `SET_PLAYER_CONTROL(0)` 289, creates seller `CREATE_CHAR` 533 | blocked on partners | add `*vendor` to `LocalScripts` (config) [verify cart/ped duplication] |
| Friend eating | `<friend>eat` (activity 8) | no | friend framework; level-1 drunk path (1.1) | host-only, partners tag along | nothing (see 1.1); partners can walk in and sit |
| Friend show | `<friend>show` (12) -> venue scripts `ambcomedyclub`, `ambcabaret` | friend: no; venue: yes | friend takes you to the venue; friend scripts set `g_U9212=1; g_U9347=1` on cleanup (`romandrink` 3094-3095, 12024-12025) and 0 at the start of an activity (12083-12084) | see below | see below |
| Friend strip club | `<friend>strip` (13), venue `ambstripclub` | friend no; venue yes | same | see below | see below |
| TV | `ambtv` (1173 lines) | yes | safehouse locates (3 radius 50 volumes, e.g. 890.96,-494.86,18.40 / 102.59,845.95,48.84 / -971.0,891.2,18.78), channel cams on the TV rendertarget, skips if `playboy4`/`francis4` running | each partner uses their own safehouse TV when in range of one | none |
| Internet cafe | `ambinternetcafe` (847) | yes | clerk `CREATE_CHAR` 172-176, `LOCATE_CHAR_ON_FOOT_3D` computer 501 (1x1x2), sets `g_U8394`, `g_U8392/8393` | per machine, independent | none; shared globals are per machine so 4 can browse at once |
| Safehouse shows | `ambshowroom` (261) | yes | showroom display | ok | none |

Venue scripts (IV): all three are local and drive the **local** player (`GET_PLAYER_CHAR(GET_PLAYER_ID())`).
- `ambcomedyclub` (1600): gate at 57 `(sub_987() || l_U6) AND michelle1 not running AND g_U9212`, then `LOCATE_CHAR_ANY_MEANS_3D`
  at `l_U152` r=2.5 (59), `SET_CHAR_PROOFS`, `SET_PLAYER_CONTROL(0)` (76), fade out 500 ms (651), `AMB@COMEDY` audience
  (laugh/clap anims 1074-1104), `SET_CHAR_COORDINATES(player, l_U161)` seat (1015) + `TASK_USE_NEAREST_SCENARIO_TO_POS_WARP`.
  Help `COMEDY_WANTED` if wanted.
- `ambcabaret` (1512): gate at 190 (needs a date/friend via `l_U100 || sub_6860()` and `g_U9347`), help `CABARET_HLP1` r=4,
  blip, trigger 1x1x1 at `l_U25`, `SET_PLAYER_CONTROL(0)` 215, scripted cam `ACTIVATE_SCRIPTED_CAMS` 219, walk to seat
  `TASK_GO_STRAIGHT_TO_COORD` 226, seats at 965.87,-274.37,17.27 (player) / 966.49,-276.11 (date `l_U81`), control back 313.
- `ambstripclub` (4308): `STR_WEA` help (no weapons), `g_U9157/9158` (anger flags), `g_U9172/9173/9178` claim one dancer
  (lap dance), `TASK_USE_NEAREST_SCENARIO_TO_POS` 1560-1574, leave prompt `STR_LEAVE`.
Why they work per player: local, driven by the local player's own coordinates, no host state except the `g_U9212/9347` flags.
Why they may *not* work for a partner **[verify]**: those flags are set only by friend/flow scripts that are suppressed on
the partner machine (initial value 0, or the partner's save). If the partner stays at 0, a partner walking into a comedy
club / cabaret / strip club gets nothing even though the host does.
Proposal: (a) log `g_U9212/g_U9347` on a partner once; if 0, have the mod write 1 into the partner's globals while joined (the
mod already sets script globals for flow state; same mechanism as island unlocks) -> every partner can attend the cabaret/comedy
club/strip club on their own, each with own camera and seat. (b) For a friend show/strip outing, the host's friend script
makes host + friend sit; partners entering the venue with their own local `ambcabaret` would also get the seat, which looks
natural for up to 4 (the 2-seat layout is a limit; there are `l_U24[..]` audience scenarios in the comedy club, so more
seats exist there, cabaret has only 2 coordinates). Value medium, risk low (config/globals) to medium (seat collisions).
TLAD/TBoGT copies of `ambcabaret/ambcomedyclub/ambstripclub` exist with the same shape; TBoGT's venues also show in the
nightlife blips managed by `ClubManBlips` (34 lines, host).

## 3. TBoGT nightlife and activities

### 3.1 Clubs and dancing
- Scripts: `ambNightclubBM` (launcher 5-90: starts `ambNightclubBM_min` or `ambNightclubBM2` by memory), `ambNightclubBM2`,
  `ambNightclubBM_min`, `ambNightclubHC2` (Hercules), `ambNightclubM92`/`M92_2` (Maisonette 9), `ambNightclubExt`, `ambClubLights`,
  `ambPoleDancer` (601). All amb* => local on every machine. They create the club crowd as ambient peds (`UPDATE_CLUB_PED`
  anim tables around 6990-7050 in `ambnightclubbm2`), bar/bartender, and offer dancing.
- `dancing` (10452 lines, **not local**): started by the club script at `ambnightclubbm2` 4361 / `ambnightclubbm_min` 3757 when
  the player is in a dance zone and presses the button (help `DANC_H01`, `START_NEW_SCRIPT("dancing")`, `SET_PLAYER_CONTROL(0)`),
  with `g_U30206` = the NPC dance partner ped and `g_U30190` = mode (0 pair/1 solo/2 ...). DDR-style move prompts
  (`e2_dancenumber_1..12`, `missdance_{bama,m9,herc}_{solo,pair}_` anims, `dancing` 148-260), score vs a goal.
- Clubs also have `Make_Ped_Drunk(club ped, 90000)` for patrons (`ambnightclubbm2` 7040), not the player.
- Gates are per-machine globals (`g_U15811[..]`, `g_U30105/30106`), not host-only.
Current co-op: host dances alone (dancing is suppressed on partners, so the dance score/partner exist only on the host); partners
see the host's pair dance via the replicated anims. A partner pressing the club-script dance button hits headline 4 [verify].
Proposal: **dancing as its own player-local minigame** (value high for fun, risk medium): add `dancing` to `LocalScripts` (each
partner runs their own `dancing` against their own club NPC; the score is per player already since it uses
`PLAYER_CHAR`/`GET_PLAYER_ID`); the club script is already local so the whole chain `ambNightclub* -> dancing` works on every machine, and the
dance NPC is the partner's own local ambient ped (not synced; players dancing in the same club see only each other's
generic dance anims, which exist already). Key risk: the club crowd/NPC would be created per machine in a world the host also
streams (duplicates); the mod already thins joiner ambient peds near the host (`ambient_sync` kThinOnM 100) which helps.
Also add `ClubSecurity` handling: leave host-only (3.2).

### 3.2 Club management / security
`ClubSecurity` (6947): started by `ambNightclubM92` 170 (after `sub_39010(2,"START_CLUBSEC")`), host-only. Blips (375, 782, 1148),
`LOCATE_CHAR_ON_FOOT_3D` at door (-480.67,165.78,10.61), a "LOOK_AROUND" phase (759), spots a troublemaker `l_U600`
(`ADD_BLIP_FOR_CHAR` 782, approach 8 m / 1.6 m 934-942), ejects them; `PRINT_NOW "ENDED"` 661. `ClubManBlips` (34 lines, main 28234)
just holds `SET_MISSION_FLAG` while tonym* or ClubSecurity run. Proposal: leave host-only; partners follow as bodyguards (existing
regroup + team-fire settings). Add a note: do not let partners trigger `ambNightclubM92` 154 while joined (headline 4).
Value low, risk none.

### 3.3 Fight club
`fightclublauncher` (410, host only; `initial`) enters at (-396.61,1463.02,15.52) r=7 with `g_U15811[25]==1`, help
`FIGHT_HELP`, then `START_NEW_SCRIPT("FightClubRing", 1820)` (137); `FightClubRing` (7258) starts `FightClubScreen` (5366).
Structure: AI opponents (`CREATE_CHAR` 1599-1605, 4518, 5628, 5703; `l_U532[0..1]` health 180..270 by tier 5761-5831),
`TASK_COMBAT(opponent, PLAYER)` (3944, 4684, 4831), rounds A-F (`RoundA..F` 2764-2939), cash bet (`NoCashFC` 1224), belt (`Belt`
3366), `SET_CHAR_COORDINATES(player, ring)` (3224, 5422), `SET_PLAYER_CONTROL(0)` for the walk-in/out; crowd cheering via
`SAY_AMBIENT_SPEECH_WITH_VOICE` (1762). Single player vs 1-2 AI fighters, one at a time.
Current co-op: host fights; partners regroup into the arena room as spectators (mission-style sync of the AI fighters); crowd
and result are the host's.
Proposal (value medium, risk low-to-high):
- Phase 1 (do now, no code): partners spectate; team-fire "No aim" keeps them from hitting the host. Verify the ring doesn't
  block a partner entering (fight club doors are `SET_STATE_OF_CLOSEST_DOOR`, `FIGHT_CLUB_DOOR_SLOT_OPEN` 1202).
- Phase 2 (optional): "1v1 human vs human" = replace the opponent ped with a partner: hook `CREATE_CHAR` (model 52357603 etc.) in
  `FightClubRing` so the opponent slot returns the partner's puppet handle, skip its `SET_CHAR_HEALTH`/`TASK_COMBAT`
  (partner steers themselves), sync damage through the existing melee sync. Needs: winner detection from health thresholds
  (script reads opponent ped health), and the puppet's ped health must be set (script sets 180-270 on the puppet, hurting the
  partner). High risk; defer.
- A 3-4 player brawl is not supported by the script's one-opponent loop.

### 3.4 Base jumping
`baseJumpLauncher` (1058, host-only; `initial` 80): 15 jump sites (`for I<15` blips 410-455, sprite 106), `g_U42975[I]` per-site
state, button prompt `sub_7574/sub_7835`, then `g_U12379=1`, `SET_PLAYER_CONTROL(0)`, fade out 500 (123-132), `START_NEW_SCRIPT_WITH_ARGS("baseJumping", ref l_U524, 42, 8192)`
(161), waits for it to end (case 4). `baseJumping` (2039): puts the player at the site (`SET_CHAR_COORDINATES` 108), freefall
(`TASK_PLAY_ANIM_NON_INTERRUPTABLE "Free_Fall" "PARACHUTE"` 362/422/373), rings `ADD_SCORE(player, 30)` (564-668)
+ `PARACHUTE_COLLECT_RING` sound, landing target (score/time), `BasejumpScreen` HUD (`main` 23341).
Current co-op (FEATURES): checkpoints/ring track show for everyone, parachute auto-given to partners. The jump itself is the
host's; partners are regrouped to the platform and can jump on their own but have no ring score/time.
`parachute*` is already always-local, so a local jump per partner is natural.
Proposal (value high, risk medium): **each player runs their own `baseJumping`**:
1. Add `basejump*` (matches `baseJumpLauncher`, `baseJumping`, `BasejumpScreen`) to `LocalScripts` on partners; keep the host as is.
2. Partner machine has per-machine `g_U42975` (unlocked sites) and `g_U12379`: set the sites/flags as for the host when
   joined (same global-write mechanism as item 4).
3. Cross-player: the host broadcasts "jump N started"; partners auto-trigger their own launcher at the same site (so all
   four jump in a group, simultaneous freefall), and after landing each broadcasts score; HUD adds a mini leaderboard (mod overlay).
Risks: ring objects/blips are created by the script per machine (fine, local); partner's ped is mirrored on the host (the
host's `ADD_SCORE` payout hook `story_sync` 0x537379A8 `payout` must not double-count, scores are per machine);
`SET_CHAR_COORDINATES` at the platform fights with the mod's regroup. Value: reuses one script per player, no sync of the jump.

### 3.5 Races (TBoGT `racesp`/`racesplauncher`, TLAD `RoadRash`/`racelauncher`, `malcrace`)
- TBoGT: `racesplauncher` (420, host-only) 3 start circles (612.83,752.26,4.03 / -1034.89,667.39,5.24 / 816.48,321.37,7.38),
  gate `g_U15811[29]==1`, locate r=2.5 on foot, `g_U12379=1`, `START_NEW_SCRIPT("raceSP", 8192)` (47). `racesp` (8865) racer array
  `l_U1129[4]`: per racer `_fU0` ped, `_fU8` car, `_fU12` model, `_fU24` blip, `_fU32` checkpoint, `_fU36` grid pos,
  `_fU52` current checkpoint idx, `_fU60` lap; race struct `_fU16[]` checkpoints, `_fU824` count, `_fU828` laps,
  `_fU852` type 0-7 (car, boat 3/4, mix 6/7 with a parachute leg). AI racers `CREATE_CAR`/`CREATE_CHAR_INSIDE_CAR`
  (3249-3283, 4626-4638, 4716-4736), driven by `TASK_CAR_MISSION*`/`TASK_CAR_DRIVE_*`; player at random slot
  `l_U1128`; checkpoint blips (`sub_32902` 3640-3730), `RacesScreen` HUD (`main` 23410). Position/win is the script's own
  distance/checkpoint ordering over the 4 slots.
- TLAD: `raceLauncher` (2329) -> `RoadRash` (9671), same racer array (`l_U630`, 2536-2558), `malcRace` (10289, only inside mission `malc2` 1000).
- **There are exactly 4 racer slots (player + 3 AI) in every race script, which maps perfectly onto 4 players.**
Current co-op: the host races the AI, partners watch (checkpoints shown). Positions are host-only.
Proposal (value high, risk high; the priority-1 "big" item):
- Phase 1 (cheap): partners drive along, host results only.
- Phase 2: **partners take the AI slots.** In `raceSP`/`RoadRash` only (script-name filter in hooks), intercept `CREATE_CAR` /
  `CREATE_CHAR_INSIDE_CAR` / `CREATE_CHAR` for AI slots (call sites above) and return each partner's existing car/ped (or create and
  replace them), and ignore `TASK_CAR_*` aimed at those peds. The script then tracks them with its own checkpoint/lap logic
  (it uses `_fU52` progress and `LOCATE_*` against the racer ped, which works on the puppet's synced position).
  The host script computes places for 1+N racers and ends the race. Partner HUD: the existing replicated `RacesScreen` +
  checkpoints; add `finish place` text from the host.
  Risks: puppet/vehicle position lag causes missed checkpoints (script uses small radii), partner has to start the race from
  the grid (regroup puts them into their own car "right behind" the host, per FEATURES), boats/parachute legs of mixed races,
  and result flow (the host's `g_U12379` etc.). Do it after base jumping.

### 3.6 Drug wars
`DrugWarsControl` (1112, from `initial` 78; host) checks `g_U15811[28]`, LOCATE at marker pads (`l_U161/171/181/188`, 12.5 m
entry, `LOCATE ... 2.5` trigger), sets the type and starts one of `DrugWarsDeal` (9364), `DrugWarsPickup` (9412), `DrugWarsConvoy`
(8108), `DrugWarsShipment` (8565) (counts at 125-129, 600-604), HUD `DrugWarScreen` (main 23375), `ability_vehicle` conflicts. Single
player: collect/sell/convoy missions with AI enemies, rewards in cash (`g_U42919` and rank thresholds 884-936).
Current co-op: mission-style host content; partners help fight (team-fire off by default for allies). No per-player score.
Proposal: keep host-only; add pay-out sharing (mod's `payout` ADD_SCORE hook already runs). Value medium, risk none.

### 3.7 Gang wars (TLAD)
`GangWarC` (1850, `TLAD/initial` 85; host) places blips for turf points (`l_U380/390/400/407`), `GW_HELP_01..03`,
`SET_PLAYER_CONTROL_ADVANCED(…,0,1,1)`, launches `GangWar1` (12316, assault) or `GangWar2` (13181, defend). They use relationship
groups (`SET_CHAR_RELATIONSHIP_GROUP` 1820, 2099, 2358-2374; allies in `g_U11081`/`l_U838`), `CREATE_CAR 301427732` per
site (2970 ff.), enemy blips (4843+). Current co-op: host mission; partners fight. Proposal: nothing (best case, they are
group members by relationship group 0; check partners are treated as allies and not hit by the AI allies).
TBoGT `GANGWAR` is also a friend-activity id (14).

### 3.8 TLAD `ambClubhouse`, `ambGentlemansClub`
`ambclubhouse` (262, local): creates stray bikers and bartender (`CREATE_CHAR` 4, `AMB@BAR`), two sites (994.03,-267.83,25.24 /
-1721.25,339.66,36.34), checks time of day/weather, nothing player-driven. `ambgentlemansclub` (65, local): one `CREATE_CHAR`
doorman at (-33.35,765.82,13.72). Both ambient set dressing; nothing to do.

## 4. Priorities (value / risk)

| # | Item | Value | Risk | Effort |
|---|------|-------|------|--------|
| 1 | Fix `drunk_sync` trigger (use `SET_DRUNK_CAM>0`, own level/timer per partner) | high | low | small |
| 2 | `LocalScripts += *vendor` | low-medium | low | config |
| 3 | Verify/guard `ambNightclub*`->`dancing`/`ClubSecurity` freeze on partners | high (prevents softlocks) | low | test + small block |
| 4 | Venue flags (`g_U9212`, `g_U9347`) on partners so cabaret/comedy/strip work solo | medium | low | small |
| 5 | Base jumping local per player (+ shared start, score board) | high | medium | medium |
| 6 | Dancing local per player (`dancing` as local) | medium | medium | small after 3 |
| 7 | Races: partners in AI slots | high | high | large |
| 8 | Fight club: partner as opponent | medium | high | large |
| 9 | Optional mod-owned bar drinks | medium | medium | medium |
| 10 | Drug wars, gang wars, club security, clubhouses | low | none | none |

## 5. Tests to run
1. `romandrink` with a partner next to the host: check the log for `Drunk: host is drunk`; if absent the trigger fix (1) is confirmed needed.
2. Partner alone walks into `ambcomedyclub`/`ambcabaret`/`ambstripclub`: does anything start? Print `g_U9212/g_U9347`.
3. Partner walks into Maisonette 9 and the Bahama Mamas dance zone: frozen/black screen means headline 4.
4. Partner at a base jump site/race start: any prompt? (expected none, host-only).
