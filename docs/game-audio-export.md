# Game audio export

`engine-sim-export` renders an engine sound set that a game can play back in
real time. The physics runs offline, without a window or audio device, and the
tool writes seamless RPM loops, transients and RPM sweeps as Ogg Vorbis, MP3
and WAV files, plus a JSON manifest describing every clip.

```
build/Release/engine-sim-export.exe --out engine-audio-export
```

With no arguments it exports the engine selected in `assets/main.mr`. A full
set (29 clips, 1000–6000 rpm in 500 rpm steps) takes about 40 seconds on a
16-thread CPU.

## What a game needs

Most racing and driving games build engine audio in one of two ways:

1. **Crossfaded RPM loops** (Forza, Project CARS, most indie titles). The
   engine is recorded at a set of fixed RPMs. At runtime the two loops
   nearest the current RPM play together and are crossfaded, and each one is
   pitch-shifted by `currentRpm / loopRpm`. Loops are recorded in two load
   states:
   * **on-load**: throttle open and the engine working, the aggressive
     "accelerating" sound;
   * **off-load**: throttle closed and the engine being driven by the car,
     the softer "overrun/decelerating" sound.

   The game blends these two layers by throttle or engine load. The fewer
   loops there are, the further each one has to be pitch-shifted, and heavy
   pitch-shifting sounds artificial ("lawnmower" at high pitch). A spacing of
   300–500 rpm is typical.

2. **Granular synthesis** (AudioMotors, REV, Wwise/FMOD granular plugins).
   The tool cuts a continuous recording of the engine sweeping through its
   RPM range, together with an RPM curve, into per-cycle grains, then
   re-sequences the grains in real time. This needs **RPM sweeps** with
   **RPM telemetry**, not loops.

Both approaches are layered with **one-shots** for events that loops can't
express: starting, stopping, free-rev blips and the rev limiter.

The exporter produces all of these:

| Need | Clips | Notes |
| --- | --- | --- |
| On-load RPM layer | `onload_XXXXXrpm` | Dyno holds the RPM at full throttle (`--onload-throttle`) |
| Off-load RPM layer | `offload_XXXXXrpm` | Dyno motors the engine with the throttle closed (`--offload-throttle`) |
| Idle | `idle` | Engine idles on its own; RPM is measured |
| Rev limiter | `limiter` | Full throttle in neutral, bouncing on the limiter; loopable |
| Startup / shutdown | `startup`, `shutdown` | Starter cranking until the engine catches; ignition off until it stops |
| Free-rev blip | `rev_blip` | Idle, full throttle to 85 % of redline, release, back to idle |
| Granular source | `sweep_onload_up`, `sweep_offload_down` | Slow dyno ramps across the RPM range, with telemetry CSVs |

### How the loops are made seamless

A loop only sounds right if the end flows into the start without a click,
a level dip or a hiccup in the firing rhythm.

1. **Steady state.** The dynamometer holds the crank at the target RPM. The
   engine settles for `--settle` seconds before recording starts, so gas
   dynamics and temperatures have stabilized.
2. **Pitch-synchronous length.** The loop length is a whole number of engine
   cycles. One cycle is 720° of crank rotation for a four-stroke, or
   `44100 × 120 / rpm` samples, and the cycle count is chosen so the loop
   comes close to `--loop-length`. The exact length is then refined to the
   sample with the highest normalized cross-correlation across the seam.
   The start point is searched as well, which helps unsteady signals such as
   a hunting idle.
3. **Correlation-adaptive crossfade.** The seam is blended over
   `--crossfade` milliseconds, or at least one full engine cycle. The fade
   law adapts to the measured correlation. Coherent audio uses an
   equal-gain fade and uncorrelated noise uses an equal-power fade, which
   keeps the perceived level constant across the seam.
4. **DC removal.** Mean subtraction for loops, a 5 Hz high-pass for
   one-shots.

The manifest reports `seamCorrelation` for every loop. For engine-sim
engines this is typically 0.95–0.999. It is lower at very low RPM under full
load, where the simulation's combustion randomness makes consecutive cycles
genuinely different; the adaptive crossfade covers that case.

### Levels

The synthesizer's raw output has an unrealistically wide level range. For
the default Subaru EJ25, the 1000 rpm on-load loop is about 65 dB quieter
than the 6000 rpm one. The interactive application hides this with an
automatic leveler. For games, the exporter offers three modes:

* `--normalize loudness` (default). Every clip is normalized to the same
  integrated loudness (ITU-R BS.1770, default `-18 LUFS`), limited by the
  sample peak ceiling (`--peak-db`, default `-1 dBFS`). Layers then crossfade
  without level jumps, and the game controls volume through its own
  RPM/throttle curves, which is how loop-based engine audio is usually
  mixed. Each clip's natural level is kept in the manifest as
  `naturalLevelDb` (0 dB = the loudest clip), so the original curve can be
  restored, or compressed (see below).
* `--normalize set`: one gain for the whole set, raw relative levels kept.
* `--normalize file`: each clip peak-normalized on its own.

The peak ceiling leaves headroom for lossy encoding, which can push peaks
slightly above the source.

## Formats

| Format | Use it for | Loop metadata |
| --- | --- | --- |
| **Ogg Vorbis** | Recommended for everything, especially loops | The exact sample count is stored in the stream, so decoders return exactly the encoded length. Vorbis comments `LOOPSTART=0` and `LOOPLENGTH=<n>` are added for engines that read them. |
| **WAV** (24-bit, or 16-bit with TPDF dither) | Masters, middleware import (FMOD, Wwise), re-encoding | A `smpl` chunk with a forward loop over the whole file, read by Unity, Unreal, FMOD, Wwise and most samplers |
| **MP3** | Compatibility only; avoid it for loops | MP3 frames add encoder delay and padding. The exporter writes a Xing/LAME info tag recording both, which gapless decoders use to trim them, but many players and engines ignore it. That leaves a short gap at the loop point. |

Every file holds exactly one loop (loop start 0, loop end = file end), so
engines that can only loop the whole file, such as Unity's
`AudioSource.loop`, still play seamlessly.

## Output

```
engine-audio-export/<engine>/
  manifest.json
  ogg/        idle.ogg, onload_01000rpm.ogg, ..., sweep_onload_up.ogg
  mp3/        same names
  wav/        same names
  telemetry/  startup.csv, shutdown.csv, rev_blip.csv, sweep_*.csv
```

RPM values in file names are zero-padded, so an alphabetical sort is also an
RPM sort.

Telemetry CSVs have one row per 4 ms simulation block: `time_s`, `sample`
(index into the clip), `rpm`, `throttle` (0–1 speed control), and
`dyno_torque_nm`. Granular tools import the sweep audio together with its
RPM curve.

### manifest.json

```jsonc
{
  "engine": { "name": "Subaru EJ25", "cylinders": 4, "displacementLiters": 2.457,
              "redlineRpm": 6500, "idleRpm": 768, "limiterRpm": 6503, ... },
  "audio":  { "sampleRate": 44100, "channels": 1, "normalization": "loudness",
              "targetLoudnessLufs": -18, "peakCeilingDbfs": -1 },
  "layers": {                       // RPM-sorted blend layers
    "onLoad":  [ { "clip": "onload_01000rpm", "rpm": 1000.0, "naturalLevelDb": -78.4 }, ... ],
    "offLoad": [ ... ]
  },
  "clips": [
    {
      "name": "onload_03000rpm", "kind": "loop", "group": "onload", "load": "on",
      "throttle": 1, "targetRpm": 3000,
      "rpm": { "mean": 3000, "min": 3000, "max": 3000 },   // pitch reference
      "samples": 176400, "durationSeconds": 4,
      "loop": { "start": 0, "length": 176400, "engineCycles": 100,
                "crossfadeSamples": 3528, "seamCorrelation": 0.996 },
      "gainDb": -110.2, "peakLimited": false, "naturalLevelDb": -33.5,
      "loudnessLufs": -18, "peakDbfs": -8.7, "rmsDbfs": -16.6,
      "files": { "ogg": "ogg/onload_03000rpm.ogg", "mp3": "...", "wav": "..." }
    }
  ]
}
```

## Playing the set in a game

Each frame, given `rpm` and `throttle` (or engine load) from the vehicle
simulation:

1. **Pick the neighbours.** In each layer (`onLoad`, `offLoad`), find the
   two loops whose `rpm` brackets the current RPM. Below the lowest loop, use
   `idle` as the lower neighbour.
2. **Crossfade by RPM.** `t = (rpm - lo.rpm) / (hi.rpm - lo.rpm)`. Use
   equal-power gains (`gLo = cos(t·π/2)`, `gHi = sin(t·π/2)`) so the
   loudness doesn't dip between loops.
3. **Pitch.** Set each voice's playback rate to `rpm / clip.rpm.mean`. With
   500 rpm spacing the shift stays within about ±25 %.
4. **Blend by load.** Multiply the on-load layer by `throttle` and the
   off-load layer by `1 - throttle`, again with an equal-power curve, and
   smooth the throttle value over 50–150 ms.
5. **Overall volume.** With `loudness` normalization, apply your own volume
   curve. A good starting point is the natural level compressed about 3:1:
   `volumeDb = naturalLevelDb / 3`, linearly interpolated between the
   neighbouring loops.
6. **Events.** Play `startup`, `shutdown` and `rev_blip` as one-shots. Fade
   to `limiter` while the engine sits on the rev limiter.

Engine notes:

* **Unity.** Import `.ogg` or `.wav`. Use *Decompress On Load* for loops
  this short so pitch changes cost nothing. Drive `AudioSource.pitch` and
  `volume` per voice, with `loop = true`, and start every loop voice at once
  so they stay alive while silent.
* **Unreal.** Import the `.wav` files. Sound Waves pick up the `smpl` loop,
  and MetaSounds can do the RPM crossfade and pitch with a Wave Player per
  loop.
* **Godot.** `AudioStreamOggVorbis` with `loop = true`. Drive `pitch_scale`
  and `volume_db` on one `AudioStreamPlayer` per loop.
* **FMOD / Wwise.** Put the loops in a multi-instrument or Blend Container
  on an RPM parameter, one track each for on-load and off-load, with
  overlapping crossfades and AutoPitch or pitch RTPCs. For granular plugins,
  feed them the sweeps and the telemetry CSV.

## Options

Run `engine-sim-export --help` for the full list. The most useful ones:

| Option | Default | |
| --- | --- | --- |
| `--script <file.mr>` | `assets/main.mr` | Any engine-sim script that calls `set_engine` |
| `--rpm-min/--rpm-max/--rpm-step` | dyno minimum / redline / 500 | Loop grid; the top of the range is always included |
| `--clips` | all | `idle,onload,offload,limiter,startup,shutdown,revs,sweeps` |
| `--formats` | `ogg,mp3,wav` | |
| `--loop-length` | 4 s | Longer loops repeat less obviously |
| `--crossfade` | 80 ms | At least one engine cycle is always used |
| `--normalize` / `--loudness` / `--peak-db` | `loudness` / -18 LUFS / -1 dBFS | See *Levels* |
| `--sim-frequency` | engine's own | Raise it (e.g. 40000) for cleaner high-RPM audio; render time scales with it |
| `--jobs` | all cores | |
| `--seed` | 1 | Renders are reproducible for a given seed |

## Limitations

* **Exhaust only.** engine-sim synthesizes the exhaust note, not intake
  roar, induction or turbo noise, gear whine, tyres or wind. Layer those
  separately.
* **Four-stroke cycle.** Loop lengths assume a 720° cycle, which is what
  engine-sim simulates.
* **Model behaviour.** Levels and timbre are whatever the simulation
  produces. In engine-sim, low-RPM full-load audio is much quieter than
  overrun at the same RPM; `naturalLevelDb` records this.
* **Reproducibility.** engine-sim draws its noise from `rand()`. The MSVC
  runtime keeps that state per thread, so the exporter seeds it per clip and
  output does not depend on `--jobs`. On a C runtime with a global `rand()`
  state, multi-threaded renders would not be bit-identical.
* **Rendering speed.** The physics runs at roughly real time per thread, so
  sets render in parallel, one engine instance per clip.

## Sources

* Audiokinetic, [Loop-Based Car Engine Design with Wwise, part 1](https://audiokinetic.com/loop-based-car-engine-design-with-wwise-part-1) and [part 2](https://www.audiokinetic.com/en/loop-based-car-engine-design-with-wwise-part-2)
* Audiokinetic, [Engine Sound Modeling: From Sampling to Granular Synthesis in Wwise](https://www.audiokinetic.com/engine-sound-modeling-from-sampling-to-granular-synthesis-in-wwise)
* Designing Sound, [Vehicle engine design: Project CARS, Forza Motorsport 5 and REV](https://designingsound.org/2014/08/11/vehicle-engine-design-project-cars-forza-motorsport-5-and-rev/)
* MCV, [How to make racing car engines roar using AudioMotors FMOD](https://www.mcvuk.com/development/how-to-make-racing-car-engines-roar-using-audiomotors-fmod)
* Wikipedia, [Gapless playback](https://en.wikipedia.org/wiki/Gapless_playback) (MP3 encoder delay and the LAME tag)
* ITU-R BS.1770-4, *Algorithms to measure audio programme loudness and true-peak audio level*
