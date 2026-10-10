# Engine Simulator

![Engine Simulator](docs/public/screenshots/screenshot_v01.png?raw=true)

A real-time internal combustion engine simulation built to produce engine audio and engine response. It isn't an engineering tool, so don't expect accurate figures for engine design or tuning.

This fork of [ange-yaghi/engine-sim](https://github.com/ange-yaghi/engine-sim) adds **`engine-sim-export`**, a command-line tool that renders game-ready engine sound sets. Upstream development continues as the [Community Edition](https://github.com/Engine-Simulator/engine-sim-community-edition).

## Building (Windows)

Requirements:

- Visual Studio 2022 with the *Desktop development with C++* workload
- CMake 3.21 or newer
- Git
- [vcpkg](https://github.com/microsoft/vcpkg)
- Flex and Bison: `winget install WinFlexBison.win_flex_bison`

Steps:

```
git clone --recurse-submodules https://github.com/forfex/engine-sim
cd engine-sim

git clone https://github.com/microsoft/vcpkg ..\vcpkg
..\vcpkg\bootstrap-vcpkg.bat
set VCPKG_ROOT=%CD%\..\vcpkg

cmake --preset windows-vcpkg
cmake --build --preset release
```

The first configure step installs SDL2, SDL2_image, Boost, libvorbis and LAME from `vcpkg.json`, so it takes a few minutes. It also applies the patches in `patches/` to the submodules.

The build writes these to `build\Release\`:

| Executable | Purpose |
| --- | --- |
| `engine-sim-app.exe` | Interactive simulator |
| `engine-sim-export.exe` | Game audio exporter |
| `engine-sim-game-audio-test.exe` | Exporter tests |
| `engine-sim-test.exe` | Upstream simulator tests (some already fail upstream) |

Run the app from the `build` folder, because it finds `assets/` relative to that folder. It needs the legacy [DirectX End-User Runtime (June 2010)](https://www.microsoft.com/en-us/download/details.aspx?id=8109) for `d3dx*_43.dll`. The exporter doesn't.

## Choosing an engine

`assets/main.mr` selects the engine with its third `import` line:

```
import "engine_sim.mr"
import "themes/default.mr"
import "engines/atg-video-2/01_subaru_ej25_eh.mr"

use_default_theme()
main()
```

Point that line at any engine script that defines `main()`, such as the files under `assets/engines/atg-video-1/` and `assets/engines/atg-video-2/`. In the app, press **Enter** to reload the script.

## Exporting engine sounds for games

```
build\Release\engine-sim-export.exe --out engine-audio-export
build\Release\engine-sim-export.exe --script assets\engines\ae86-4age.mr --out engine-audio-export
```

Without `--script`, the exporter uses `assets/main.mr`. Engine scripts must use `set_engine` / `set_transmission` / `set_vehicle`; the Community Edition's `run(...)` syntax isn't supported here.

Each run writes `engine-audio-export/<engine>/` (gitignored):

| Clips | Use |
| --- | --- |
| `onload_XXXXXrpm`, `offload_XXXXXrpm` | Seamless loops every 500 rpm at full and closed throttle, for RPM crossfading |
| `idle`, `limiter` | Seamless loops |
| `startup`, `shutdown`, `rev_blip` | One-shots |
| `sweep_onload_up`, `sweep_offload_down` | RPM sweeps for granular engines (AudioMotors, REV, FMOD/Wwise granular) |

Formats:

- `ogg/` (recommended): Ogg Vorbis, sample-accurate, with `LOOPSTART`/`LOOPLENGTH` tags.
- `wav/`: 24-bit WAV with a `smpl` loop chunk.
- `mp3/`: MP3 with a LAME gapless tag. Avoid it for loops: many game engines ignore the tag, which leaves a gap at the loop point.
- `telemetry/`: RPM and throttle over time for the sweeps and one-shots.
- `manifest.json`: every clip's RPM (the pitch reference), loop points, loudness and natural level, plus RPM-sorted blend layers.

All clips are normalized to −18 LUFS with a −1 dBFS peak ceiling, and the game applies its own volume curve. Use `--normalize set` to keep the simulator's raw relative levels instead.

Common options:

| Option | Default |
| --- | --- |
| `--rpm-min` / `--rpm-max` / `--rpm-step` | dyno minimum / redline / 500 |
| `--clips idle,onload,offload,limiter,startup,shutdown,revs,sweeps` | all |
| `--formats ogg,mp3,wav` | all |
| `--loop-length <s>` | 4 |
| `--loudness <LUFS>` / `--peak-db <dBFS>` | −18 / −1 |
| `--sim-frequency <hz>` | the engine's own setting |
| `--jobs <n>` | all cores |

Run `engine-sim-export --help` for every option. [docs/game-audio-export.md](docs/game-audio-export.md) explains how the loops are made seamless, what's in the manifest, and how to play the set in Unity, Unreal, Godot, FMOD and Wwise.

## App controls

| Key / input | Action |
| :---: | :---: |
| A | Toggle ignition |
| S | Hold for starter |
| Q, W, E, R | Throttle presets |
| Space + scroll | Fine throttle |
| D | Toggle dyno |
| H | Toggle RPM hold (needs the dyno on) |
| G + scroll | Change hold speed |
| Up / Down | Shift gear |
| Shift | Clutch (hold Space as well to engage it slowly) |
| Z + scroll | Volume |
| X + scroll | Convolution level |
| C + scroll | High-frequency gain |
| V + scroll | Low-frequency noise |
| B + scroll | High-frequency noise |
| N + scroll | Simulation frequency |
| I | Show dyno stats |
| M / , | Next / previous view layer |
| Tab | Change screen |
| F | Fullscreen |
| 1 to 5 | Slow motion |
| Enter | Reload engine script |
| Escape | Quit |

## License

MIT. See [LICENSE](LICENSE). The original simulator is by [AngeTheGreat (Ange Yaghi)](https://github.com/ange-yaghi).
