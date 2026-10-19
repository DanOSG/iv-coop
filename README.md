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

The post-build step copies `iv-coop.asi` and its `.pdb` into `<game>/plugins/`. If the game is installed
somewhere else, set `-DIVCOOP_GAME_DIR=...` at configure time. Use `-DIVCOOP_DEPLOY=OFF` to skip the copy.

## Run

Launch the game normally. On first run the plugin writes `plugins/iv-coop.ini`. Check `plugins/iv-coop.log`
to see what it did. **F8** toggles the overlay.
