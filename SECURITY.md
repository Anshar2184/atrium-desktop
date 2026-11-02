# Security Policy

Atrium hides and restores the Windows taskbar, captures the screen for its glass effect and hosts tray icon messages, so security and privacy issues matter even at the prototype stage.

## Reporting a vulnerability

Please use GitHub's **private vulnerability reporting** (Security → Report a vulnerability) instead of a public issue. Include the Windows build, steps to reproduce and the impact you observed. You can expect an initial response within seven days.

## Scope

- Screen content leaving the GPU or being written to disk without the user's action.
- Ways for another process to drive Atrium's named pipes or tray bridge.
- Situations where the Windows taskbar is left hidden or unusable.
