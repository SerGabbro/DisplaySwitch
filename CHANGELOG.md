# Changelog

All notable changes to DisplaySwitch are listed here. The format follows
[Keep a Changelog](https://keepachangelog.com/en/1.1.0/) and the project uses
[Semantic Versioning](https://semver.org/).

## 1.0.0 - 2026-11-21

First public release.

### Added
- `list`: table of connected monitors (ID, name, state, mode, position, primary) and a
  check of every profile against the monitors connected right now (`ready`, `MISSING`,
  `AMBIGUOUS`, `<- current`).
- `apply <profile>`: turns on only the profile's monitors (virtual displays included)
  and sets resolution, refresh rate, position and primary monitor.
- `version` and `help` commands.
- Monitors are matched by hardware ID (EDID model code), stable across reboots, driver
  reinstalls and port changes; `name:` is supported too.
- `refresh_rate` alone, without `width`/`height`, changes only the refresh rate and
  keeps the current resolution.
- Automatic rollback to the previous layout when a profile cannot be fully applied.
- Retries while Windows refuses display changes (lock screen, UAC prompt) and when a
  mode change fails right after a topology change.
- Relaunch into the user's session when started by a Windows service.
- One switch at a time, and a log file next to the exe.
- Icon, version info and manifest (per-monitor DPI aware, UTF-8) embedded in the exe.
