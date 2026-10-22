"""Static mission analysis for docs/missions.md (M8 groundwork).

Reads the decompiled GTA IV / TLAD / TBoGT scripts from
github.com/0x52617665/GTAIVMissionScripts (public domain; clone it anywhere) and, for every
script that is a mission, lists what it does that co-op has to handle: entities and their
models, cutscenes, text blocks, scripted cameras, player-control locks, car recordings,
stealth/tail checks, timers, interiors, world changes, wanted levels, how it passes and
fails. Model hashes are resolved through the game's own .ide files.

    python tools/mission_analyser.py <GTAIVMissionScripts dir> <GTAIV game dir> [out.md]

Heuristic by design: it counts native calls and reads literal arguments, it does not
follow control flow. The co-op notes point at the rules in docs/ARCHITECTURE.md.
"""

import collections
import os
import re
import sys

EPISODES = [("IV", "IV", "common"), ("TLAD", "TLAD", "TLAD/common"), ("TBOGT", "TBoGT", "TBoGT/common")]

CALL = re.compile(r"\b([A-Z][A-Z0-9_]{2,})\s*\(")
STRARG = r'\(\s*"([^"]+)"'


def joaat(s):
    h = 0
    for ch in s.lower().encode():
        h = (h + ch) & 0xFFFFFFFF
        h = (h + (h << 10)) & 0xFFFFFFFF
        h ^= h >> 6
    h = (h + (h << 3)) & 0xFFFFFFFF
    h ^= h >> 11
    return (h + (h << 15)) & 0xFFFFFFFF


def load_models(game, data_dirs):
    """hash -> (name, kind) from vehicles.ide (kind: car/bike/boat/heli/plane/train) and peds.ide."""
    models = {}
    for d in data_dirs:
        for fname, is_vehicle in (("vehicles.ide", True), ("peds.ide", False)):
            path = os.path.join(game, d, "data", fname)
            if not os.path.exists(path):
                continue
            for line in open(path, encoding="latin1"):
                parts = [p.strip() for p in line.split(",")]
                if len(parts) < 3 or parts[0].startswith("#") or not parts[0]:
                    continue
                name = parts[0].lstrip("#").strip()
                kind = parts[2].lower() if is_vehicle else "ped"
                if is_vehicle and kind not in ("car", "bike", "boat", "heli", "plane", "train"):
                    continue
                models[joaat(name)] = (name, kind)
    return models


def first_int_arg(text, native):
    out = []
    for m in re.finditer(native + r"\(\s*(-?\d+)\s*,", text):
        out.append(int(m.group(1)) & 0xFFFFFFFF)
    return out


def analyse(path, models):
    text = open(path, encoding="latin1").read()
    calls = collections.Counter(CALL.findall(text))
    c = lambda *names: sum(calls[n] for n in names)
    r = {"calls": calls}
    r["mission"] = bool(c("REGISTER_MISSION_PASSED", "PLAYSTATS_MISSION_PASSED"))
    r["mission_flag"] = "SET_MISSION_FLAG( 1 )" in text
    r["cutscenes"] = sorted(set(re.findall(r"START_CUTSCENE_NOW" + STRARG, text)))
    # Text blocks: REQUEST_ADDITIONAL_TEXT literals, or the usual helper sub_N("NAME", slot).
    blocks = set(re.findall(r"REQUEST_ADDITIONAL_TEXT\(\s*\"([A-Za-z0-9_]+)\"", text))
    blocks |= set(re.findall(r"sub_\d+\(\s*\"([A-Za-z0-9_]+)\"\s*,\s*[0-9]\s*\)", text))
    r["text_blocks"] = sorted(blocks)
    r["ipls"] = sorted(set(re.findall(r"(?:REQUEST_IPL|REMOVE_IPL)" + STRARG, text)))
    vehicles, peds = collections.Counter(), collections.Counter()
    for h in first_int_arg(text, "CREATE_CAR") + first_int_arg(text, "REQUEST_MODEL"):
        name, kind = models.get(h, (None, None))
        if kind and kind != "ped":
            vehicles[f"{name} ({kind})"] += 1
    for native, idx in (("CREATE_CHAR", 1),):
        for m in re.finditer(native + r"\(\s*-?\d+\s*,\s*(-?\d+)\s*,", text):
            h = int(m.group(1)) & 0xFFFFFFFF
            if h in models and models[h][1] == "ped":
                peds[models[h][0]] += 1
    r["vehicles"], r["peds"] = vehicles, peds
    r["vehicle_kinds"] = sorted({k.split("(")[1].rstrip(")") for k in vehicles})
    r["entities"] = c("CREATE_CHAR", "CREATE_CHAR_INSIDE_CAR", "CREATE_CHAR_AS_PASSENGER", "CREATE_CAR",
                      "CREATE_OBJECT", "CREATE_OBJECT_NO_OFFSET")
    r["cams"] = c("CREATE_CAM")
    r["control_off"] = c("SET_PLAYER_CONTROL", "SET_PLAYER_CONTROL_ADVANCED")
    r["fades"] = c("DO_SCREEN_FADE_OUT")
    r["recordings"] = c("START_PLAYBACK_RECORDED_CAR", "START_PLAYBACK_RECORDED_CAR_USING_AI",
                        "START_PLAYBACK_RECORDED_CAR_WITH_OFFSET", "START_PLAYBACK_RECORDED_CAR_LOOPED")
    r["stealth"] = c("HAS_CHAR_SPOTTED_CHAR", "HAS_CHAR_SPOTTED_CHAR_IN_FRONT", "CAN_CHAR_SEE_DEAD_CHAR",
                     "IS_CHAR_IN_ANGLED_AREA_2D", "IS_PLAYER_BEING_ARRESTED")
    # IV draws mission timers/counters/bars itself every frame (the engine's on-screen
    # timer is used once in all scripts), so look for script-drawn HUD.
    r["timers"] = c("DISPLAY_TEXT", "DISPLAY_TEXT_WITH_NUMBER", "DISPLAY_TEXT_WITH_FLOAT",
                    "DISPLAY_TEXT_WITH_LITERAL_STRING", "DRAW_RECT", "DRAW_SPRITE", "DRAW_CURVED_WINDOW",
                    "CLEAR_ONSCREEN_TIMER", "CLEAR_ONSCREEN_COUNTER")
    r["interiors"] = c("ACTIVATE_INTERIOR", "REQUEST_INTERIOR_MODELS", "GET_INTERIOR_FROM_CHAR",
                       "SET_ROOM_FOR_CHAR_BY_NAME", "SET_ROOM_FOR_CHAR_BY_KEY")
    r["world"] = c("SWITCH_ROADS_OFF", "SWITCH_ROADS_ON", "SWITCH_PED_PATHS_OFF", "SWITCH_PED_ROADS_OFF",
                   "SET_STATE_OF_CLOSEST_DOOR_OF_TYPE", "SWAP_NEAREST_BUILDING_MODEL", "REQUEST_IPL", "REMOVE_IPL",
                   "SET_ZONE_NO_COPS", "ENABLE_SAVE_HOUSE")
    r["wanted"] = c("ALTER_WANTED_LEVEL", "ALTER_WANTED_LEVEL_NO_DROP", "SET_MAX_WANTED_LEVEL")
    r["weapons"] = c("GIVE_WEAPON_TO_CHAR")
    r["player_warps"] = c("WARP_CHAR_INTO_CAR", "WARP_CHAR_INTO_CAR_AS_PASSENGER", "SET_CHAR_COORDINATES",
                          "WARP_CHAR_FROM_CAR_TO_COORD")
    r["car_blips"] = c("ADD_BLIP_FOR_CAR")
    r["conversations"] = c("NEW_SCRIPTED_CONVERSATION", "ADD_LINE_TO_CONVERSATION", "START_SCRIPT_CONVERSATION")
    r["phone"] = c("NEW_MOBILE_PHONE_CALL", "START_MOBILE_PHONE_CALL", "IS_MOBILE_PHONE_CALL_ONGOING")
    r["payout"] = c("ADD_SCORE")
    r["deatharrest"] = c("HAS_DEATHARREST_EXECUTED")
    r["achievements"] = sorted(set(re.findall(r"AWARD_ACHIEVEMENT\(\s*(\d+)", text)))
    r["news"] = c("UNLOCK_MISSION_NEWS_STORY")
    r["minigame"] = c("IS_MINIGAME_IN_PROGRESS", "SET_MINIGAME_IN_PROGRESS")
    r["lines"] = text.count("\n")
    return r


def coop_notes(r):
    n = []
    if r["cutscenes"]:
        n.append(f"{len(r['cutscenes'])} cutscene(s): replicate start/skip; partners hidden (M7, free-cutscene option)")
    if r["cams"] and r["control_off"]:
        n.append("scripted camera scenes with player control off: stream the camera to joiners or let them keep control")
    if r["car_blips"] and r["vehicles"]:
        n.append("mission gives/marks vehicles: apply the mission-provided-vehicle duplicate rule")
    single = [k for k in r["vehicle_kinds"] if k in ("bike", "heli", "boat", "plane")]
    if single:
        n.append(f"uses {', '.join(single)}: limited seats; partners may need their own vehicle")
    if r["recordings"]:
        n.append(f"{r['recordings']} car recording(s) (scripted chase/drive): partners follow in their own or a duplicate car")
    if r["stealth"]:
        n.append("stealth/spotting checks: partners can blow cover (decide per mission: allowed fail or partner-ignored)")
    if r["timers"]:
        n.append("script-drawn HUD (timer/counter/bar/text drawn each frame): not covered by text sync; replicate the drawing or its values")
    if r["interiors"]:
        n.append("interiors: check partners can enter with the host (room/interior state)")
    if r["world"]:
        n.append("changes world state (roads/doors/IPLs/buildings): covered by the world ledger")
    if r["wanted"]:
        n.append("mission-imposed wanted level: applies to every player")
    if r["fades"] and r["player_warps"]:
        n.append("fades + player warps: host teleported; joiners get the 'join host' option")
    if r["minigame"]:
        n.append("minigame: host plays, partners spectate")
    if r["phone"] or r["conversations"]:
        n.append("calls dialogue natives directly (most missions go through speechcontrol)")
    if not r["deatharrest"]:
        n.append("no death/arrest handler: check how a host death is handled")
    return n


def main():
    scripts, game = sys.argv[1], sys.argv[2]
    out = sys.argv[3] if len(sys.argv) > 3 else os.path.join(os.path.dirname(__file__), "..", "docs", "missions.md")
    models = load_models(game, [d for _, _, d in EPISODES])
    results = []
    totals = collections.Counter()
    for ep, folder, _ in EPISODES:
        d = os.path.join(scripts, folder)
        for f in sorted(os.listdir(d)):
            r = analyse(os.path.join(d, f), models)
            totals[ep] += 1
            if r["mission"]:
                results.append((ep, f[:-4], r))

    L = []
    L.append("# Mission audit (static)\n")
    L.append("Generated by `tools/mission_analyser.py` from the decompiled scripts in "
             "[GTAIVMissionScripts](https://github.com/0x52617665/GTAIVMissionScripts) (public domain). "
             "Counts are native calls in the script text (all branches), not runtime behaviour. "
             "Co-op notes point at the rules in [ARCHITECTURE.md](ARCHITECTURE.md).\n")
    by_ep = collections.Counter(ep for ep, _, _ in results)
    L.append("Missions found: " + ", ".join(f"{ep} {by_ep[ep]} (of {totals[ep]} scripts)" for ep, _, _ in EPISODES) + "\n")
    L.append("## Summary\n")
    L.append("| Episode | Mission | Cutscenes | Cams | Vehicles | Chase rec. | Stealth | Drawn HUD | Interior | World | Wanted | Payout |")
    L.append("|---|---|---|---|---|---|---|---|---|---|---|---|")
    yn = lambda v: str(v) if v else ""
    for ep, name, r in results:
        kinds = "/".join(r["vehicle_kinds"])
        mark = "" if r["mission_flag"] else " *"
        L.append(f"| {ep} | [{name}](#{ep.lower()}-{name.lower()}){mark} | {yn(len(r['cutscenes']))} | {yn(r['cams'])} | {kinds} | "
                 f"{yn(r['recordings'])} | {yn(r['stealth'])} | {yn(r['timers'])} | {yn(r['interiors'])} | "
                 f"{yn(r['world'])} | {yn(r['wanted'])} | {'yes' if r['payout'] else ''} |")
    L.append("")
    for ep, name, r in results:
        L.append(f"## {ep} {name}\n")
        L.append(f"- Script size: {r['lines']} lines; entities created: {r['entities']}; pass: "
                 f"{'REGISTER_MISSION_PASSED' if r['calls']['REGISTER_MISSION_PASSED'] else 'PLAYSTATS_MISSION_PASSED (stranger/side)'}"
                 f"; death/arrest handler: {'yes' if r['deatharrest'] else 'no'}")
        if r["cutscenes"]:
            L.append(f"- Cutscenes: {', '.join(r['cutscenes'])}")
        if r["text_blocks"]:
            L.append(f"- Text blocks: {', '.join(r['text_blocks'])}")
        if r["vehicles"]:
            L.append("- Vehicles: " + ", ".join(f"{k}{' x' + str(v) if v > 1 else ''}" for k, v in r["vehicles"].most_common()))
        if r["peds"]:
            L.append("- Peds: " + ", ".join(f"{k}{' x' + str(v) if v > 1 else ''}" for k, v in r["peds"].most_common(15)))
        if r["ipls"]:
            L.append(f"- IPLs: {', '.join(r['ipls'])}")
        extras = []
        for key, label in (("cams", "script cameras"), ("control_off", "player-control switches"), ("fades", "fades"),
                           ("recordings", "car recordings"), ("stealth", "spotting checks"), ("timers", "script-drawn HUD calls"),
                           ("interiors", "interior calls"), ("world", "world-state calls"), ("wanted", "wanted-level calls"),
                           ("weapons", "weapons given"), ("player_warps", "warps/teleports"), ("news", "news unlocks")):
            if r[key]:
                extras.append(f"{label} {r[key]}")
        if r["achievements"]:
            extras.append(f"achievements {', '.join(r['achievements'])}")
        if extras:
            L.append("- Also: " + ", ".join(extras))
        notes = coop_notes(r)
        if notes:
            L.append("- **Co-op:**")
            L.extend(f"  - {x}" for x in notes)
        L.append("")
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    open(out, "w", encoding="utf-8").write("\n".join(L))
    print(f"{len(results)} missions -> {out}", dict(by_ep))


if __name__ == "__main__":
    main()
