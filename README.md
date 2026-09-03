# Tr4nZf0rM3r5_sequenced

A timed transformation-sequencer **video filter** for [OBS Studio](https://obsproject.com).
Attach it to any source (typically an Image source) and it runs a chain of GPU
transformations — rotate, zoom, scale, flip — with per-step durations, then
optionally hides the source, or swaps in a fresh image from a library on every
trigger.

Built for streamers who fire stingers from an external controller (TouchPortal,
Stream Deck, hotkeys…): one visibility toggle = one full animation, always
re-armed.

[![IMAGE ALT TEXT](http://img.youtube.com/vi/gXX72yIGDcI/0.jpg)](http://www.youtube.com/watch?v=gXX72yIGDcI "Video Title")
<br>
Click the image above to watch the demo video.

---
### 🚀 Get the Windows Installer
Want the ready-to-install binary without building from source?
Get the official installer and portable zip here: **[Tr4nZf0rM3r5_sequenced on Gumroad](https://9381776901660.gumroad.com/l/lkgtbv)**
---

## Features

- **Up to 8 sequential steps**, each one of:
  - **Rotate** — full turns, clockwise / counter-clockwise
  - **Zoom in & out** — ping-pong scale peak
  - **Scale** — linear X/Y scale
  - **Flip** — N flips, vertical or horizontal axis
- **Per-step duration** in milliseconds.
- **Loop modes** — No loop / Loop last step / Loop whole sequence.
- **Start on show** — sequence restarts whenever the source becomes visible.
- **Auto-hide** (No loop only) — source visibility switches itself off when the
  sequence ends, ready for the next trigger.
- **Image library** — point the filter at a folder of images (`00.png`,
  `01.jpg`, …): the source adopts the first image's dimensions, is centred on
  the canvas, and every trigger shows the next image in order — or a random
  one with **Randomise**. Disabling the library (or removing the filter)
  restores the source's original image.
- **Presets** — save / load / delete named step sequences (stored in
  `.tr4nzf0rm3r5_presets.json` in your home folder), plus file **Import /
  Export** (`*.json`) to move presets between machines. Three factory presets
  are built in.

## Requirements

- OBS Studio 30.x or newer (developed and tested on 32.2.2), Windows 10/11 or Linux.
- Plain C, no external dependencies beyond `libobs`.

## Install

Copy the built binary into your OBS plugins folder:

- Windows: `C:\Program Files\obs-studio\obs-plugins\64bit\`
- Linux (flatpak): `~/.var/app/com.obsproject.Studio/config/obs-studio/plugins/tr4nzf0rm3r5_sequenced/bin/64bit/`

## Build from source

Requires an [obs-studio](https://github.com/obsproject/obs-studio) source
checkout for the `libobs` headers (and, on Windows, its compiled `obs.lib`).

Windows — *x64 Native Tools Command Prompt*:

```bat
cmake -S . -B build
cmake --build build --config RelWithDebInfo
```

Linux:

```bash
cmake -S . -B build
cmake --build build
```

## Usage

1. Add a source (e.g. **Image**), right-click → **Filters** → **+** →
   **Tr4nZf0rM3r5_sequenced**.
2. Set **Number of steps**, then configure each step's transformation and
   duration.
3. Tick **Start sequence when source becomes visible** for trigger-based use.
4. Toggle the source's visibility (eye icon / hotkey / controller) to play.

## License

Copyright (C) 2026 xjj3ck

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; either version 2 of the License, or (at your option) any later
version.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program (see [LICENSE](LICENSE)). If not, see
<https://www.gnu.org/licenses/>.
