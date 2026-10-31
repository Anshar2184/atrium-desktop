<div align="center">

# Atrium Desktop

**A native glass desktop for Windows 11**

Floating taskbar with real-time refraction · desktop widgets in one material · Explorer stays, crashes recover

[![build](https://github.com/wynter-yaxley/atrium-desktop/actions/workflows/build.yml/badge.svg)](https://github.com/wynter-yaxley/atrium-desktop/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![Platform: Windows 11](https://img.shields.io/badge/platform-Windows%2011-0078D6)](#status)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?logo=cplusplus&logoColor=white)](native/)
[![WinUI 3 settings](https://img.shields.io/badge/settings-WinUI%203%20%C2%B7%20.NET%2010-512BD4?logo=dotnet&logoColor=white)](settings/)
[![Status](https://img.shields.io/badge/status-prototype-orange)](docs/VERIFICATION.md)

[简体中文](README.md) | English

</div>

> [!NOTE]
> **v0.2 development build.** A C++ glass taskbar that coexists with the Windows taskbar: pinned and running apps, click to switch windows, clock and calendar, plus a settings app that adjusts the look live. It never hides or replaces the Windows taskbar; full replacement waits until recovery and tray handling pass on stable Windows 11.

<p align="center">
  <img src="docs/images/v02-bar-liquid.png" alt="Liquid preset" width="80%"><br>
  <img src="docs/images/v02-bar-frosted.png" alt="Frosted preset" width="80%"><br>
  <img src="docs/images/v02-bar-light.png" alt="Dark text chosen automatically over a light backdrop" width="80%"><br>
  <sub>On screen: liquid (strong refraction and dispersion after macOS Liquid Glass), frosted, and dark ink chosen automatically over a light backdrop (test pattern behind)</sub>
</p>

<p align="center">
  <img src="docs/images/v02-bar-hover.png" alt="Hover" width="80%"><br>
  <img src="docs/images/v02-settings-pins.png" alt="Settings" width="60%"><br>
  <sub>Hovered icons lift and show their name above; the settings app adjusts material and size live and manages pinned apps (UI in Chinese for now)</sub>
</p>

## What it is

Atrium redesigns the layer you look at every day: a floating glass taskbar and a set of desktop widgets sharing one material and one design language. Explorer and the Windows shell stay in place; Start, Search, the Notification Center and Quick Settings keep working.

- **One material engine**: rounded and capsule shapes, edge refraction, subtle dispersion, GPU blur, soft highlights. Liquid and frosted looks are presets of one pipeline.
- **Native and light**: C++20, Win32, Direct3D 11, DirectComposition, DirectWrite. No browser runtime, no service, no admin rights.
- **Safe to live with**: an independent native guard restores the Windows taskbar if Atrium crashes or hangs; no injection into Explorer or DWM, no system files touched.

## Quick start

1. Download the `Atrium-win-x64` artifact from the latest successful run in [Actions](https://github.com/wynter-yaxley/atrium-desktop/actions/workflows/build.yml), or build it as described below.
2. Unzip and run `Atrium.Core.exe`. The glass taskbar appears at the bottom of the screen; on first run it imports your current Windows taskbar pins.
3. Left-click an app to open it, bring it to the front or minimize it; middle-click for a new window; right-click to pin, unpin or close. Click the clock for the calendar and notifications.
4. Double-click the tray icon (or right-click it → the first menu item) to open the settings: material, size, pinned apps and more.
5. Right-click the tray icon → the last menu item to quit. Nothing is written to the registry and no service is installed; settings live in `%LOCALAPPDATA%\Atrium`.

## Status

All verification ran on a Windows 11 build 26300 (Insider) laptop with Intel Arc + RTX 4060. Numbers: [docs/VERIFICATION.md](docs/VERIFICATION.md) (Chinese). Current decisions: [docs/DECISIONS.md](docs/DECISIONS.md).

| Area | Result |
|---|---|
| v0.2 glass taskbar and settings | ✅ coexistence mode; pinned and running apps, click to switch (5 real-click checks pass); with a backdrop changing every frame, present p95 0.8 ms, about 4 % of one core, 144 MB; 12 end-to-end settings checks and 7 protocol checks pass |
| Live glass | ✅ WGC capture with self-exclusion + D3D11; iGPU frame work p95 2.4 ms, content lag p95 1 frame |
| Self-capture echo | ✅ exact comparison with the last presented backdrop; slow gradients, 1 px cursors and low-contrast scrolling text are never lost |
| Click-through | ✅ click-through visual window + region-shaped input window; shadows and transparent corners pass clicks through |
| Taskbar takeover and recovery | ✅ takeover protocol v2 + native guard, with a real AppBar: 15 fault scenarios (kill, hang, startup stall, both processes dying, lock timeout, restore not taking effect…) all restore the user's original settings (single monitor; a real Explorer restart is untested) |
| Native entry points | ✅ Start, Search, Quick Settings and Notification Center work while the Explorer taskbar is hidden |
| Third-party tray icons | ⚠️ forwarding bridge verified on normal and failure paths; Explorer restarts and real-app compatibility not yet tested |
| Desktop widgets | ✅ event-driven state machine; 13 scenarios incl. Win+D, the Show Desktop button and desktop icons (single monitor) |
| Glass in screenshots | ⚠️ built-in screenshot by default; rebuilding the backdrop from per-window captures works dynamically but still needs synthesized window shadows |
| Notification center | ⏸ deferred, not in the first release |

**Not yet verified**: multiple monitors, HDR, high refresh rates, Explorer restarts, sleep/resume, long-running stability and stable (non-Insider) Windows 11 builds.

## Repository layout

```text
native/core    Atrium.Core: glass bar, tray icon, settings pipe (C++20, D3D11, DirectComposition)
native/guard   native taskbar recovery guard Atrium.Guard (C++20, static CRT)
settings/      settings app Atrium.Settings (C# / WinUI 3)
tools/         build.ps1 packaging script, icon generator
tests/         end-to-end and protocol test scripts
docs/          architecture, current decisions, verification report, settings protocol, risks, roadmap (Chinese)
poc/Probe      verification probes: capture, glass renderer, input, taskbar, tray, widgets, recovery
poc/TrayProbe  ManagedShell comparison probe
poc/results    raw probe results (JSON)
```

## Build

Requires Windows 10 19041+ (Windows 11 recommended), the .NET 10 SDK and Visual Studio or Build Tools with the C++ workload and a Windows SDK (CMake 3.25+).

```powershell
# native core, guard and settings app into the portable folder dist/Atrium (-Zip also zips it)
./tools/build.ps1

# tests (briefly start Atrium and drive the settings window; the settings file is restored)
./tests/e2e-settings.ps1 -Package
./tests/pipe-protocol.ps1
./tests/e2e-bar.ps1          # moves the mouse and clicks; runs only after 30 s without user input

# verification probes
dotnet build poc/Atrium.Poc.sln -c Release
```

> [!WARNING]
> Some probes hide the taskbar, inject keys and clicks, or temporarily change the taskbar auto-hide or desktop-icon settings. They wait until you are idle and restore everything afterwards; read [poc/README.md](poc/README.md) before running them.

## Contributing

Design review and probe results from other hardware are the most useful contributions right now, especially stable Windows 11, multiple monitors and HDR. See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

[MIT](LICENSE). Visual inspiration comes from Apple's Liquid Glass; Atrium is not affiliated with Apple or Microsoft.
