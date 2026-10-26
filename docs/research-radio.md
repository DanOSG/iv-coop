# Research spike: how GTA IV's radio picks what plays, and how to sync it

Status: static analysis only (nothing was run or patched). Target: `GTAIV.exe` 1.2.0.59 (image base `0x400000`).
Method: strings/RTTI, `tools/find_pattern.py`-style scans, `dumpbin /disasm` on the **plain** part of `.text`, the
game's audio config files (read-only), and FusionFix's `natives.ixx` for native names/signatures (facts only).
The encrypted range `0x401000-0x4FB000` was not analysed (DRM boundary); where radio code calls into it, that is
noted and the question falls back to black-box experiments. Scratch scripts are not in the repo.

## Verdict

**Feasible, with an experiment first.** The radio is implemented in plain, readable code (`0x943xxx-0x946xxx`, the
natives at `0x9CBxxx-0x9CDxxx`), the natives we need are thin wrappers around it, and the track lists live in
ordinary data files we can hash. What static analysis could **not** settle is the central modelling question
(is a station's position a pure function of a global clock, or a per-station state machine with its own RNG
that only advances while something is tuned/streaming?). I found no global radio clock in the plain code, and
the stations carry their own state (current/previous track, per-station RNG-like selection through helpers in
the encrypted range). So the safe plan is **state replication, not clock replication**: the host tells clients
"station S plays track T at offset O" and clients force it with natives, then re-corrects on drift. The
experiment at the end decides whether cheaper clock-only sync is possible.

| Question | Answer |
|----------|--------|
| 1. Continuous virtual timeline from a global clock? | **Not found / unproven.** Per-station state object, no clock global located; track choice calls helpers in the encrypted range (`0x47C2E0/0x47C330/0x47C350`, not analysed). Treat as per-station state machine until the experiment says otherwise. |
| 2. DJ/ident/ad/news/weather selection | Done by the same station object from metadata categories (see section 2); news/weather/adverts are separate shared "stations" (`RADIO_NEWS_*`, `RADIO_WEATHER_*`, `RADIO_ADVERTS`). Inputs we can enumerate: category weights, min-time-between-repeats, back-to-back rules. Not a visible function of story progress. |
| 3. Natives | All present and all handlers are in plain `.text`; `FORCE_RADIO_TRACK` has two unexplained ints that probably carry a start offset (strong lead). |
| 4. Track lists | `pc/audio/config/game.dat16` + `sounds.dat15` + `speech.dat` + `pc/audio/sfx/radio_*.rpf` (+ episode `EP1_/EP2_RADIO_*.DAT*`). Hash those. |
| 5. Design | Section 5. |

## 1. Radio code map (GTAIV.exe 1.2.0.59)

All addresses are VAs. Everything below is outside `0x401000-0x4FB000`.

### Class and strings

- RTTI: `audRadioAudioEntity` (string at `0xE91468`, RTTI `.?AVaudRadioAudioEntity@@`), `audRadioEmitter`,
  `audStaticRadioEmitter`, `audEntityRadioEmitter`, `CChangeRadioStationEvent` (`CHANGE_RADIO_STATION_EVENT`,
  a game event, probably what other peds react to).
- No `audRadioStation` RTTI string exists in this build; the station is a plain struct managed by free functions
  (below). The name `audRadioSlot` also does not exist; per-station playback uses **stream slots**
  (`RadioSlot %u state: %s, station: %s, retuningEmitter: %u, retuningStation: %u [stream slot %d]`, `0xEB2658`)
  with states `RADIO_SLOT_IDLE / STARTING / PLAYING / STOPPING / RETUNING / RETUNING_STOPPING_OLD /
  RETUNING_STARTING_NEW` (`0xEB25C0` region). Useful as a debug-print template.
- Selection tuning constants (metadata keys read at init): `RADIO_TRACK_CATEGORY_MIN_TIME_BETWEEN_REPEATS`,
  `RADIO_TRACK_CATEGORY_WEIGHTS_FROM_MUSIC`, `..._FROM_BACK_TO_BACK_MUSIC`, `..._NO_BACK_TO_BACK_MUSIC`
  (`0xE88D04` area). Same for the user-music (Windows Media) station: `USER_MUSIC_WEIGHTS_FROM_MUSIC`,
  `..._FROM_BACK_TO_BACK_MUSIC`.

### Station table and station object

- Station manager init: **`0x945420`** (called once). It resets counts, allocates the station array
  (`0x78` bytes = up to 30 pointers, pointer at `0x11D76AC`, capacity word at `0x11D76B0/B2`), then for
  `i = 1..0x62` looks up metadata objects named `"%s%02u"` (format at `0xE88A40`) via `0x894EC0`
  (metadata hash lookup on the object at `0x115D9A0`) and builds the stations with `0x9458E0` (normal station)
  or `0x945DD0` (the `i != 0` variant). It also creates the shared pseudo-stations:
  `RADIO_NEWS_%02d` news chain (`0x945688`: globals `0x11D7644`), `RADIO_WEATHER_WEATHER` /
  `RADIO_WEATHER_%02d` weather chain (`0x11D7640`), `RADIO_ADVERTS` and the three ad pools
  `RADIO_STATION_ADVERTS_GENERIC / _RUSSIAN / _SPANISH` (`0x11D7648`), `END_CREDITS_TRACK_LIST` (`0x11D764C`).
  These chains are linked lists of metadata nodes (node `+0x39` word = total count, `+0x3B` = next, `+0x3F`
  byte = entries in node, entries `+0x40` stride 8).
- **Station count: byte at `0x11D74F1`.** Station by index: `0x945210` (`arr[idx]` or null). Station by name
  (string): `0x945100` -> `0x40BA60` (hash) -> `0x9450A0` (search). `0x9451F0` = index of the last
  selectable station (count-1 unless a feature flag from the encrypted range, `0x47C330`, says otherwise).
- Station object (offsets observed, semantics inferred - confirm with the experiment):
  - `+0x0000`, `+0x0BD0`: two playback stream-slot structs of size `0xBD0` (crossfade pair). Byte `+0x1917` =
    index of the current one; `+0x17A0 + 4*cur` = a per-slot metadata pointer.
  - `+0x17D0`: head of the station's track-category list (linked: node `+0x13` = entry count, `+0x14` entry
    array, `+0x18` next). `0x945340` re-rolls every entry's pick on start via `0x885490`.
  - `+0x18FC` float (init from node byte `+0x1C`), `+0x1900`..: category/rotation counters
  - `+0x1904` = node byte `+0x12`; **`+0x1908` previous track id, `+0x190C` current track id** (both init -1;
    `0x945120` shifts cur->prev and writes the new id read from slot `+0xBC4`), `+0x1910` current list node.
  - `+0x1917` current slot, `+0x191A/+0x191B` two bytes passed to the stream start, `+0x1918` station tag/name
    copy (filled by `0x4378D0`), `+0x1921` has-something flag, `+0x1923` and `+0x1929` cleared on start,
    **`+0x1927` "frozen/forced" byte**: set by `FREEZE_RADIO_STATION`; `0x945120` (ForceTrack) does nothing when
    it is 0.
- **ForceTrack: `0x945120`** (`thiscall`, ecx = station; `ret 0Ch`; args = `track name`, `a3`, `a4`):
  requires `+0x1927 != 0`, looks the track up by name in the metadata (`0x894EC0`), takes the track entry
  (`entry + 0x40`), and calls the stream-slot start function **`0xB76660(slot, a3, a4, entry+0x40, 0)`**
  (then `0xB769A0` / `0xB76920`). So `a3`/`a4` are forwarded straight to the stream start - the best candidate
  for a start offset (e.g. ms) and a flag. **Unverified**; see experiment E3.
- Anything that actually *chooses the next song* (random pick among categories honouring min-repeat time and
  weights) goes through helpers at `0x47C2E0 / 0x47C330 / 0x47C350` (inside the encrypted range, so not read)
  and the metadata-array helpers `0x8E1420/0x8E12F0/0x8E1640/0x8E1650` (plain; array-of-struct accessors,
  stride 28 bytes). Conclusion: **the RNG itself is not visible here.** Whether it is a seeded deterministic
  generator or `rand()`-like state cannot be answered statically.

### Where "time" enters

I found no read of a wall clock or game clock inside the station functions I disassembled
(`0x945120-0x945340`, `0x9458E0-0x946700` call summary). The only time-like inputs are the
min-time-between-repeats keys (per-category last-played timestamps) and the stream position the audio engine
reports for the playing slot. That is consistent with "each station is its own state machine that advances
while streamed". It does **not** exclude a virtual-position model (position derived from a per-station start
time) implemented in the encrypted helpers. FusionFix patches two radio bugs (`fixes.ixx`: "streamed radio
stations resetting/restarting after playing a cutscene", and Roman's depot background radio); the first one's
existence suggests station state *does* normally survive being un-tuned, only breaks across cutscenes.

## 2. DJ lines, idents, ads, news, weather

From `game.dat16` object names (strings dump): each real station has metadata lists
`RADIO_<STATION>_MUSIC`, `_IDENTS`, `_DJSOLO` (talk/DJ segments), plus bridging sounds
`RADIO_INDEPENDENCE_TO_AD`, `_TO_NEWS`, `_USER_INTRO`, `_USER_OUTRO`. Top-level objects are
`RADIO_STATION_<NAME>` (station definition), `RADIO_NEWS_01/02/NEWS`, `RADIO_WEATHER_*`, `RADIO_ADVERTS`,
`RADIO_STATION_ADVERTS_{GENERIC,RUSSIAN,SPANISH}`. Individual items are in `sounds.dat15`, named
`RADIO_<STATION>_<ITEM>_{STEREO|POS}_<ITEM>_{LEFT|RIGHT}` (that is where the track and DJ sound names come
from, e.g. `RADIO_INDEPENDENCE_INTRO_05_STEREO_INTRO_05_LEFT`, `RADIO_CLASSICAL_AMBIENT_ID_03_...`).
Selection is by the same category/weights mechanism as music (the `RADIO_TRACK_CATEGORY_*` keys), with
news/weather/adverts being shared chains the station inserts: the init code shows the chains are linked lists
walked with a per-chain position counter, i.e. their "what comes next" state is a few integers per chain
(`0x11D7640/0x11D7644/0x11D7648`). Inputs that I could confirm statically: category weights, repeat limits,
back-to-back rules, language ad pools. **Not** confirmed: any dependence on time of day or story progress
(the DJ lines for missions are mission-script-triggered speech, not radio selection). So for sync: DJ/ident/ad
state is "just more station state" and cannot be steered independently; it follows if the station is forced
onto the same track timeline, but spoken segments between songs will diverge after a drift unless re-forced.

## 3. Natives

All hashes from FusionFix's `natives.ixx` (IV hashes are not joaat of the name). Registration is
`push handler; push hash; call 0x86E820` in a block starting near `0x9CE1A0`, so handler addresses below were
read straight from the registration code. `args` = script slot pointer (`[esp+4]` -> `+8` -> arg array),
cdecl.

| Native | Hash | Handler | Signature | Notes |
|--------|------|---------|-----------|-------|
| GET_PLAYER_RADIO_STATION_INDEX | `0x4E493AAF` | `0x9CBC30` | `uint f()` | Current station index (`jmp 0x9A39A0`, reads the audio entity at `0x12845D0`). Sync read. |
| GET_PLAYER_RADIO_STATION_NAME | `0x25136AC2` | `0x9CBC40` | `char* f()` | |
| GET_PLAYER_RADIO_STATION_NAME_ROLL | `0x1A936344` | `0x9CBC50` | `char* f()` | |
| GET_RADIO_NAME | `0x7EC9580E` | `0x9CBC60` | `char* f(int idx)` | Enumerate stations 0..N-1 for the handshake. |
| GET_PLAYER_RADIO_MODE | `0x32795678` | `0x9CBC20` | `int f()` | |
| RETUNE_RADIO_TO_STATION_INDEX | `0x48ED6432` | `0x9CC3E0` | `void f(uint idx)` | Force-retune the player's radio (-> `0x9CD690` -> `0x9A4700` on `0x12845D0`). The sync "apply station" call. |
| RETUNE_RADIO_TO_STATION_NAME | `0x58BA4401` | `0x9CC3F0` | `void f(char*)` | |
| RETUNE_RADIO_UP / DOWN | `0x6B1C6027` / `0x0E843CEA` | `0x9CC400` / `0x9CC3D0` | `void f()` | Fire when the player flips the dial; hook these to detect user choice. |
| IS_RADIO_RETUNING | `0x45C344AA` | `0x9CBEA0` | `bool f()` | |
| IS_RADIO_HUD_ON | `0x45F249B7` | `0x9CBE80` | `bool f()` | |
| FREEZE_RADIO_STATION | `0x08A015CF` | `0x9CBBB0` | `void f(char* station)` | Sets station byte `+0x1927 = 1`. Required before FORCE_RADIO_TRACK works. |
| UNFREEZE_RADIO_STATION | `0x3E5B7E59` | `0x9CC970` | `void f(char* station)` | |
| FORCE_RADIO_TRACK | `0x6A7E47C9` | `0x9CBB90` | `void f(char* station, char* track, int a3, int a4)` | `-> 0x9CCCF0 -> 0x945120`. a3/a4 go to the stream start (offset/flag candidates). Only acts on a frozen station. |
| SKIP_RADIO_FORWARD | `0x12A86E89` | `0x9CC780` | `void f()` | |
| UNPAUSE_RADIO | `0x78F7286F` | `0x9CC9B0` | `void f()` | |
| SET_LOUD_VEHICLE_RADIO | `0x34686B92` | `0x9CC520` | `void f(Vehicle, bool)` | |
| MUTE_POSITIONED_RADIO | `0x32C75195` | `0x9CBF60` | `void f(bool)` | |
| SET_MOBILE_PHONE_RADIO_STATE | `0x52C83902` | `0x9CC560` | `void f(bool)` | |
| ENABLE/DISABLE_FRONTEND_RADIO | `0x5328068B` / `0x6B2F3E97` | `0x9CBAC0` / `0x9CBA20` | `void f()` | |
| REPORT_TAGGED_RADIO_TRACK | `0x0ED8621F` | `0x9CC370` | `void f(int textId)` | Gives the *track text id* of the song the player tagged (the only native exposing a song identity). |

Gaps: there is **no native that returns the current track name/id or playback position.** Song identity has to
come from memory (station `+0x190C`) or from the audio entity; see experiments. Cheat: `REPORT_TAGGED_RADIO_TRACK`
is only for the "like" button.

Which are useful for sync:
- Read: `GET_PLAYER_RADIO_STATION_INDEX`, `IS_RADIO_RETUNING`, and the station struct fields above.
- Write: `RETUNE_RADIO_TO_STATION_INDEX`, `FREEZE_RADIO_STATION` + `FORCE_RADIO_TRACK` + `UNFREEZE_RADIO_STATION`.
- Detect user input (so we don't fight the player): hook `RETUNE_RADIO_UP/DOWN/TO_STATION_*` via
  `native_hooks::on()`; but note most player dial turns go through the input path directly to `0x9A4700`'s
  callee, so also poll the station index.

## 4. Where track lists come from (for the join-handshake hash)

Game folder (`GTAIV/` under the Steam dir; paths below are relative to it):

| File | Holds |
|------|-------|
| `pc/audio/config/game.dat16` (268 KB) | Station definitions, `*_MUSIC / _IDENTS / _DJSOLO` lists, category weights, news/weather/adverts chains. **Primary track-list file.** |
| `pc/audio/config/sounds.dat15` (1.88 MB) | Per-track and per-DJ-line sound objects (names like `RADIO_<STATION>_<ITEM>_STEREO_...`). |
| `pc/audio/config/speech.dat` (544 KB) | DJ/speech banks (`RADIO_AFRO_BEAT`, `RADIO_BABYLON`, ... `RADIO_VLADIVOSTOK`). |
| `pc/audio/config/rpf.xml`, `waveslots.xml` | Which RPFs are loaded into which slots. |
| `pc/audio/sfx/radio_*.rpf` (28 files, 30-330 MB each, `radio_extratracks.rpf` 334 MB) | The audio itself (the downgrade mod replaces/adds these). |
| `TLAD/pc/audio/config/EP1_RADIO_{GAME.DAT16,SOUNDS.DAT15,SPEECH.DAT}` and `TBoGT/pc/audio/config/EP2_RADIO_*` | Episode radio overlays. Story mode only needs the base set unless the user plays an episode. |

No plain-text track list exists; the lists are binary metadata (`.dat15/.dat16`). Hash recipe for the
handshake (cheap, deterministic, robust to the downgrade): `H = SHA-256` (or xxh3) over, in a fixed order,
(a) full contents of `game.dat16`, `sounds.dat15`, `speech.dat`, `rpf.xml`, `waveslots.xml` (about 3 MB total,
instant), and (b) for each `radio_*.rpf`: file size + the first 64 KB (the RPF header/TOC lists the entries) +
the last 4 KB. Do not hash the 3 GB of RPF audio in full at startup. Cache the result per (path, size, mtime).
Additionally send the runtime station list (`GET_RADIO_NAME(0..n-1)`) as a cheap sanity check. A mismatch in
(a) means the metadata differ and radio sync must be disabled; a mismatch only in (b) means the audio differs
(wrong per-station sets would cause silence or wrong songs) - treat as disabled too, with a clear message.
The downgrade mod may also patch `game.dat16`/`sounds.dat15` (it must, to re-add the tracks); so (a) is the
critical part.

## 5. Sync design

### Premises

1. Station choice is cheap to sync (one native each way).
2. Song + offset sync needs us to read what is playing and to force it elsewhere. The levers are
   `FREEZE_RADIO_STATION`, `FORCE_RADIO_TRACK(station, track, a3, a4)` (offset maybe), and `UNFREEZE`.
3. Every player's local selection logic keeps running; after we force a track, the next song is chosen
   locally, which will diverge between clients unless we force it too. So we **replicate decisions
   (host-authoritative), we do not rely on equal RNG**.

### What the host (driver or session host) sends

- Join: `{ list_hash, station_names[], per-radio-owner snapshot }`.
- Radio-owner snapshot `{ vehicle_netid, station_idx, track_id/name, started_at_host_time_ms, offset_ms,
  seq }` for the radio of a **shared vehicle** (driver is owner) and for on-foot "boombox" (ignore for v1).
- Periodic: every 5-10 s a keyframe, and an event on every station change and every track change (the track
  change is detected by polling `+0x190C`; only the owner polls).

### What the client does

- Time base: host timestamp `t_h` translated through the session clock (already in the ENet session; use
  the offset-corrected clock, same as for entities).
- `apply(snapshot)`: if `station_idx` differs -> `RETUNE_RADIO_TO_STATION_INDEX(idx)`; wait for
  `IS_RADIO_RETUNING == false`; then if `track` differs or `|local_pos - expected_pos| > 1.5 s` -> `FREEZE(station)`,
  `FORCE_RADIO_TRACK(station, track, 1?, offset?)`, `UNFREEZE(station)` (the exact ints from experiment E3).
  `expected_pos = offset_ms + (now_session - t_h)`.
- Do **not** touch anyone who is not in the owner's vehicle (decision from `ARCHITECTURE.md`); and when the
  owner's seat is vacated, hand ownership to the next driver and stop forcing.
- User override: a client that retunes manually stops following until the owner changes station or the
  player re-enters the vehicle (no tug-of-war).

### Drift correction

Songs are streamed, so local playback rates are equal within a few ms/min; drift mostly comes from stalls and
from the +1 s of `RETUNING` crossfades. Correct only on keyframes: hard re-force when drift > 1.5 s, never
smooth-resample. Do not correct during the 3 s after a retune, during cutscenes, or while the phone/TV
mutes the radio (`MUTE_*`).

### DJ/ads/news

Treat them as ordinary "tracks" if their ids are visible through the station current-track field
(probable, since `+0x190C` is written for whatever the stream slot plays - verify in E1). If so the same
mechanism covers them. If not, accept that they diverge and only sync the music, which is the user-visible
50 % of the experience.

### Risks

- **Unknown semantics of `FORCE_RADIO_TRACK` ints** and whether it accepts an offset. Fallback: force the
  track from the start and delay it by the expected offset-sleep (i.e. force at the next track boundary),
  accepting up to a song-length mismatch.
- **Track naming**: the native wants a metadata sound name; we must send names (string) or hashes, and clients
  need identical `sounds.dat15` (the hash check covers it).
- **Station state is mutated by FORCE**: freezing probably blocks the station from choosing the next track
  until `UNFREEZE`; if UNFREEZE restarts, we need to keep it frozen and advance manually (E4).
- **Code in the encrypted range** (`0x47C2E0/330/350`) takes part in selection and possibly in a "network game"
  check (`0x945AE0` calls `0x47C330`); under a network/session mode the radio might behave differently. We
  are not entering MP session mode, but if we ever do, re-test.
- **Cutscenes/FusionFix**: the radio resets across cutscenes; the owner's keyframe will repair it, but sync
  must be paused during cutscenes.
- **Downgrade mod unknowns**: it may change `rpf` names/layout; the hash recipe is by file content, not name.
- **Custom "User Music" station** (Windows Media library): per-machine tracks; exclude it from sync
  (fall back to a different station or leave local).
- **Audio threading**: reading the station struct from the game thread while the audio thread mutates it
  can tear (these are plain int fields; reading `+0x1908/+0x190C` twice and comparing is enough).

## 6. Experiments (logging only)

All use `native_hooks::on()/before()` for hooks and a once-per-second `script::on_tick` poll that **reads**
memory; nothing writes the game in E1-E2.

**E1 - what the station state looks like.** Every 1 s log: `GET_PLAYER_RADIO_STATION_INDEX`, station name,
`IS_RADIO_RETUNING`, and for the current station struct (pointer from `*(uint**)0x11D76AC` indexed by the
station index; guard with count byte `*(uint8_t*)0x11D74F1`, SEH-protected read) the dwords at
`+0x1904, +0x1908, +0x190C, +0x1910`, bytes `+0x1917, +0x1927`, floats at `+0x18FC`. Drive around, wait through
2-3 songs, a DJ line and an ad. Expected: `+0x190C` changes exactly at track boundaries; `+0x1908` shows the
previous.

**E2 - timeline model (the key one).** Log the same fields for **all** stations every second, not only the
tuned one. Steps: (1) tune station A for 90 s, (2) tune B for 5 min, (3) tune A again. If A's `+0x190C`
advanced while untuned, or A resumes mid-song at a position consistent with elapsed wall time -> virtual
timeline. If A restarts with a fresh pick -> per-tune state. (4) Reload the same savegame twice and compare
the first station/track after load: identical => deterministic seed; different => `rand()`-like.
Also tune the same station in two game instances started a few seconds apart (needs the second machine/
instance) and compare the track/time pairs.

**E3 - what `FORCE_RADIO_TRACK`'s ints do.** Add a debug key that calls
`FREEZE_RADIO_STATION(name)`; `FORCE_RADIO_TRACK(name, track, 1, 0)`, then `(…, 0, 0)`, `(…, 1, 30000)`,
`(…, 0, 30000)`; use the track name you logged in E1. Note in the log whether it starts at 0 or at ~30 s.
Also hook `FORCE_RADIO_TRACK` with `native_hooks::before` to capture the arguments real mission scripts
use (a few missions force tracks, e.g. radio tuned to a particular station during a drive).

**E4 - freeze semantics.** After E3, call `UNFREEZE_RADIO_STATION` and log whether the next song is chosen
normally or the track restarts.

**E5 - drift.** With the radio running, log `GetGameTimer` against the number of seconds into the track
(from E3-derived offset) over 10 minutes to see audio/clock drift.

Outcome mapping: E2 says "virtual timeline from a seed/clock" -> sync becomes "send the seed/clock once";
else the state-replication design above is the implementation. E3 decides whether mid-song joins are
possible.

## 7. Recommended next steps

1. Implement E1+E2 as a debug-only module (`src/game/radio_probe.*`) behind a config flag; run it, paste the
   log into this doc.
2. Implement the file-hash handshake (cheap, independent of the engine; section 4).
3. Station sync only (driver -> passengers) first; song/offset sync after E3.

## 8. Probe results (2026-10-26, run 1)

- **Every station plays all the time**, tuned or not (all 23 stations' slots advance and change items while
  untuned): a live timeline per station, not per-tune state.
- An item = (track-list metadata pointer, category byte, track number). Categories seen: 0, 1, 2 (music),
  5, 8, 10 (idle/off). Ads resolve to `RADIO_STATION_ADVERTS_GENERIC`; other list pointers are odd addresses
  inside one ~80 KB block (the loaded `game.dat16` metadata), so `pointer - lookup("RADIO_ADVERTS")` is a
  portable item id between machines with the same `game.dat16`.
- Runtime station names (`GET_RADIO_NAME`) are `RADIO_0`..`RADIO_22`, not the script names (`VLADIVOSTOK`),
  so name-based natives (`FREEZE_RADIO_STATION`, `FORCE_RADIO_TRACK`) can't be driven from the probe's names.
- **Play position: slot `+0x990`, ms**, rising 1000/s. `0xB76660` (load item) sets it to a random value
  (`0x885490`), which is why tuning in starts mid-song; `0xB76C70(slot, ms)` adds to it before the stream
  starts. Plan: load the item, overwrite `+0x990`, start (`0xB769A0`, `0xB76920`) - `start_item` in the probe.
- Each station has two slots: the current one plays, the other holds the next item (crossfade / DJ over intro).
