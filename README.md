# vcam

A virtual camera for Windows 11. It shows up in every app as a real camera (Windows Camera, Teams, Zoom,
Meet in the browser, Discord, OBS...). You feed it from the `vcam` tool (colour bars, a picture, a video, your
screen, a window, another webcam) or from your own program through a small C SDK or Python.

```
vcam install                 # once, as administrator
vcam screen                  # share your screen as a camera
vcam image me.png --fill     # or a picture
vcam webcam --mirror         # or your real webcam, mirrored
```

vcam runs entirely in user mode. There is no driver, no test signing and no Secure Boot change. It uses
the Windows 11 virtual camera API (`MFCreateVirtualCamera`). Windows Frame Server, the service that already
shares physical cameras between apps, loads vcam's media source and hands its frames to every application.

## Compared to OBS Virtual Camera

| | vcam | OBS Virtual Camera |
| --- | --- | --- |
| Apps that use Media Foundation / WinRT only, such as the Windows Camera app | yes | no (DirectShow filter) |
| DirectShow apps | yes, through Frame Server | yes |
| Formats offered to apps | 12 sizes up to 1080p (4K optional), 30 and 60 fps, NV12 / YUY2 / RGB32; apps pick | one, fixed to OBS's output settings |
| Cameras | up to 4, each with its own name | 1 |
| Feed it from your own code | C ABI (`vcam.dll`) and Python | no public API |
| Needs a program running for the camera to exist | no; apps always list it, with a placeholder (yours or the built-in one) until a producer starts | listed; frames need OBS running |
| CPU while nothing is watching | none: sources pause, webcams are released | OBS keeps rendering |
| Screen and window capture | GPU-scaled straight to the size apps ask for | via OBS scenes |
| Compositing scenes, overlays, filters | no, use OBS for that; it can feed vcam through the SDK | yes |

Under the hood it is a real camera device: it appears in Settings › Bluetooth & devices › Cameras, the
camera privacy switches apply to it, and several apps can use it at once.

## Requirements

- Windows 11 (build 22000 or later), x64.
- Administrator rights to install. Running sources works for every user; administrators get a few extras, see
  [How it works](#how-it-works).

## Install

1. Download the `vcam-x64` artifact from the latest successful run of the **build** workflow, or
   [build it](#building), and extract it.
2. In an **administrator** terminal, in the extracted folder:

   ```
   .\vcam install
   ```

   This copies vcam to `C:\Program Files\vcam`, registers the media source and the camera, adds vcam to `PATH`
   and to *Installed apps*. Options:

   | | |
   | --- | --- |
   | `--cameras N` | register N cameras (1-4), fed independently with `--camera N` |
   | `--name NAME` | the name apps show (repeat for each camera); Windows may append "Windows Virtual Camera" |
   | `--max-resolution 1440p\|4k\|WxH` | largest mode offered (default 1080p) |
   | `--no-60fps` | offer 30 fps modes only |
   | `--no-path` | do not add vcam to `PATH` |

Running `vcam install` again upgrades or repairs an installation; files that are in use are replaced at the
next restart. `vcam uninstall` removes everything except the settings folder `%ProgramData%\vcam`.

## Use

Every source streams until Ctrl+C and pauses while no application is using the camera.

| Command | Feeds the camera with |
| --- | --- |
| `vcam bars` | colour bars, a moving marker and a clock (handy to check framing, colours and smoothness) |
| `vcam color #1e90ff` | a solid colour (also `black`, `white`, `red`...) |
| `vcam image FILE` | a picture: PNG, JPEG, BMP, GIF, TIFF, and WebP/HEIC with the Windows codecs; EXIF rotation is applied |
| `vcam video FILE` | a video, looped (`--no-loop` stops at the end); anything Windows can play |
| `vcam webcam [N\|NAME]` | another camera, passed through; useful with `--mirror`, `--fill`, `--flip` |
| `vcam screen [N]` | a monitor (default: the primary one), without the yellow capture border |
| `vcam window TITLE` | the first window whose title contains TITLE |

`vcam list` shows the monitors, windows and cameras you can use. Common options:

| | |
| --- | --- |
| `--camera N` | which vcam camera to feed |
| `--fit` (default), `--fill`, `--stretch` | what to do when the picture's aspect ratio differs from what the app asked for: pad, crop or stretch |
| `--background COLOR` | padding colour for `--fit` |
| `--mirror`, `--flip` | flip left/right, upside down |
| `--fps N`, `--size WxH` | override the frame rate / render size (default: whatever the app asked for) |
| `--no-cursor` | hide the mouse pointer in screen and window capture |
| `--always` | keep producing even while no app watches |

`vcam status` shows the installation, whether each camera is registered, which producer feeds it, what apps
are streaming (size, format, frame rate) and frame counters.

### More

- `vcam placeholder FILE` sets the picture apps see while nothing feeds a camera; `--reset` restores the
  built-in one.
- `vcam autostart image C:\me.png --fill` runs that source whenever you sign in, without a UAC prompt (a
  scheduled task); `vcam autostart --off` removes it.
- `vcam config --name 2="Slides" --max-resolution 1440p --60fps off --logging on` changes settings later.
  New settings apply the next time an app opens the camera, or immediately after
  `vcam restart-frame-server` (which briefly disconnects apps using any camera).

## Feeding it from your own code

The SDK takes frames in BGRA, RGBA, BGR, RGB, gray, NV12, I420, YUY2 or UYVY, at any size and at your own pace.
It converts them directly into the format the app is streaming. If you are slower than the app, the camera
repeats your last frame; if you are faster, it drops frames. `vcam_get_status` tells you the size the app wants,
so you can render at exactly that size and skip scaling.

**C / C++**: `sdk\vcam.h`, link `sdk\vcam.lib`, ship `vcam.dll`:

```c
#include "vcam.h"

vcam_producer* cam;
vcam_options options = VCAM_OPTIONS_INIT;   /* camera 1, fit, no mirroring */
if (vcam_open(&options, &cam) == VCAM_OK) {
    while (running) {
        render(pixels, width, height);
        vcam_send_packed(cam, VCAM_FORMAT_BGRA, width, height, pixels, 0);
    }
    vcam_close(cam);
}
```

**Python** (`python\vcam.py`, numpy optional):

```python
import vcam

with vcam.Camera(fmt="bgr") as cam:       # an OpenCV frame is BGR
    while True:
        ok, frame = capture.read()
        cam.send(frame)
        cam.sleep_until_next_frame()
```

`python\example.py` is a complete animated example. Only one producer can feed a camera at a time;
`vcam_open` reports `VCAM_ERROR_BUSY` otherwise.

## How it works

```
 producer (vcam.exe, your app)              Windows Frame Server (service)            applications
 ┌──────────────────────────┐   shared    ┌─────────────────────────────┐        ┌──────────────┐
 │ SDK: convert + scale into │  memory    │ vcamsource.dll              │  MF /  │ Camera, Teams│
 │ the format apps asked for │ ─────────► │ media source per camera:    │ ─────► │ Zoom, OBS,   │
 │ (vcam_send)               │ ◄───────── │ paces frames, placeholder   │  DShow │ browsers ... │
 └──────────────────────────┘  requested  └─────────────────────────────┘        └──────────────┘
                                 format
```

- **Cameras** are registered once, for all users and with system lifetime, so they exist whether or not anything
  feeds them. vcam tags each camera, and an elevated `vcam` re-registers any camera Windows dropped.
- **The media source** (`vcamsource.dll`) runs inside Frame Server as LOCAL SERVICE. Each camera advertises
  NV12, YUY2 and RGB32 at standard sizes. While an app streams, a high-priority thread delivers samples at the
  negotiated rate. A producer frame goes out as soon as it arrives, and the last one repeats when the producer is
  slower, so latency stays below one frame. Without a producer it shows the placeholder.
- **Shared memory** connects the two: a header and three frame slots per camera. Writers never wait for readers:
  each slot is protected by a sequence lock, and a reader that loses a race simply takes the next frame. A crashed
  producer or app can never block the other side. The media source treats the memory as untrusted input and
  validates every frame header before reading pixels. The camera publishes the format it is streaming, so
  producers convert straight into it and the source only copies.
- **Who creates the memory**: whoever comes first, either the media source (session 0) or an elevated producer.
  A producer without administrator rights that starts before any app has opened the camera keeps its last frame
  and publishes it the moment an app does.
- **Conversion and scaling** are portable C++ with SSE4.1 kernels. They handle BT.601/BT.709 and limited/full
  range. The scaler averages the source area when shrinking, so it does not alias. Screen capture scales on the
  GPU instead.

Security: the shared memory and its events are open to every signed-in user (`Authenticated Users`), just like
the cameras, which are registered for all users. On a machine shared between several people, any of them can
feed or watch a vcam camera.

## Troubleshooting

| Problem | What to do |
| --- | --- |
| Apps do not list the camera | `vcam status`. If a camera is "NOT registered", run `vcam install` again (as administrator). Make sure Settings › Privacy & security › Camera allows camera access. |
| `vcam install`: access denied while registering | Camera access is off in the privacy settings; or the files came from the internet and are blocked (vcam removes the mark itself, but antivirus tools may block too). |
| The picture is the placeholder | No producer is running for that camera (`vcam status` shows it). |
| Changed settings do not show | `vcam restart-frame-server`, then reopen the camera. |
| Anything else | `vcam config --logging on`, reproduce, and read `%ProgramData%\vcam\logs\vcamsource.log` (written by the media source inside Frame Server). |

## Building

Visual Studio 2022 or later with the C++ desktop workload (Windows 11 SDK), and CMake:

```
cmake -S . -B build -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

The binaries land in `build\bin\Release`: `vcam.exe`, `vcamsource.dll` and `vcam.dll`. Run `vcam install` from
there. The portable core and its tests also build on Linux (`cmake -S . -B build && cmake --build build && ctest --test-dir build`).

| Path | What |
| --- | --- |
| `core/` | portable: pixel formats, conversion and scaling (+ SSE4.1), shared-memory ring, frame pacing, test pattern and placeholder |
| `windows/source/` | `vcamsource.dll`, the Media Foundation media source Frame Server loads |
| `windows/sdk/` | `vcam.dll` + `vcam.h`, the producer SDK |
| `windows/app/` | `vcam.exe`: install / status / config, and the sources |
| `windows/common/` | names, configuration, logging and shared memory shared by all three |
| `python/` | the Python binding and an example |
| `tests/` | unit tests for the core, and an end-to-end test that drives the media source like Frame Server does |

### Tests

- `core_tests`: every format-to-format conversion round trip, SIMD versus scalar output (bit-identical), scaling
  quality, the shared-memory ring under a concurrent writer and reader (no torn frames), fuzzed frame headers,
  and frame pacing. CI runs it on Linux (with AddressSanitizer and UBSan) and on Windows.
- `source_test`: loads `vcamsource.dll` in-process and drives it the way Frame Server does: activation, the
  sample allocator, start, sample requests, format switches and shutdown. A real producer feeds it, and the test
  checks colours, letterboxing, mirroring, re-rendering after a format change, the 30 fps pacing and the
  placeholder.
