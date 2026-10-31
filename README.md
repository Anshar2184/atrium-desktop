# Atrium Desktop

**A native glass desktop for Windows 11** — a floating taskbar and desktop widgets rendered with real-time refraction, in one consistent design language. Explorer and the Windows shell stay in place; Atrium replaces what you look at, not the operating system.

[简体中文](README.zh-CN.md)

> **Status: feasibility verified, product not yet built.** This repository currently contains the architecture review, the verification report and the probe programs used to test every high-risk Windows integration on a real desktop. There is no installable app yet.

<p align="center">
  <img src="docs/images/glass-liquid.png" alt="Liquid glass material rendered by the probe" width="80%"><br>
  <img src="docs/images/glass-frosted.png" alt="Frosted glass material rendered by the probe" width="80%"><br>
  <sub>Liquid and frosted presets of the same D3D11 material, rendered by <code>poc/Probe</code> over a synthetic background.</sub>
</p>

## Goals

- **One material engine** for the taskbar, widgets and future launcher / control center: rounded and capsule shapes, edge refraction, subtle dispersion, GPU blur, soft highlights. Liquid and frosted looks are presets of the same pipeline.
- **Native and light**: C++20, Win32, Direct3D 11, DirectComposition, DirectWrite. No browser runtime, no service, no admin rights.
- **Safe to live with**: an independent guard process restores the Windows taskbar if Atrium crashes; no injection into Explorer or DWM, no system files touched.

## What has been verified

All results come from probes run on Windows 11 build 26300 with a hybrid Intel Arc / RTX 4060 laptop. Full details, numbers and decisions: [docs/VERIFICATION.md](docs/VERIFICATION.md) (Chinese).

| Area | Result |
| --- | --- |
| Live refraction from Windows.Graphics.Capture with self-exclusion | Works; 0.16 ms GPU per frame on the iGPU for a 1500×196 bar |
| Self-capture echo | Exists; a coarse backdrop comparison removes it (45 → 8 capture fps, real changes still drawn) |
| Glass visible in ordinary screenshots | Works by rebuilding the backdrop from per-window captures instead of excluding Atrium |
| Click-through corners and shadows | Works with a click-through visual window plus a region-shaped input window |
| Hiding the Explorer taskbar, crash recovery | Works; guard restores the taskbar 128 ms after the core is killed |
| Start, Search, Quick Settings, Notification Center | Open normally while the Explorer taskbar is hidden |
| Third-party tray icons | Works with a forwarding tray bridge; a few icons from apps that ignore `TaskbarCreated` need a fallback |
| Task switching and live thumbnails | Works after a user click on the bar; DWM thumbnails render |
| Widgets during Show Desktop (Win+D) | Works with public top-level windows, no WorkerW embedding |
| Notifications | Readable without packaging; live change events need package identity, so not in the first release |

Not yet verified: multiple monitors, HDR, high refresh rates, Explorer restarts, sleep/resume, long-running stability and stable (non-Insider) Windows 11 builds.

## Repository layout

```text
docs/          architecture review, verification report, risks, roadmap (Chinese)
poc/Probe      verification probes: capture, glass renderer, input, taskbar, tray, widgets
poc/TrayProbe  ManagedShell comparison probe (WPF)
poc/results    raw probe results (JSON)
```

Start with [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md), then [docs/VERIFICATION.md](docs/VERIFICATION.md) and [docs/ROADMAP.md](docs/ROADMAP.md).

## Running the probes

The probes need Windows 10 19041+ (Windows 11 recommended) and the .NET 8 SDK. Some of them hide the taskbar or inject clicks and keys; read [poc/README.md](poc/README.md) before running anything.

```powershell
dotnet build poc/Atrium.Poc.sln -c Release
poc/Probe/bin/Release/net8.0-windows10.0.26100.0/win-x64/Atrium.Probe.exe env
```

## Contributing

The project is at the architecture stage, so design feedback, Windows compatibility reports and probe results from other hardware are the most useful contributions right now. See [CONTRIBUTING.md](CONTRIBUTING.md).

## License

[MIT](LICENSE). Visual inspiration comes from Apple's Liquid Glass; Atrium is not affiliated with Apple or Microsoft.
