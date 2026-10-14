# PipeWire Auto Router

A Qt6 patchbay GUI and headless watcher for reconnecting PipeWire app streams to
named devices. It is meant for setups like:

- Discord or WebRTC audio -> `obs-discord`
- A game or browser stream -> `obs-game`

It does not patch Helvum. It works beside Helvum by using the same PipeWire graph
that Helvum displays.

## Requirements

- Linux with PipeWire and PulseAudio compatibility enabled
- `pw-link`, `pw-dump`, and `pactl` available in `PATH`
- Qt 6 Widgets
- Python 3
- A user systemd session is required only for the optional background watcher

## Install A Release On KDE

Download `pipewire-auto-router-0.2.0-linux-x86_64.tar.gz` and `SHA256SUMS`,
then run:

```sh
sha256sum --check SHA256SUMS --ignore-missing
tar -xzf pipewire-auto-router-0.2.0-linux-x86_64.tar.gz
cd pipewire-auto-router-0.2.0-linux-x86_64
./install-kde-app.sh
```

Open the KDE application launcher and search for `PipeWire Auto Router`. The
installer copies the application into `~/.local/share/pipewire-auto-router`, so
the extracted release directory can be deleted afterward.

To update, extract the newer release and run `./install-kde-app.sh` again. Your
rules in `~/.config/pipewire-auto-router/rules.json` are preserved.

## Build And Run From Source

Install a C++17 compiler, CMake 3.20 or newer, and the Qt 6 development files.
Then, from the extracted source archive or repository:

```sh
./run-qt.sh
```

After the first build, this also opens the Qt6 GUI:

```sh
python3 pipewire_auto_router.py
```

Use it like a patchbay:

- Use `Create Fake Audio` to create a virtual sink like `obs-game` or
  `obs-discord`. Keep `Save and recreate automatically` enabled if the watcher
  should recreate it after PipeWire/Pulse restarts.
- Use `Auto-connect Status...` to inspect or remove saved fake audio devices and saved
  auto-reconnect links. It shows which links are connected now, which ones are
  waiting for a device, and which saved fake devices are running.
- Drag from an output port to an input port to connect them.
- Keep `Connect matching channels` enabled to connect stereo pairs together.
- When a source already has other links, the app opens a chooser. Check only the
  existing links you want disconnected, or keep all of them.
- Blue links are connections that already existed in PipeWire.
- Green links are connections made in this GUI session.
- Orange links are exact connections saved for auto-reconnect.
- Enable `Auto-reconnect new links` only when newly created links should be saved
  immediately.
- Keep `Auto-apply reconnect` enabled if the open GUI should keep restoring
  saved orange links after devices or streams come back.
- Auto-reconnect supports `output_* -> playback_*` and `monitor_* -> playback_*`
  links.
- Right-click a green session-created output/monitor-to-playback link to enable
  auto-reconnect for that exact connection. Existing blue links cannot be saved
  accidentally.
- Right-click an orange link to disable its saved auto-reconnect entry.
- Right-click a port and choose `Disconnect port links` to remove its current
  links.

For Discord/WebRTC, drag `WEBRTC VoiceEngine:output_FL` to
`obs-discord:playback_FL`; the matching right channel is connected too. For a
game, start the game so its stream appears, then drag its output to
`obs-game:playback_FL`.

Saved fake audio devices and auto-reconnect connections are saved to:

```text
~/.config/pipewire-auto-router/rules.json
```

## CLI

List the current graph:

```sh
python3 pipewire_auto_router.py --list
```

Apply saved auto-reconnect connections once:

```sh
python3 pipewire_auto_router.py --apply
```

Watch, recreate saved fake devices, and reconnect continuously:

```sh
python3 pipewire_auto_router.py --watch
```

Watch mode listens to `pw-dump --monitor` and reacts when PipeWire ports or
links change. It does not poll the full graph while idle; `interval_seconds` is
used only as the retry delay if the monitor connection exits.

Preview changes without changing links:

```sh
python3 pipewire_auto_router.py --apply --dry-run
```

## Autostart

After creating and saving auto-reconnect connections in the GUI:

```sh
./install-user-service.sh
```

That installs and starts a user systemd service that runs the watcher in your
desktop session. Check it with:

```sh
systemctl --user status pipewire-auto-router.service
```

The service uses the installed copy of the Python backend, so the downloaded or
extracted release folder is no longer needed.

## KDE Application Menu

Install the GUI in the KDE application launcher:

```sh
./install-kde-app.sh
```

The entry is named `PipeWire Auto Router` and appears in the Audio category. In
a source tree the installer builds a release binary first; in a binary release
it installs the included executable directly.

## Uninstall

From an extracted release or source tree, run:

```sh
./uninstall.sh
```

This removes the KDE launcher, installed application, and background service.
It intentionally keeps your routing rules at
`~/.config/pipewire-auto-router/rules.json`.

## Create A Release

Maintainers can build both source and Linux x86_64 binary archives with:

```sh
./make-release.sh
```

Artifacts and `SHA256SUMS` are written to `dist/`. The version comes from the
`VERSION` file. The release build runs the automated tests before packaging.

## Notes

Auto-reconnect uses exact PipeWire port names such as
`WEBRTC VoiceEngine:output_FL -> obs-discord:playback_FL`. That keeps it scoped
to the connection you chose instead of reconnecting every stream between two
devices.
