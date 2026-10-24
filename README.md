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

Download `pipewire-auto-router-0.3.0-linux-x86_64.tar.gz` and `SHA256SUMS`,
then run:

```sh
sha256sum --check SHA256SUMS --ignore-missing
tar -xzf pipewire-auto-router-0.3.0-linux-x86_64.tar.gz
cd pipewire-auto-router-0.3.0-linux-x86_64
./install-kde-app.sh
```

Open the KDE application launcher and search for `PipeWire Auto Router`. The
installer copies the application into `~/.local/share/pipewire-auto-router`, so
the extracted release directory can be deleted afterward.

To update, extract the newer release and run `./install-kde-app.sh` again. Your
rules in `~/.config/pipewire-auto-router/rules.json` are preserved. If the
background watcher is running, the installer restarts it to load the update.

## Build And Run From Source

Install a C++17 compiler, CMake 3.20 or newer, and the Qt 6 Widgets and Test development files.
Then, from the extracted source archive or repository:

```sh
./run-qt.sh
```

After the first build, this also opens the Qt6 GUI:

```sh
python3 pipewire_auto_router.py
```

The window separates **Live connections** from **Saved reconnect rules**.

In **Live connections**, drag an output port to an input port. Choose how the
next connection should behave before drawing it:

- `Connect once` connects now without saving a reconnect rule.
- `Save reconnect rule` connects now and remembers the route for later.
- `Match all audio channels` connects matching channels, including both sides of
  a stereo pair. `Only the two dragged ports` creates one exact link.
- `Show: Audio ports / All ports` filters the display. Hidden links keep working.

The description below these choices explains their effects. Orange lines have
saved reconnect rules; blue lines are live connections. Right-click a supported
line to save its rule, forget its rule, or disconnect it. Auto-reconnect supports
`output_* -> playback_*` and `monitor_* -> playback_*` links.

In **Saved reconnect rules**, each route groups its channels. Select the route
to act on all its channels, or expand it to select one channel:

- `Remove route & disconnect` deletes the selected rules and disconnects their
  audio now. They will no longer be restored by this router.
- `Forget route (keep live audio)` deletes the rules while leaving live audio
  connected. It stops future restoration by this router.
- `Reconnect saved now` tries existing enabled rules once. It does not save any
  additional live connections. Offline ports stay waiting.

Saved rules reconnect automatically while the GUI or background watcher is
running. Closing the GUI does not stop an installed background watcher. Saved
rule edits take effect in the watcher without restarting it. `Refresh status`
only reads the graph and saved rules; it does not connect anything.

When a source already has other destinations, the app lists the affected links.
`Add destination (keep existing)` keeps them playing. `Move audio (disconnect
listed)` disconnects the listed links and removes their saved rules before
connecting the new destination.

Use `Create virtual audio device…` to create a sink like `obs-game` or
`obs-discord`. The dialog explains its recreation option. `Saved virtual
devices…` manages saved device definitions separately from audio routes.

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
links change. It also checks the configuration file metadata every half second
so saved edits take effect while the graph is idle. It does not poll the full
graph while idle; `interval_seconds` is used only as the retry delay if the
monitor connection exits. Empty configurations remain watchable, and a broken
configuration pauses restoration until it is fixed instead of using stale rules.

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

To run the backend and isolated Qt UI regression tests locally:

```sh
cmake -S . -B qt-build
cmake --build qt-build
ctest --test-dir qt-build --output-on-failure
```

The Qt tests run offscreen with fake audio commands and a temporary configuration.

## Notes

Auto-reconnect uses exact PipeWire port names such as
`WEBRTC VoiceEngine:output_FL -> obs-discord:playback_FL`. That keeps it scoped
to the connection you chose instead of reconnecting every stream between two
devices.
