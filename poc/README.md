# Verification probes

Small, single-purpose programs that test one Windows behaviour each on a real interactive desktop and write their findings to `results/<probe>.json`. They are evidence for [docs/VERIFICATION.md](../docs/VERIFICATION.md), not product code: the product core is planned in C++20 (see [docs/TECH-DECISIONS.md](../docs/TECH-DECISIONS.md)), while the probes use C# so they can run without a C++ toolchain.

## Build

Requires Windows 10 19041 or later (Windows 11 recommended) and the .NET 8 SDK.

```powershell
dotnet build poc/Atrium.Poc.sln -c Release
$probe = "poc/Probe/bin/Release/net8.0-windows10.0.26100.0/win-x64/Atrium.Probe.exe"
```

Every probe takes an optional watchdog limit in seconds as its second argument (`Atrium.Probe.exe glass 60`). When the limit is reached the probe restores everything it changed and exits.

## Probes

| Command | Topic | What it does to your desktop |
| --- | --- | --- |
| `env` | P00 adapters, outputs, DPI, HDR, capture APIs | Nothing (read-only) |
| `wda` | P01a capture exclusion | Shows a small magenta window for ~2 s |
| `glass [tag]` | P01b live glass, echo, GPU cost, internal screenshot | Shows a click-through glass bar at the bottom for ~15 s |
| `wincap` | P01c per-window capture | Shows small test windows; captures (never saves) the visible app windows |
| `border` | P01d capture border | Shows a test window for ~2 s |
| `optionc` | P01e rebuilt backdrop, screenshot-visible glass | Shows test windows and glass in a free desktop area |
| `input` | P02 click-through strategies | **Moves the cursor and clicks** on its own windows; waits until you are idle |
| `taskbar` | P03/P07 hide taskbar, AppBar, crash recovery | **Hides the taskbar** for a few seconds and kills its own child process |
| `taskbar2` | P03b non-auto-hide taskbar | **Changes the taskbar auto-hide setting temporarily** and restores it |
| `entry` | P03c Start, Search, Quick Settings, Notification Center | **Presses Win, Win+S, Win+A, Win+N and Esc**; waits until you are idle |
| `trayhost` | P04b forwarding tray bridge | Registers a test tray icon and briefly becomes the tray host |
| `tasks` | P04c task list, activation, thumbnails | **Clicks** on its own window; waits until you are idle |
| `desktop` | P06 widgets and Show Desktop | **Presses Win+D twice**; waits until you are idle |
| `notify` | P05 notification listener | Reads the number of current notifications only |
| `echo` | P01f slow and small changes under the glass (round 2) | Shows a glass bar and small test windows for ~13 s |
| `perf [tag]` | P01h real-time per-frame timing (round 2) | Shows a glass bar and a counter window for ~10 s |
| `optionc2` | P01g dynamic per-window backdrop rebuild (round 2) | Shows moving test windows and glass in a free desktop area |
| `trayfail` | P04d tray bridge failure semantics (round 2) | Briefly becomes the tray host; test icons go to a fake tray, other apps to Explorer |
| `smto` | SendMessageTimeout timing reference | Starts a fake tray process only |
| `recovery <guard.exe>` | P07b takeover transaction + native guard (round 2) | **Hides the taskbar and changes auto-hide repeatedly**, kills/hangs its own child processes, restores everything |
| `desktop2` | P06b event-driven desktop widgets (round 2) | **Presses Win+D, invokes Show Desktop, clicks, toggles desktop icons on and back off**; waits until you are idle |
| `demo [dir]` | README images | Nothing on screen; renders over a synthetic background |

`TrayProbe` (WPF) runs the same tray scenario through ManagedShell for comparison:

```powershell
poc/TrayProbe/bin/Release/net8.0-windows/win-x64/Atrium.TrayProbe.exe <path-to-Atrium.Probe.exe> poc/results
```

Helper modes started by the probes themselves: `clicktarget`, `tbcore`, `tbguard`, `trayclient`, `faketray`, `sendto`, `txcore`.

## Native guard

`native/` is a CMake project (Visual Studio 2022 generator, static CRT):

```powershell
cmake --preset win-x64 -S native
cmake --build native/build/win-x64 --config Release
# then
$probe recovery 180 (Resolve-Path native/build/win-x64/Release/Atrium.Guard.exe)
```

## Hybrid GPUs

On laptops where the display is driven by the integrated GPU, NVIDIA drivers may still place a process on the discrete GPU, which breaks DXGI desktop duplication. `run-igpu.ps1` runs a probe with the per-app "power saving" GPU preference for the probe executable and removes that preference afterwards:

```powershell
pwsh poc/run-igpu.ps1 -Exe (Resolve-Path $probe) glass 70 igpu
```

## Safety rules the probes follow

- Every change to system state registers an undo action that runs on normal exit, exceptions, Ctrl+C and the watchdog.
- Probes that inject input wait until the user has been idle for at least 15 seconds and restore the cursor and the foreground window.
- Probes never store window titles, notification text or pixels of other applications in `results/`; screenshots of the live desktop (`results/**/*.png`) are git-ignored.
