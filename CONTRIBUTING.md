# Contributing to Atrium Desktop

Thanks for your interest. The most valuable contributions to Atrium right now are:

- **Reports from other machines**: stable Windows 11 builds, multiple monitors, HDR, high refresh rates, desktop icons enabled, AMD/NVIDIA-only systems. Describe what you saw and attach `%LOCALAPPDATA%\Atrium\logs\core.log` to an issue.
- **Design review**: the design language is in [docs/DESIGN.md](docs/DESIGN.md).
- **Compatibility reports** for tray icons, task grouping and Show Desktop behaviour with specific applications.

## Ground rules

- Atrium must never require disabling security features, replacing system files, injecting into Explorer or DWM, or killing Explorer. Proposals that depend on these will not be merged into the core.
- Prefer public, documented Windows APIs. If an undocumented behaviour is unavoidable, isolate it behind a compatibility module and explain it in the pull request.
- Report results honestly: a test is `PASS`, `FAIL` or `SKIP`. Missing hardware is a skip, not "should work".
- Do not commit screenshots or data that show other people's windows, notifications or personal content. Use `ATRIUM_PRIVACY=1` and a synthetic backdrop for images.

## Pull requests

1. Open an issue first for anything larger than a typo, so the approach can be agreed on.
2. Keep a pull request to one topic; match the style of the surrounding code and documents.
3. Make sure `./tools/build.ps1` (native core, settings app, portable package) passes, and try the change on the running desktop.
4. Describe how you verified the change and on which Windows build.

Documents under `docs/` are currently written in Chinese; English contributions and translations are welcome.

By contributing you agree that your contributions are licensed under the [MIT License](LICENSE).
