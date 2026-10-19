# iv-coop

Free-roam co-op mod for GTA IV: The Complete Edition (Steam, `GTAIV.exe` 1.2.0.59).
See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md).

## Requirements

- Visual Studio 2022 with the "Desktop development with C++" workload (its bundled CMake is enough)
- GTA IV CE with [FusionFix](https://github.com/ThirteenAG/GTAIV.EFLC.FusionFix) installed (provides the ASI loader)

## Build

From a shell where `cmake` is the one bundled with VS (or open the folder in Visual Studio):

```
cmake --preset x86
cmake --build --preset debug
```

The post-build step copies `iv-coop.asi` and its `.pdb` into `<game>/plugins/`. If the game is running, the
loaded copy is renamed to `*.old` first, and the new build takes effect on the next launch. If the game is
installed somewhere else, set `-DIVCOOP_GAME_DIR=...` at configure time. Use `-DIVCOOP_DEPLOY=OFF` to skip the copy.

`tools/find_pattern.py` finds byte patterns in `GTAIV.exe` for static analysis (pair it with
`dumpbin /disasm /range:...`).

## Run

Launch the game normally. On first run the plugin writes `plugins/iv-coop.ini` with all settings and their
defaults. Check `plugins/iv-coop.log` to see what it did.

| Key | Action |
|-----|--------|
| F8 | Toggle the overlay |
| F7 | Toggle the running-scripts panel |
| F9 | Toggle the sync ghost (M1 loopback test: an MP-model ped mirrors you ~300 ms late, 2 m to your side) |

To freeze scripts by name for testing, use `[Scripts] Suppress=name1,name2` in the ini.
