"""Unattended episode self-test (src/story/self_test.h).

Writes plugins/iv-coop/selftest.txt, launches the game windowed behind other windows (the command line the launcher
used last time, as tools/mission_runner.py does), and starts or stops the test bot whenever the game asks for it in
selftest_state.txt ("bot=none", "bot=join", "bot=serve <episode>"). Results are printed as they come
(selftest_result.txt); the game closes itself after the last episode.

    python tools/self_test.py [episodes, default 0 1 2]
"""

import os
import signal
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mission_runner import BOT, GAME, PLUGINS, game_running, launch_args  # noqa: E402

TIMEOUT = 40 * 60
STATE = os.path.join(PLUGINS, "selftest_state.txt")
RESULTS = os.path.join(PLUGINS, "selftest_result.txt")
BOT_LOG = os.path.join(PLUGINS, "selftest_bot.log")


def read(path):
    try:
        return open(path, encoding="utf-8").read().splitlines()
    except OSError:
        return []


def main():
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    clear = 0  # a previous run's game (or its launcher) may still be closing: wait for 5 clear seconds in a row
    for _ in range(60):
        busy = subprocess.run(["tasklist", "/NH"], capture_output=True, text=True).stdout
        clear = 0 if ("GTAIV.exe" in busy or "PlayGTAIV.exe" in busy) else clear + 1
        if clear >= 5:
            break
        time.sleep(1)
    else:
        sys.exit("close the game first")
    episodes = [int(a) for a in sys.argv[1:]] or [0, 1, 2]
    for path in (STATE, os.path.join(PLUGINS, "selftest.active")):
        if os.path.exists(path):
            os.remove(path)
    with open(os.path.join(PLUGINS, "selftest.txt"), "w") as f:
        f.write("episodes=" + ",".join(map(str, episodes)) + "\n")
    shown = len(read(RESULTS))
    bot, bot_state = None, "none"
    log = open(BOT_LOG, "a")
    subprocess.Popen(launch_args(), cwd=GAME, creationflags=subprocess.CREATE_NO_WINDOW)
    deadline, started = time.time() + TIMEOUT, False
    try:
        while time.time() < deadline:
            time.sleep(1)
            for line in read(RESULTS)[shown:]:
                print(line, flush=True)
                shown += 1
            state = next((l[4:] for l in read(STATE) if l.startswith("bot=")), "none")
            if state != bot_state:
                if bot:
                    bot.terminate()
                    bot.wait()
                    bot = None
                args = None
                if state == "join":
                    args = ["--fixed", "--speed", "0", "--radius", "4", "--retry"]
                elif state.startswith("serve "):
                    args = ["--serve", "--retry", "--episode", state.split()[1]]
                if args:
                    log.write(f"--- {time.strftime('%H:%M:%S')} bot {' '.join(args)}\n")
                    log.flush()
                    bot = subprocess.Popen([BOT] + args, stdout=log, stderr=subprocess.STDOUT,
                                           creationflags=subprocess.CREATE_NO_WINDOW)
                print(f"[driver] bot={state}", flush=True)
                bot_state = state
            running = game_running()
            started |= running
            if started and not running:
                break
        else:
            subprocess.run(["taskkill", "/F", "/IM", "GTAIV.exe"], capture_output=True)
            print("[driver] timed out; game killed", flush=True)
    finally:
        if bot:
            bot.terminate()
        for line in read(RESULTS)[shown:]:
            print(line, flush=True)
        for name in ("selftest.txt", "selftest.active"):
            p = os.path.join(PLUGINS, name)
            if os.path.exists(p):
                os.remove(p)
    print("[driver] done", flush=True)


if __name__ == "__main__":
    main()
