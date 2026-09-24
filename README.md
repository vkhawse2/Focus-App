# Focus App

A tiny, fully offline Windows focus timer that plays **binaural beats**
(alpha / beta / gamma) to help you concentrate. Single ~125 KB executable,
no installer, no network, sips RAM.

## Features

- **True binaural beats** — 200 Hz carrier in the left ear, 200 + beat in the
  right ear (use headphones for the effect):
  - Alpha — 10 Hz (relaxed focus)
  - Beta — 20 Hz (active focus)
  - Gamma — 40 Hz (deep concentration)
- **Click-free audio engine** — preset switches dip the volume to
  near-silence, swap the frequency at the bottom of the dip, and glide back
  up; stopping and quitting fade out instead of cutting, so there are never
  pops or cracks.
- System tray icon — minimize/hide to tray, tray menu (Open / Stop / Exit).
- Session timer, volume slider, animated waveform.
- Custom lightning-bolt app + tray icon.
- 100% offline: links only against Windows system DLLs
  (`KERNEL32`, `USER32`, `GDI32`, `SHELL32`, `WINMM`, `msvcrt`).

## Build (Windows, MinGW-w64)

```bat
build.bat
```

This runs `windres` on `resources.rc` and compiles `focus_app.c` into
`FocusApp.exe`. Any MinGW-w64 toolchain works
([MSYS2](https://www.msys2.org/), [winlibs](https://winlibs.com/), …).

## Cross-compile (Linux)

```sh
x86_64-w64-mingw32-windres resources.rc -O coff -o resources.o
x86_64-w64-mingw32-gcc -O2 -s -mwindows -municode -o FocusApp.exe \
    focus_app.c resources.o -lwinmm -lshell32
```

## Audio test

`test_dsp.c` is a host-side harness containing an exact copy of the audio
engine (`fill_buffer` + the UI/callback state machine). It verifies the
binaural frequencies and asserts there are no sample discontinuities when
switching presets, stopping, restarting, or exiting:

```sh
gcc -O2 -o test_dsp test_dsp.c -lm && ./test_dsp
```

## Notes

- The app is unsigned, so Windows SmartScreen will ask for confirmation on
  first launch (**More info → Run anyway**).
- Headphones (or earphones) are required for the binaural effect — speakers
  won't produce it.
