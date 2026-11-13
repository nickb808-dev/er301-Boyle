# Boyle

A clean stereo compressor for the ER-301, in the spirit of the WMD MSCL. It's
meant for taming dynamic signals without adding colour: feedforward,
stereo-linked, with a wide soft knee and every gain calculation done in dB.

Mnemonic **Bo**. Category **Essentials**, next to the stock Limiter.

## Controls

| Control | What it does | Range | Default |
|---|---|---|---|
| `comp` | The MSCL's one knob. It lowers the threshold by up to 24 dB, raises the ratio from 1:1 to ∞:1 (2:1 at 0.5, 4:1 at 0.75), and adds the makeup that keeps the loud parts at the same level | 0 – 1 | 0 |
| `att` | Attack time constant | 0.15 ms – 100 ms | 10 ms |
| `rel` | Release time constant (the reduction falls linearly in dB) | 0.6 ms – 4 s | 150 ms |
| `gain` | Output gain | ±36 dB | 0 dB |
| `mix` | Dry/wet, for parallel compression | 0 – 1 | 1 |
| `key` | Sidechain input. Patch audio here and set Detect to `key`. Leave the dial at 0 | — | 0 |

Every control takes CV. At its defaults (comp 0, gain 0 dB) the unit is an
exact bypass: the output equals the input bit for bit, whatever the other
settings are.

## Menu

| Option | Choices | What it does |
|---|---|---|
| Range | hot / **mid** / low | Where the threshold starts at comp 0: −6, −15 or −24 dBFS. A ±5 V signal peaks at about −6 dBFS. Use low for quiet sources |
| Makeup | **inf** / 9dB / 3dB | Caps the comp knob's makeup gain (the MSCL's Limit switch). With 3 dB, turning comp up mostly lowers the peaks, so it behaves more like a peak limiter |
| Det HPF | off / **60Hz** / 120Hz | A high-pass filter on the detector only, so bass doesn't pump the whole mix. The audio itself isn't filtered |
| Detect | **self** / key | What the detector listens to: the input itself, or the `key` branch, for ducking |

## Display

The meter shows three bars on a dB scale:

- **Left:** the input, with a tick at the threshold.
- **Middle:** the gain reduction, hanging down from the top. A full bar is 24 dB.
- **Right:** the output.

The sub display shows the reduction, the threshold and ratio, and the makeup
and output gain.

## How it works

- **Detector:** the louder of L² and R² (or the key²), high-passed if HPF is
  on, feeds a power envelope that rises at the attack time constant and falls
  at the release time constant. With both channels driving one detector, both
  channels always get the same gain, so the stereo image doesn't move.
- **Gain computer:** a 10 dB soft knee, then a slope of 1 − 1/ratio above it.
- **Gain:** makeup − reduction + output gain, in dB, turned into a linear gain
  once per sample.
- **Latency:** none. There is no lookahead.

## Build and test

```bash
make hosttest
```

```bash
make dist
```

`make hosttest` runs the gates and these host test modes:

- `ident`: the bypass, over 54 option and mix combinations
- `math`: the fast log2 and exp2
- `curve`: the soft-knee formula, plus 48 DC operating points against it
  (within 0.01 dB)
- `attack` and `release`: the time constants, against the closed-form
  envelope
- `link`, `key`, `limit`, `mix`, `hpf`, `viz`, `nan`, `cpu`
- `asan`, under ASAN/UBSAN

`make dist` is the Docker cross-build and package, written to
`build/am335x/boyle-<version>.pkg`.

Started from `er301-template`. `ER301-UNIT-STANDARD.md` is the standard and
`ER301-BRIEF.md` is its condensed version.

## Files

```
src/Boyle.h, .cpp      the engine (od::Object): ports, options, process()
src/BoyleMeter.h       the graphics (od::Graphic): meter and sub display
src/libboyle.swig      SWIG interface
src/compat.cpp         sincosf / fprintf stubs the firmware ABI lacks
assets/Boyle.lua       the unit: graph, menu, views
assets/MeterView.lua   the display-only ViewControl
assets/toc.lua         package manifest
test/host/             the host rig: fake od:: layer + test modes
tools/                 the check scripts
```

Apache-2.0, see `LICENSE` and `NOTICE`.
