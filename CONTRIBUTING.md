# Contributing to Atrium Desktop

Thanks for your interest. Atrium is at the architecture and feasibility stage, so the most valuable contributions right now are:

- **Probe results from other machines**: stable Windows 11 builds, multiple monitors, HDR, high refresh rates, desktop icons enabled, AMD/NVIDIA-only systems. Run the probes described in [poc/README.md](poc/README.md) and attach the JSON files from `poc/results/` to an issue.
- **Design review** of the documents in [docs/](docs/); start with [DECISIONS.md](docs/DECISIONS.md), the single list of current decisions.
- **Compatibility reports** for tray icons, task grouping and Show Desktop behaviour with specific applications.

## Ground rules

- Atrium must never require disabling security features, replacing system files, injecting into Explorer or DWM, or killing Explorer. Proposals that depend on these will not be merged into the core.
- Prefer public, documented Windows APIs. If an undocumented behaviour is unavoidable, isolate it behind a compatibility module and list it in `docs/RISKS.md`.
- Report results honestly: a probe result is `Passed`, `Failed` or `Inconclusive`. Missing hardware is `Inconclusive`, not "should work".
- Do not commit screenshots or data that show other people's windows, notifications or personal content. Use the synthetic `demo` probe for images.

## Pull requests

1. Open an issue first for anything larger than a typo, so the approach can be agreed on.
2. Keep a pull request to one topic; match the style of the surrounding code and documents.
3. Make sure `dotnet build poc/Atrium.Poc.sln -c Release` and the native build (`cmake --preset win-x64 -S native`, then `cmake --build native/build/win-x64 --config Release`) pass.
4. Describe how you verified the change and on which Windows build.

Documents under `docs/` are currently written in Chinese; English contributions and translations are welcome.

By contributing you agree that your contributions are licensed under the [MIT License](LICENSE).
