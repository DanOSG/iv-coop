"""Mission saves for testing: a save just before each mission on the docs/coverage.md playlist.

Starting a mission script directly skips the story flow (on-mission state, map unlocks), so
missions are played from real saves instead: GTASnP (gtasnp.com) user uploads, picked so the
save's last mission comes shortly before the target and its 100% checklist has the target
still to do. The game has 13 slots and the player's own saves use some, so they go in in
batches of up to 10 into the free slots; a batch only replaces files this tool installed.

    python tools/mission_saves.py list
    python tools/mission_saves.py install <batch number>    (1 = the first 10, 2 = the next 10, ...)

Saves are also kept in Documents/Rockstar Games/GTA IV/iv-coop-mission-saves/<script>.
"""

import json
import os
import sys
import time
import urllib.request

# script, mission, GTASnP id, last mission in that save (in playlist order; None: no save found).
SAVES = [
    ("finale2", "A Revenger's Tragedy", "EodbLb", "If the Price is Right"),  # Mr. & Mrs. Bellic comes first
    ("bell7", "To Live and Die in Alderney", "72U5kz", "Trespass"),
    ("ray6", "Late Checkout", "jfBEvS", "Weekend at Florian's"),
    ("packie3", "Three Leaf Clover", "koEhVx", "Waste Not Want Knots"),
    ("bell6", "Truck Hustle", "jKZOtJ", "Buoys Ahoy"),
    ("roman6", "Jamaican Heat", "mDUz3q", "Easy Fare"),
    ("gerry5", "Diamonds are a Girl's Best Friend", "EYRs54", "She's A Keeper"),
    ("faustin6", "The Master and the Molotov", "lEJcRP", "Rigged to Blow"),
    ("vlad3", "Clean Getaway", "iPEBOI", "Hung Out to Dry"),
    ("jacob2", "Shadow", "OEwLEO", "Logging On"),
    ("brucie4", "No. 1", "ivL69L", "...Out of the Closet"),
    ("jacob1", "Concrete Jungle", "3HK2k7", "Jamaican Heat"),
    ("jimmy1", "Pegorino's Pride", "SeEIya", "Truck Hustle"),
    ("manny1", "Escuela of the Streets", "CUQcHS", "No. 1"),
    ("elizabeta4", "Have a Heart", "pWOY4T", "The Snow Storm"),
    ("bell5", "Trespass", "jYg6Xk", "Catch The Wave"),
    ("dwayne3", "Undress to Kill", "VAntvn", "Ruff Rider"),
    ("packie1", "Harboring a Grudge", "6NGvhY", "Have a Heart"),
    ("roman11", "Roman's Sorrow", "5ly13a", "Russian Revolution"),
    ("francis1", "Call and Collect", "6CY2k2", "Hostile Negotiation"),
    ("francis5", "Blood Brothers", "JESFVg", "Tunnel of Death"),
    ("roman14", "That Special Someone", "kNJWvy", "...Pest Control"),
    ("roman5", "Easy Fare", "BAl1K7", "Bleed Out"),
    # roman1 (The Cousins Bellic): a new game. bell3m (High-End Assassin), hossan_1 (a random
    # character): no save picked.
]
BATCH = 10

DOCS = os.path.join(os.path.expanduser("~"), "Documents", "Rockstar Games", "GTA IV")
LIBRARY = os.path.join(DOCS, "iv-coop-mission-saves")
INDEX = os.path.join(LIBRARY, "installed.json")  # save file -> script, for files this tool put in


def profile_dir():
    profiles = os.path.join(DOCS, "Profiles")
    dirs = [d for d in os.listdir(profiles) if len(d) == 8 and os.path.isdir(os.path.join(profiles, d))]
    if len(dirs) != 1:
        sys.exit(f"expected one profile folder in {profiles}, found {dirs}")
    return os.path.join(profiles, dirs[0])


def download(save_id, slot):
    url = f"https://gtasnp.com/download/file/{save_id}?downgrade_version=0&slot={slot}"
    req = urllib.request.Request(url, headers={"User-Agent": "Mozilla/5.0 (iv-coop mission saves)"})
    data = urllib.request.urlopen(req, timeout=60).read()
    if data[12:16] != b"SAVE":
        sys.exit(f"{save_id}: not a GTA IV save")
    return data


def install(batch):
    prof = profile_dir()
    os.makedirs(LIBRARY, exist_ok=True)
    index = json.load(open(INDEX)) if os.path.exists(INDEX) else {}
    # Free slots: files that don't exist, or that this tool installed. GTASnP slot N = SGTA4{N-1:02}.
    slots = [n for n in range(1, 14)
             if not os.path.exists(os.path.join(prof, f"SGTA4{n - 1:02d}")) or f"SGTA4{n - 1:02d}" in index]
    todo = SAVES[(batch - 1) * BATCH:batch * BATCH]
    if not todo:
        sys.exit("no such batch")
    if len(slots) < len(todo):
        sys.exit(f"only {len(slots)} free slots")
    for name in [k for k in index]:  # take the previous batch out
        os.remove(os.path.join(prof, name)) if os.path.exists(os.path.join(prof, name)) else None
        del index[name]
    for (script, mission, save_id, after), slot in zip(todo, slots):
        name = f"SGTA4{slot - 1:02d}"
        data = download(save_id, slot)
        open(os.path.join(prof, name), "wb").write(data)
        os.makedirs(os.path.join(LIBRARY, script), exist_ok=True)
        open(os.path.join(LIBRARY, script, name), "wb").write(data)
        index[name] = script
        print(f"slot {slot:2} {name}: before {mission} ({script}), saved after {after}")
        time.sleep(1.5)
    json.dump(index, open(INDEX, "w"), indent=1)


def main():
    if len(sys.argv) < 2 or sys.argv[1] == "list":
        for i, (script, mission, save_id, after) in enumerate(SAVES):
            print(f"batch {i // BATCH + 1}  {script:<11} {mission:<36} after {after}  https://gtasnp.com/{save_id}")
    elif sys.argv[1] == "install":
        install(int(sys.argv[2]))


if __name__ == "__main__":
    main()
