"""Runs the playlist missions unattended (story/mission_runner.cpp is the in-game half).

For each installed mission save (tools/mission_saves.py), it marks that save as the newest so the
game loads it on start, writes plugins/iv-coop/runner.txt, launches the game windowed (it stays
behind other windows and keeps running unfocused), and waits: the mod hosts a session, the test
bot joins, the mission is recorded and played through by the audit's auto-play, and the game
closes itself. Results collect in plugins/iv-coop/runner_result.txt, reports in audit/<script>.md,
recordings in runner-<script>.ivcrec, coverage in coverage/.

    python tools/mission_runner.py [script ...]      (default: every installed mission save)
"""

import json
import os
import re
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mission_saves  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GAME = r"C:\Program Files (x86)\Steam\steamapps\common\Grand Theft Auto IV\GTAIV"
PLUGINS = os.path.join(GAME, "plugins", "iv-coop")
BOT = os.path.join(ROOT, "build", "Debug", "iv-coop-bot.exe")
SECONDS = 900          # per mission, in game
LOAD_ALLOWANCE = 240   # launch + load + quit


def launch_args():
    """The command line the launcher used last time (from the mod's log), windowed."""
    log = os.path.join(GAME, "plugins", "iv-coop.log")
    args = None
    with open(log, encoding="utf-8", errors="replace") as f:
        for line in f:
            if "Command line:" in line:
                args = line.split("Command line:", 1)[1].strip()
                break
    if not args:
        sys.exit("no 'Command line:' in iv-coop.log; launch the game normally once first")
    exe = re.match(r'"([^"]+)"', args).group(1)
    rest = args[len(exe) + 2:].strip()
    return f'"{exe}" {rest} -windowed'


MAIN_SCRIPT = os.path.join(os.path.dirname(ROOT), "research", "GTAIVMissionScripts", "IV", "main.txt")


def contact_points():
    """script -> (x, y, z) of its contact blip, from the story flow's own tables in the decompiled main:
    sub_28480(contact, n, _, location, "M_n", ...,"Script") and sub_23830(location, x, y, z, ...).
    Location 50/51: none (main or a phone call starts the mission)."""
    if not os.path.exists(MAIN_SCRIPT):
        return {}
    text = open(MAIN_SCRIPT, encoding="latin1").read()
    where = {int(m.group(1)): (float(m.group(2)), float(m.group(3)), float(m.group(4))) for m in
             re.finditer(r"sub_23830\( *(\d+), *(-?[\d.]+), *(-?[\d.]+), *(-?[\d.]+),", text)}
    out = {}
    for m in re.finditer(r'sub_28480\( *\d+, *\d+, *\d+, *(\d+), *"M_\d+", *\d+, *\d+, *\d+, *"(\w+)"', text):
        loc, script = int(m.group(1)), m.group(2).lower()
        if loc in where:
            out[script] = where[loc]
    return out


def game_running():
    out = subprocess.run(["tasklist", "/FI", "IMAGENAME eq GTAIV.exe", "/NH"], capture_output=True, text=True).stdout
    return "GTAIV.exe" in out


def results():
    path = os.path.join(PLUGINS, "runner_result.txt")
    return open(path, encoding="utf-8").read().splitlines() if os.path.exists(path) else []


def run(script, save_file, cmd, at):
    prof = mission_saves.profile_dir()
    os.utime(os.path.join(prof, save_file))  # newest: the game loads it on start
    with open(os.path.join(PLUGINS, "runner.txt"), "w") as f:
        f.write(f"mission={script}\nseconds={SECONDS}\n")
        if at:
            f.write("at={:.2f},{:.2f},{:.2f}\n".format(*at))
    before = len(results())
    subprocess.Popen(cmd, cwd=GAME, creationflags=subprocess.CREATE_NO_WINDOW)
    deadline = time.time() + SECONDS + LOAD_ALLOWANCE
    started = False
    while time.time() < deadline:
        time.sleep(5)
        running = game_running()
        started |= running
        if started and not running:
            break
    else:
        subprocess.run(["taskkill", "/F", "/IM", "GTAIV.exe"], capture_output=True)
        print(f"{script}: timed out; game killed")
    for path in ("runner.txt", "runner.active"):
        p = os.path.join(PLUGINS, path)
        if os.path.exists(p):
            os.remove(p)
    new = results()[before:]
    print(f"{script}: " + (new[-1] if new else "no result (the game closed or crashed before finishing)"), flush=True)
    time.sleep(10)  # let the launcher settle before the next launch


def main():
    if game_running():
        sys.exit("close the game first")
    index_path = mission_saves.INDEX
    installed = json.load(open(index_path)) if os.path.exists(index_path) else {}
    by_script = {s: f for f, s in installed.items()}
    order = [s for s, *_ in mission_saves.SAVES if s in by_script]
    wanted = sys.argv[1:] or order
    cmd = launch_args()
    points = contact_points()
    bot = subprocess.Popen([BOT, "--fixed", "--speed", "0", "--radius", "4", "--retry"],
                           creationflags=subprocess.CREATE_NO_WINDOW)
    try:
        for script in wanted:
            if script not in by_script:
                print(f"{script}: no installed save (python tools/mission_saves.py install <batch>)")
                continue
            run(script, by_script[script], cmd, points.get(script))
    finally:
        bot.terminate()


if __name__ == "__main__":
    main()
