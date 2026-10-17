# vcam

A very small virtual webcam for Windows. It shows up as a camera and streams **solid black**
at 640×480, 1280×720 or 1920×1080 (YUY2, 30 fps), whichever the application asks for.

## How it differs from OBS-style virtual cameras

OBS and most virtual camera apps register a user-mode DirectShow filter or Media Foundation
source. Applications that enumerate cameras see a COM class pointing at a DLL.

vcam is a **kernel-mode AVStream capture driver** (`vcam.sys`), the same driver model real
webcams use. It installs as a Plug and Play device (Device Manager → Cameras), is registered
under `KSCATEGORY_VIDEO_CAMERA`, and applications open it through the Windows kernel streaming
stack — DirectShow, Media Foundation / Frame Server and the camera apps all see an ordinary
capture device. There is no helper process feeding frames; the driver produces them itself.

The price of that is driver signing, see [Limitations](#limitations).

## Layout

| Path | What |
| --- | --- |
| `driver/vcam.cpp` | The driver: one capture filter, one pin, a timer DPC that fills each buffer with black |
| `driver/vcam.inf` | Install information (Camera class, `ROOT\VCAM`) |
| `app/vcamctl.c` | `vcamctl install` / `vcamctl uninstall` — creates the device node and installs the package |
| `scripts/install.ps1`, `scripts/uninstall.ps1` | Sign with a local test certificate, trust it, install / remove |

## Install

Download the `vcam-x64` artifact from the latest successful run of the *build* workflow (or
build it yourself, below), unzip it, and in an **elevated PowerShell**:

```powershell
.\install.ps1
```

The first run turns on test signing and asks you to restart; run it again after the restart.
It then signs the package with a certificate it creates on your machine, trusts that
certificate, and installs the driver. "Virtual Webcam" appears under Cameras in Device Manager.

Remove everything with `.\uninstall.ps1`.

## Limitations

- **Driver signing.** Windows only loads signed kernel drivers. This project ships unsigned;
  `install.ps1` uses *test signing*, which requires **Secure Boot off** and shows a
  "Test Mode" watermark. To use it on a normal machine you need the driver signed through the
  Microsoft Hardware Dev Center (needs an EV code-signing certificate).
- Windows 10 2004 or later, x64 only.
- Black frames only; the format and picture are fixed in `driver/vcam.cpp`.
- Written against the Microsoft AVStream samples and checked by compiling and linking against the
  real WDK headers and libraries; see the notes in the pull request for what has and has not been
  run on a Windows machine.

## Build

Needs Visual Studio with the C++ workload and the Windows Driver Kit (WDK).

```powershell
msbuild vcam.sln -p:Configuration=Release -p:Platform=x64
```

`driver\x64\Release\` receives `vcam.sys`; `bin\x64\Release\vcamctl.exe` the installer.
Without a WDK installed, `nuget restore packages.config -PackagesDirectory packages` first; see
`Directory.Build.props`.

## Customising

- **Resolutions / frame rate:** the `VCAM_YUY2_MODE(...)` lines and `VCAM_FRAME_INTERVAL` in
  `driver/vcam.cpp`.
- **Name shown to applications:** `VCam.DeviceDesc` and `VCam.FriendlyName` in `driver/vcam.inf`.

## Licensing

The driver's structure follows Microsoft's `avssamp` and `avshws` AVStream samples
(<https://github.com/microsoft/Windows-driver-samples>), which are under the Microsoft Public
License; that license is in `licenses/MS-PL.txt`.
