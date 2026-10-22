# WalleFFP

A small `d3d9.dll` proxy that makes **WALL-E (PC, 2008)** playable under the
[NVIDIA RTX Remix](https://github.com/NVIDIAGameWorks/rtx-remix) runtime.

It sits between the game and the Remix bridge, forwards every Direct3D 9 call,
and only adds what Remix cannot work out on its own. Game files are not
modified.

## What it does

- **Gives Remix a camera.** The game draws everything with vertex shaders and
  never calls `SetTransform`, so Remix sees no camera. The proxy reads the
  view and projection matrices from the vertex shader constants
  (`c5..c8` view, `c10..c13` projection), validates the projection, detects
  whether it needs transposing and publishes both via `SetTransform` once per
  scene on draws into the backbuffer.
- **Steers the game to its simple render path.** `GetDeviceCaps` reports
  Pixel Shader 1.4 / Vertex Shader 1.1 and at most 4 texture stages.
- **Fixes the startup hang with Remix.** The Remix bridge client re-posts
  `WM_ACTIVATEAPP(TRUE)` for every one it receives; the game answers each with
  `SetWindowPos(TOPMOST)` + `ShowWindow(SW_MAXIMIZE)` + `WM_SIZE`, so the
  message queue never drains and the first frame is never rendered. The proxy
  subclasses the game window above the bridge and drops repeated activation
  messages whose state did not change.

## Install

1. Copy the RTX Remix runtime (bridge client `d3d9.dll`, `.trex/` and the rest)
   next to `wall-e.exe` as usual.
2. Rename the Remix `d3d9.dll` to `d3d9_remix.dll`.
3. Put the `d3d9.dll` built from this repository next to `wall-e.exe`.

If `d3d9_remix.dll` is missing, the proxy falls back to the system `d3d9.dll`
(useful to check that the proxy alone does not change the picture).

## Switches

All switches are environment variables, read when the game starts. `0` turns a
switch off, anything else turns it on.

| Variable | Default | Effect |
| --- | --- | --- |
| `WALLEFFP_SPOOF_CAPS` | on | Report PS 1.4 / VS 1.1 caps |
| `WALLEFFP_PUBLISH_CAMERA` | on | Publish view/projection from VS constants |
| `WALLEFFP_FORCE_PRIMARY_RT` | off | Redirect backbuffer-sized render targets to the backbuffer |
| `WALLEFFP_FORCE_PRIMARY_RT_AGGRESSIVE` | off | Redirect every render target to the backbuffer (breaks post-processing) |

The proxy writes diagnostics with `OutputDebugString` (activation filter,
camera projection, per-frame render target usage every 300 frames). Use
DebugView or attach a debugger to read them.

## Build

- Visual Studio 2026 (platform toolset v145), Windows SDK.
- Open `WALLEFFP/WALLEFFP.slnx`, pick **Win32** (the game is 32-bit) and
  Debug or Release. The output is `d3d9.dll`.
- Optional: set `WALLEFFP_GAME_DIR` to the game folder and the post-build step
  copies `d3d9.dll` (and `d3d9.pdb`) there.

## License

MIT, see [LICENSE](LICENSE). RTX Remix and WALL-E are not part of this
repository and are covered by their own licenses.
