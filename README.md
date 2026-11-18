# Vintage Chonk

A warm, modern-classic synthesizer: a VST3 instrument for Windows DAWs, built from an original sound engine
and a 1970s-style panel. It also converts Native Instruments Massive presets (`.nmsv`) into its own sounds.

![The Vintage Chonk panel in a DAW](docs/screenshots/vst3_panel.png)

## Features

* **3 oscillators** with eight classic analogue-style waveforms (triangle, "shark", reverse saw, saw, square
  and three pulses), footage ranges 64' to 1', fine tune and a phase-bend intensity.
* **Mod oscillator** for ring, phase and wave-position modulation; **noise** with colour.
* **2 filters**: LADDER (24 dB transistor ladder), ACID (303-style diode ladder), LP4, LP2, HP, BP, NOTCH;
  serial, parallel or anything between.
* **4 envelopes** with delay, attack level, decay level, sustain time and loop.
* **4 modulators**, each an LFO, a 16-step Performer (32 curve shapes) or a 16-step Stepper; 16 modulation
  routings, 8 macros, velocity, key, mod wheel, aftertouch, bend and random as sources.
* **6 effects** in a fixed chain: OCT FUZZ, TAPE, ENSEMBLE, BBD ECHO, OIL CAN, REVERB (TWIN TANK or SPRING);
  both echoes can sync to the host tempo.
* **16 voices**, poly / mono / legato, glide.
* **16 factory presets** as host programs; your own presets saved by name.
* **22 automatable host parameters** (8 macros, master, filter cutoffs and emphasis, mod osc, effect levels,
  glide); full state saved with the DAW project.
* **IMPORT .NMSV**: converts Native Instruments Massive (original, not Massive X) presets on the spot, with a
  report of what carried over. The converter also exists as a single offline web page
  (`editor/converter.html`) and as a Python tool (`tools/preset_converter/convert.py`).
* Any sample rate and block size: the engine runs at 44.1 kHz with a high-quality resampler; output does not
  depend on the host's block size; 1.5-2.2 ms latency, reported to the host.

![Effects page](docs/screenshots/effects.png)

The full user guide is `docs/guide/Vintage-Chonk-User-Guide.pdf`. It also covers the same engine as
replacement firmware for the M-VAVE FM-1 keyboard and as an Android tablet app; those two builds live outside
this repository (the FM-1 firmware depends on M-VAVE's SDK).

## Build the VST3 (Windows x64)

Needs: Visual Studio 2022 Build Tools (MSVC, Windows SDK 10.0.26100), git, LLVM for Windows (`clang-cl.exe`
in `C:\Program Files\LLVM`, or set `MFE_CLANG_CL`), and optionally node (regenerates the factory bank header).

```powershell
git clone <this repository> vintage-chonk
cd vintage-chonk
powershell -ExecutionPolicy Bypass -File plugin\build.ps1 -Test
```

`plugin\build.ps1` first runs `plugin\fetch_deps.ps1`, which downloads the third-party code at pinned versions
and checks each by commit or SHA-256: JUCE 8.0.15 (with one small patch, see `plugin/patches/`) and Microsoft's
WebView2 SDK 1.0.4314.35. Output: `plugin\dist\Vintage Chonk.vst3`. Install it by copying that folder into
`C:\Program Files\Common Files\VST3\` (admin), then rescan plugins in your DAW. Details, the host test and
Steinberg's validator: `plugin/README.md`.

## Other tests

```powershell
powershell -ExecutionPolicy Bypass -File run_tests.ps1    # engine (C), converter (Python), web pages (node)
```

The C tests need gcc or clang; the page tests use headless Chrome when it is installed.

## Repository layout

| Folder | What |
|---|---|
| `dsp/` | the sound engine (C11, integer and float paths, no allocation) |
| `plugin/` | the VST3 plugin (JUCE 8), build and fetch scripts, JUCE patch, host test |
| `panel/` | the web panel (HTML/CSS/JS) shown in the plugin window |
| `editor/` | the browser preset converter (one offline HTML file, built from `editor/src`) |
| `tools/` | `.nmsv` reader, preset converter, offline renderer, table generator, page builders |
| `tests/` | engine, sequencer, converter and page tests; synthetic test presets |
| `docs/guide/` | the user guide (HTML sources, pictures, PDF) |
| `examples/` | a few short demo renders and their presets |

## Licence

Vintage Chonk is free software under the GNU Affero General Public License v3 (`LICENSE`), because the
plugin links JUCE under its AGPLv3 option. Third-party code, fonts and trademarks: `NOTICE`.

Native Instruments and Massive are trademarks of Native Instruments GmbH; M-VAVE and FM-1 belong to their
owner; VST is a trademark of Steinberg Media Technologies GmbH. Vintage Chonk is an independent project, not
affiliated with or endorsed by them. No Native Instruments software, presets or wavetables are included.
