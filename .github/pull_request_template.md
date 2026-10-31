## What

<!-- One topic per pull request. Link the issue it addresses. -->

## How it was verified

- Windows build:
- Hardware (monitors, GPUs):
- Probes run / results:

## Checklist

- [ ] `./tools/build.ps1` and `dotnet build poc/Atrium.Poc.sln -c Release` pass
- [ ] Core or settings changes: `./tests/e2e-settings.ps1 -Package` and `./tests/pipe-protocol.ps1` pass
- [ ] No injection into Explorer/DWM, no system files touched, no security features disabled
- [ ] No personal content in screenshots or results
