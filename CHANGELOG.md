# Changelog

## 0.3.0 - 2026-10-24

- Reload saved reconnect rules in the background watcher so deleted routes stay
  deleted, and adopt rule edits even while the PipeWire graph is idle.
- Keep watching empty configurations and pause restoration on invalid files.
- Separate live connections and saved reconnect rules into dedicated tabs.
- Replace ambiguous routing checkboxes with explained connection and channel
  choices, group saved stereo channels, and distinguish forgetting a rule from
  removing it and disconnecting its live audio.
- Save rule removals atomically before disconnecting, coordinated with any
  reconnect already in progress, and preserve audio if saving fails.
- Add isolated Qt UI and watcher regression coverage to the release checks.
- Restart an active background watcher during upgrades so the new backend takes
  effect immediately.

## 0.2.1 - 2026-10-14

- Make the headless watcher react to PipeWire graph events instead of polling
  every two seconds, reducing reconnect latency and idle work.

## 0.2.0 - 2026-10-09

- Show whether each saved auto-connect link is connected, disconnected, disabled,
  or waiting for a source or destination device.
- Show whether saved fake audio devices are currently running.
- Add connected/running totals and manual status refresh to the saved-items view.
- Add a KDE application-menu installer and desktop launcher.
- Add stable per-user installation, autostart installation, and uninstallation.
- Add source and prebuilt Linux x86_64 release archives with SHA-256 checksums.

## 0.1.0

- Initial Qt6 PipeWire patchbay and headless auto-reconnect watcher.

