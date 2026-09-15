# DOOM for PicOS

id Software's DOOM running on the ClockworkPi PicoCalc under
[PicOS](https://github.com/jeffory/PicOS). The engine is
[doomgeneric](https://github.com/ozkl/doomgeneric) (GPL-2.0), pulled in as the
`src/` submodule from the [jeffory/doomgeneric](https://github.com/jeffory/doomgeneric)
fork on its `picos` branch. The PicOS platform layer lives in this repo: display,
input and timing in `dg_picos.c`, sound and music in `i_picos_sound.c`, and a small
OPL2 emulator (`opl.c`) with a MUS player (`mus_player.c`) for the soundtrack.

The shareware WAD (`doom1.wad`, episode 1) ships in the release ZIP. The app asks
PicOS for a 300 MHz core clock via `system_clock_khz` in `app.json`.

## Install

DOOM is on the **PicOS App Store**. Open the Store app on your PicoCalc and install
it from there.

## Build

Needs `arm-none-eabi-gcc` (tested with 15.2) and a newlib for ARM:

```sh
git submodule update --init
make
```

That produces a stripped `main.elf`. The PicOS native SDK headers and linker script
are vendored in `sdk/native/`.

`tools/opl_replay.c` is a desktop tool that renders an `.opcl` OPL capture to WAV
for checking the music emulation; its build line is in the file header.

## Release

1. Bump `version` in `app.json`.
2. Commit the change.
3. `git tag v<version> && git push && git push --tags`

GitHub Actions builds `main.elf`, packages `app.json`, `main.elf` and `doom1.wad`
into a single ZIP and publishes it as the Release for that tag. The PicOS App Store
re-indexes within about 30 minutes.
