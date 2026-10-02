# striker1945

A minimal Raylib Lite game: the first frame is an empty black canvas. No example
gameplay, atlas, sounds, asset generation pipeline or game resource partition is
included. The device runtime embeds a generated GSP canvas placeholder in the
application binary; keep that display plumbing when adding your own drawing.

From the workspace root:

```sh
python mosaico.py game sim --project projects/striker1945
python mosaico.py game sim --project projects/striker1945 --headless --frames 120
python mosaico.py game build --project projects/striker1945
python mosaico.py iris system-update --project projects/striker1945
```

Run `python mosaico.py recover` before first installation on a blank or unverified
device. New applications, changed layouts or external resources use
`iris system-update`. The immutable Vibe Mode partitions and OTA routing are
already configured; the engine starts Iris and accepts the image after its first
frame. USB High-Speed remains owned by Iris. Physical input/display behavior
still requires device validation after Host checks.

- `main/game.c`: shared state, pointer input, fixed-step update and rendering.
  Add game logic to `game_update()` and Raylib Lite drawing to `game_render()`.
- `main/game.h`: portable interface with an opaque instance owned by its loop.
- `main/game_config.h`: project identity and display/tick constants.
- `main/game_module.c`: Host lifecycle, controls and JSON state.
- `main/main.c`: device callbacks for the shared engine application runtime.
- `game.sim.json`: sources compiled into the native Host module.

Host state exposes `phase`, `tick`, `pointer_x`, `pointer_y`, `pointer_down` and
`state_hash` for automation. Touch/pointer input records coordinates without
drawing a cursor. Pause/resume/reset are wired for Host controls and replay;
there are no gameplay keyboard bindings yet. Add each new input behavior to the
shared model and map it from the device and Host adapters.

Host needs a C compiler and Pillow, and uses the same C renderer as firmware.
Firmware needs the pinned workspace ESP-IDF and initialized BSP, engine and utils.
Before using any other Raylib API, check the engine's supported public headers.
