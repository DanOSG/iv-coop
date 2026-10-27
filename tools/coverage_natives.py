"""Sync coverage: what story scripts do that co-op doesn't sync yet.

One list of natives, grouped by what they change, used two ways:

  - the mod hooks every one of them and counts the calls each script makes while you play
    (story/coverage.cpp, from the generated src/story/coverage_natives.inc);
  - this script counts them in the decompiled mission scripts, ranks the missions by how
    many kinds of unsynced behaviour they use, and picks a short playlist that covers
    every kind (docs/coverage.md, and playlist.txt for the Ctrl+Shift+M mission launcher).

    python tools/coverage_natives.py <natives.ixx from FusionFix> <GTAIVMissionScripts dir> [game plugins dir]

Natives the mod already syncs (blips, text, conversations, cutscenes, world ledger, entity
creation) are not in the list; when one of these categories gets synced, remove it here.
"""

import collections
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

CATEGORIES = {
    "npc_anims": ["TASK_PLAY_ANIM", "TASK_PLAY_ANIM_FACIAL", "TASK_PLAY_ANIM_NON_INTERRUPTABLE",
                  "TASK_PLAY_ANIM_SECONDARY", "TASK_PLAY_ANIM_SECONDARY_IN_CAR",
                  "TASK_PLAY_ANIM_SECONDARY_NO_INTERRUPT", "TASK_PLAY_ANIM_SECONDARY_UPPER_BODY",
                  "TASK_PLAY_ANIM_WITH_ADVANCED_FLAGS", "TASK_PLAY_ANIM_WITH_FLAGS",
                  "TASK_PLAY_ANIM_WITH_FLAGS_AND_START_PHASE", "TASK_SIT_DOWN", "TASK_SIT_DOWN_INSTANTLY",
                  "TASK_SIT_DOWN_ON_NEAREST_OBJECT", "TASK_SIT_DOWN_ON_OBJECT", "TASK_SIT_DOWN_ON_SEAT",
                  "TASK_SIT_DOWN_PLAY_ANIM", "TASK_START_SCENARIO_IN_PLACE", "TASK_USE_NEAREST_SCENARIO_TO_POS",
                  "TASK_USE_NEAREST_SCENARIO_TO_POS_WARP", "TASK_HANDS_UP", "TASK_COWER", "TASK_CHAT_WITH_CHAR",
                  "TASK_PICKUP_AND_CARRY_OBJECT", "TASK_COMPLEX_PICKUP_OBJECT", "SET_CHAR_ANIM_CURRENT_TIME",
                  "SET_CHAR_ANIM_SPEED", "SET_ANIM_GROUP_FOR_CHAR", "BLEND_FROM_NM_WITH_ANIM"],
    "attachments": ["ATTACH_CAR_TO_CAR", "ATTACH_CAR_TO_CAR_PHYSICALLY", "ATTACH_CAR_TO_OBJECT",
                    "ATTACH_CAR_TO_OBJECT_PHYSICALLY", "ATTACH_OBJECT_TO_CAR", "ATTACH_OBJECT_TO_CAR_PHYSICALLY",
                    "ATTACH_OBJECT_TO_OBJECT_PHYSICALLY", "ATTACH_OBJECT_TO_PED", "ATTACH_OBJECT_TO_PED_PHYSICALLY",
                    "ATTACH_PED_TO_CAR", "ATTACH_PED_TO_CAR_PHYSICALLY", "ATTACH_PED_TO_OBJECT",
                    "ATTACH_PED_TO_OBJECT_PHYSICALLY", "ATTACH_PED_TO_WORLD_PHYSICALLY", "DETACH_CAR",
                    "DETACH_OBJECT_NO_COLLIDE", "DETACH_PED", "DETACH_PED_FROM_WITHIN_CAR"],
    "explosions_fire": ["ADD_EXPLOSION", "ADD_EXPLOSION_WITH_DIRECTION", "EXPLODE_CAR", "EXPLODE_CHAR_HEAD",
                        "START_SCRIPT_FIRE", "START_CAR_FIRE", "START_CHAR_FIRE", "START_OBJECT_FIRE",
                        "FIRE_SINGLE_BULLET", "FIRE_PED_WEAPON"],
    "particles": ["START_PTFX", "START_PTFX_ON_PED", "START_PTFX_ON_PED_BONE", "START_PTFX_ON_VEH",
                  "START_PTFX_ON_OBJ_BONE", "TRIGGER_PTFX", "TRIGGER_PTFX_ON_PED_BONE", "TRIGGER_PTFX_ON_VEH",
                  "TRIGGER_PTFX_ON_OBJ", "TRIGGER_PTFX_ON_OBJ_BONE", "SMASH_GLASS_ON_OBJECT"],
    "sounds": ["PLAY_SOUND", "PLAY_SOUND_FRONTEND", "PLAY_SOUND_FROM_POSITION", "PLAY_SOUND_FROM_PED",
               "PLAY_SOUND_FROM_VEHICLE", "PLAY_SOUND_FROM_OBJECT", "PLAY_AUDIO_EVENT", "PLAY_AUDIO_EVENT_FROM_PED",
               "PLAY_AUDIO_EVENT_FROM_VEHICLE", "PLAY_AUDIO_EVENT_FROM_OBJECT", "PLAY_STREAM_FRONTEND",
               "PLAY_STREAM_FROM_OBJECT", "PLAY_FIRE_SOUND_FROM_POSITION", "TRIGGER_MISSION_COMPLETE_AUDIO"],
    "cameras_fades": ["CREATE_CAM", "SET_CAM_ACTIVE", "ACTIVATE_SCRIPTED_CAMS", "ATTACH_CAM_TO_PED",
                      "ATTACH_CAM_TO_VEHICLE", "ATTACH_CAM_TO_OBJECT", "POINT_CAM_AT_COORD", "POINT_CAM_AT_PED",
                      "POINT_CAM_AT_VEHICLE", "POINT_CAM_AT_OBJECT", "SET_CAM_SHAKE", "DO_SCREEN_FADE_OUT"],
    "player_control": ["SET_PLAYER_CONTROL", "SET_PLAYER_CONTROL_ADVANCED", "SET_PLAYER_CONTROL_FOR_AMBIENT_SCRIPT"],
    "vehicle_state": ["OPEN_CAR_DOOR", "SHUT_CAR_DOOR", "CONTROL_CAR_DOOR", "BREAK_CAR_DOOR", "CLOSE_ALL_CAR_DOORS",
                      "LOCK_CAR_DOORS", "SMASH_CAR_WINDOW", "BURST_CAR_TYRE", "SET_CAR_HEALTH", "SET_ENGINE_HEALTH",
                      "FIX_CAR", "SWITCH_CAR_SIREN", "FORCE_CAR_LIGHTS", "SET_CAR_LIGHTS_ON", "SET_VEH_HAZARDLIGHTS",
                      "SET_VEH_INDICATORLIGHTS", "SET_TAXI_LIGHTS", "SET_CAR_LIVERY", "SET_EXTRA_CAR_COLOURS",
                      "FREEZE_CAR_POSITION", "APPLY_FORCE_TO_CAR", "START_PLAYBACK_RECORDED_CAR",
                      "START_PLAYBACK_RECORDED_CAR_USING_AI"],
    "ped_looks": ["SET_CHAR_COMPONENT_VARIATION", "SET_CHAR_DEFAULT_COMPONENT_VARIATION",
                  "SET_CHAR_RANDOM_COMPONENT_VARIATION", "SET_CHAR_PROP_INDEX", "SET_CHAR_PROP_INDEX_TEXTURE",
                  "CLEAR_ALL_CHAR_PROPS", "FREEZE_CHAR_POSITION", "APPLY_FORCE_TO_PED"],
    "object_state": ["SET_OBJECT_VISIBLE", "SET_OBJECT_DYNAMIC", "SET_OBJECT_COLLISION", "SET_OBJECT_LIGHTS",
                     "APPLY_FORCE_TO_OBJECT", "PLAY_OBJECT_ANIM", "FREEZE_OBJECT_POSITION"],
    "pickups": ["CREATE_PICKUP", "CREATE_PICKUP_ROTATE", "CREATE_PICKUP_WITH_AMMO", "REMOVE_PICKUP"],
    "drunk_effects": ["SET_PED_IS_DRUNK", "SET_DRUNK_CAM", "SET_TIME_SCALE", "SHAKE_PAD"],
    "minigames": ["SET_MINIGAME_IN_PROGRESS", "SET_USE_POOL_GAME_PHYSICS_SETTINGS"],
    "area_clears": ["CLEAR_AREA", "CLEAR_AREA_OF_CARS", "CLEAR_AREA_OF_COPS", "CLEAR_AREA_OF_OBJECTS"],
}

# Friend activities and minigames aren't missions (no REGISTER_MISSION_PASSED) but need the
# same treatment; listed separately.
ACTIVITY = re.compile(r"^(roman|jacob|brucie|dwayne|packie)(darts|drink|eat|pool|show|strip|tenpin)$|"
                      r"^(tenpinbowl|bowling_lane|bowl_trigger|pool_game|pool_table|darts|darts_launcher|"
                      r"go_on_date|date_activity|date_food|amb\w+|helicopter|manhat_heli_tours)$")

CALL = re.compile(r"\b([A-Z][A-Z0-9_]{2,})\s*\(")


def read_hashes(ixx):
    out = {}
    for m in re.finditer(r"^\s+([A-Z][A-Z0-9_]+) = (0x[0-9A-F]{8}),", open(ixx, encoding="utf-8").read(), re.M):
        out.setdefault(m.group(1), int(m.group(2), 16))
    return out


def write_inc(hashes):
    path = os.path.join(ROOT, "src", "story", "coverage_natives.inc")
    with open(path, "w", newline="\n") as f:
        f.write("// Generated by tools/coverage_natives.py; edit the list there.\n")
        f.write("// {hash, native, category}: story natives co-op doesn't sync yet.\n")
        for cat, names in CATEGORIES.items():
            for n in names:
                f.write(f'{{0x{hashes[n]:08X}, "{n}", "{cat}"}},\n')
    return path


def scan(scripts_dir, known):
    """script -> (is_mission, Counter(category -> calls), Counter(native -> calls), {world-changing natives})"""
    native_cat = {n: c for c, ns in CATEGORIES.items() for n in ns}
    res = {}
    d = os.path.join(scripts_dir, "IV")
    for fname in sorted(os.listdir(d)):
        text = open(os.path.join(d, fname), encoding="latin1").read()
        calls = collections.Counter(CALL.findall(text))
        is_mission = bool(calls["REGISTER_MISSION_PASSED"] or calls["PLAYSTATS_MISSION_PASSED"])
        cats, natives = collections.Counter(), collections.Counter()
        for n, k in calls.items():
            if n in native_cat:
                cats[native_cat[n]] += k
                natives[n] += k
        actions = {n for n in calls if n in known and not NOT_ACTIONS.match(n)}
        res[fname[:-4]] = (is_mission, cats, natives, actions)
    return res


# Not worth sync work: queries, text/blips (synced), maths, debug widgets, stats, streaming.
NOT_ACTIONS = re.compile(r"^(IS_|HAS_|GET_|DOES_|ARE_|CAN_|WAS_|LOCATE_|REQUEST_|MARK_|LOAD_|PRINT|CLEAR_PRINTS|CLEAR_HELP|"
                         r"DISPLAY_TEXT|SET_TEXT_|DRAW_|ADD_BLIP|REMOVE_BLIP|CHANGE_BLIP|TIMER|WAIT|COS$|SIN$|ASIN|ABS|"
                         r"SQRT|VDIST|ATAN|TO_FLOAT|FLOOR|ROUND|GENERATE_RANDOM|STRING|COMPARE|ALLOCATE|SETTIMER|"
                         r"SET_MISSION_FLAG|USE_PREVIOUS|THIS_SCRIPT|TERMINATE|.*WIDGET|.*DEBUG|.*_STAT$|PLAYSTATS|"
                         r"REGISTER_.*STAT|CLEAR_BIT|SET_BIT|INCREMENT_|.*TXD|RELEASE_TEXTURE)")
SKIP_MISSIONS = {"endcreditroll", "multitutorial"}  # credits; the multiplayer tutorial


def playlist(res, names, min_gain=5):
    """Greedy: keep adding the mission that uses the most world-changing natives no picked mission uses yet,
    while it brings at least `min_gain` (the long tail is one rare call per mission)."""
    use = {n: res[n][3] for n in names if n not in SKIP_MISSIONS}
    freq = collections.Counter(c for s in use.values() for c in s)
    left, picked = set(freq), []
    while left:
        best = max(use, key=lambda m: len(use[m] & left))
        gain = sorted(use[best] & left, key=lambda c: (freq[c], c))
        if len(gain) < min_gain:
            break
        picked.append((best, gain, freq))
        left -= set(gain)
    return picked, left


def main():
    ixx, scripts = sys.argv[1], sys.argv[2]
    plugins = sys.argv[3] if len(sys.argv) > 3 else None
    hashes = read_hashes(ixx)
    missing = [n for ns in CATEGORIES.values() for n in ns if n not in hashes]
    if missing:
        sys.exit("not in the native list: " + ", ".join(missing))
    print("wrote", write_inc(hashes))

    res = scan(scripts, set(hashes))
    missions = [n for n, r in res.items() if r[0] and not ACTIVITY.match(n)]
    activities = [n for n in res if ACTIVITY.match(n)]
    cats = list(CATEGORIES)
    picked, rest = playlist(res, missions)
    pl = [n for n, _, _ in picked]
    # One of each friend activity / minigame (they need a friend or a venue, so they may not start cold).
    pl_act = ["romantenpin", "jacobdarts", "packiepool", "romandrink", "romaneat", "romanshow", "romanstrip",
              "go_on_date"]

    L = ["# Sync coverage (static)\n",
         "Generated by `tools/coverage_natives.py`. Counts are calls in the decompiled script text (all branches) of "
         "natives co-op doesn't sync yet, by category. The mod counts the same natives while you play "
         "(`plugins/iv-coop/coverage/`), which shows what really runs.\n",
         "## Playlist\n",
         "The fewest missions that between them use every category at least twice (Ctrl+Shift+M starts them in "
         "this order on a throwaway save):\n"]
    for n in pl:
        L.append(f"- **{n}**: " + ", ".join(f"{c} {res[n][1][c]}" for c in cats if res[n][1][c]))
    L.append("\nFriend activities and minigames covering every category they use:\n")
    for n in pl_act:
        L.append(f"- **{n}**: " + ", ".join(f"{c} {res[n][1][c]}" for c in cats if res[n][1][c]))
    for title, names in (("Missions", missions), ("Friend activities, minigames, ambient", activities)):
        L.append(f"\n## {title}\n")
        L.append("| Script | " + " | ".join(cats) + " |")
        L.append("|---|" + "---|" * len(cats))
        for n in sorted(names, key=lambda n: -len(res[n][1])):
            L.append(f"| {n} | " + " | ".join(str(res[n][1][c] or "") for c in cats) + " |")
    L.append("\n## Natives\n")
    for c, ns in CATEGORIES.items():
        L.append(f"- **{c}**: " + ", ".join(ns))
    doc = os.path.join(ROOT, "docs", "coverage.md")
    open(doc, "w", newline="\n").write("\n".join(L) + "\n")
    print("wrote", doc)
    print("playlist:", " ".join(pl))
    print("activities:", " ".join(pl_act))
    if plugins:
        path = os.path.join(plugins, "iv-coop", "playlist.txt")
        with open(path, "w", newline="\n") as f:
            f.write("# Ctrl+Shift+M starts the next of these (docs/coverage.md says why each is here).\n")
            f.write("# Reorder or add any mission/activity script name; done ones go to playlist_done.txt.\n")
            for n in pl + pl_act:
                f.write(n + "\n")
        print("wrote", path)


if __name__ == "__main__":
    main()
