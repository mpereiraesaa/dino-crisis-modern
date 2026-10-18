# Dino Crisis Modern

Modern controls and an optional over-the-shoulder camera for the PC version
of Dino Crisis 1, on top of [Classic REbirth](https://classicrebirth.com/index.php/dino-crisis-classic-rebirth/).

It does not touch the game files or Classic REbirth's files. You drop in two
files and delete them to uninstall.

## Features

- **Modern movement.** Push the stick and Regina goes that way on screen, like
  in the RE2/RE4 remakes. Push it a little to walk and all the way to run. The
  d-pad keeps the original tank controls.
- **Modern aiming.** LT aims, RT shoots. RT alone does a quick turn.
- **Sensible menus.** Y opens the inventory and map, B backs out.
- **Door skip.** No door animation: you open a door and you are in the next room.
- **Over-the-shoulder camera (optional).** RE4-style, turned with the right
  stick. It moves in close while you aim and stops at walls instead of going
  through them. F12 or R3 switches between it and the original fixed cameras.

The levels were only built for the fixed cameras, so the shoulder camera can
show black void at the edge of the map.

## Install

1. Install Classic REbirth.
2. Copy `xinput9_1_0.dll` and `dc_modern.ini` into the game folder, next to
   `Dino Crisis.exe`. A ready-built DLL is in the
   [releases](../../releases).
3. In Classic REbirth's `config.ini`, set `inputMethod = 0` and
   `inputLast = 0` (the first XInput controller).

It needs the Dino Crisis executable Classic REbirth supports. With any other
executable the mod stays off and the game plays as usual.

## Settings

Everything is in `dc_modern.ini`, which is re-read every second while the game
runs. Each section has `Enabled=0` (`Skip=0` under `[Doors]`) to switch it
off. `[Camera] Mode` is `Fixed` or `Modern`, and the other `[Camera]` keys move
the shoulder camera.

## Build

Needs a 32-bit MinGW-w64 toolchain (`i686-w64-mingw32-g++`):

```sh
make
```

That gives `build/xinput9_1_0.dll`.

## How it works

Classic REbirth's `ddraw.dll` imports `XInputGetState` from `xinput9_1_0.dll`.
This DLL takes that name in the game folder, so Windows loads it first. It
calls the system's real XInput by its full path, then edits the controller
state before REbirth sees it. It also reads and patches the game's memory for
the features a controller state alone can't do:

- **Movement.** The stick's screen direction is turned into Regina's yaw,
  relative to the current camera. The stick's tilt decides walk or run.
- **Door skip.** The door transition task is moved straight to its last step.
- **Shoulder camera.** The game's camera callbacks are hooked to place the eye
  and target behind Regina. Camera collision uses the game's own collision
  shapes.
- **Whole room.** While the shoulder camera is on, the room is drawn in full,
  not only the parts the current fixed camera shows. It is also sorted by
  each polygon's depth instead of the fixed per-piece order made for the
  fixed cameras.

- **Faces seen from behind.** The rooms were modelled for the fixed cameras,
  so many walls and fences only exist on one side. With the shoulder camera
  on, the mod tells Classic REbirth to keep those faces too. REbirth, not the
  game, builds and draws the polygons, through Direct3D 9. See
  [Classic REbirth owns the geometry](docs/classic-rebirth.md) before patching
  anything that draws.

The addresses are for the Classic REbirth build of the game. The DLL checks the
executable's code, and REbirth's, before it changes anything.

## Credits

- Gemini and the Classic REbirth team, for making the PC port playable.
- Capcom, for Dino Crisis.

This project is not affiliated with Capcom.
