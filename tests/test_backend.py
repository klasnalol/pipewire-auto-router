import contextlib
import fcntl
import itertools
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

import pipewire_auto_router
from pipewire_auto_router import (
    Config,
    ConnectionRule,
    FakeDevice,
    GraphSnapshot,
    PipeWireGraphMonitor,
    PipeWireBackend,
    choose_pairs,
    is_valid_device_name,
    is_supported_auto_connection,
    parse_links,
    parse_port_lines,
    port_channel,
    watch_config,
)


class FakeBackend(PipeWireBackend):
    def __init__(self, graph):
        self.graph = graph
        self.connected = []
        self.disconnected = []
        self.created_devices = []
        self.sinks = set()

    def snapshot(self):
        return self.graph

    def connect(self, output, input_, linger):
        self.connected.append((output, input_, linger))

    def disconnect(self, output, input_):
        self.disconnected.append((output, input_))

    def list_sinks(self):
        return set(self.sinks)

    def ensure_devices(self, devices, dry_run=False):
        messages = []
        for device in devices:
            if device.name in self.sinks:
                continue
            if dry_run:
                messages.append(f"{device.name}: would create fake audio device {device.name}")
            else:
                self.sinks.add(device.name)
                self.created_devices.append((device.name, device.description, device.channels))
                messages.append(f"{device.name}: create fake audio device {device.name}")
        return messages


class BackendParsingTests(unittest.TestCase):
    def test_stalled_audio_command_is_bounded_and_reported(self):
        command = ["pw-link", "-L", "App:output_FL", "Ryzen:playback_FL"]
        with mock.patch.object(
            pipewire_auto_router.subprocess, "run", side_effect=pipewire_auto_router.subprocess.TimeoutExpired(command, 6)
        ) as run:
            with self.assertRaisesRegex(pipewire_auto_router.PipeWireCommandError, "timed out"):
                pipewire_auto_router.run_command(command)
        self.assertEqual(run.call_args.kwargs["timeout"], 6)

    def test_parse_port_lines_strips_ids(self):
        text = "  46 obs-game:monitor_FL\nChromium:output_FR\n"
        self.assertEqual(parse_port_lines(text), ["obs-game:monitor_FL", "Chromium:output_FR"])

    def test_parse_links_uses_only_forward_link_entries(self):
        text = """Chromium:output_FL
  |-> alsa_output.card:playback_FL
obs-discord:playback_FL
  |<- WEBRTC VoiceEngine:output_FL
WEBRTC VoiceEngine:output_FL
  |-> obs-discord:playback_FL
"""
        self.assertEqual(
            parse_links(text),
            {
                ("Chromium:output_FL", "alsa_output.card:playback_FL"),
                ("WEBRTC VoiceEngine:output_FL", "obs-discord:playback_FL"),
            },
        )

    def test_channel_matching(self):
        self.assertEqual(port_channel("Chromium:output_FL"), "FL")
        self.assertEqual(port_channel("obs-game:playback_FR"), "FR")
        self.assertIsNone(port_channel("bluez_midi.server:out"))

    def test_choose_stereo_pairs(self):
        outputs = ["App:output_FL", "App:output_FR"]
        inputs = ["obs-game:playback_FR", "obs-game:playback_FL"]
        self.assertEqual(
            choose_pairs(outputs, inputs),
            (
                [("App:output_FL", "obs-game:playback_FL"), ("App:output_FR", "obs-game:playback_FR")],
                [],
            ),
        )

    def test_audio_filter_excludes_named_video_stream(self):
        backend = PipeWireBackend()
        graph = GraphSnapshot(
            outputs=[],
            inputs=[],
            links=set(),
            node_text={"kwin_wayland": "kwin_wayland kwin-screencast-spotify Stream/Output/Video"},
        )
        self.assertFalse(backend._media_matches("kwin_wayland:output_1", graph, "audio"))
        self.assertTrue(backend._media_matches("kwin_wayland:output_1", graph, "any"))

    def test_exact_connection_rule_only_reconnects_selected_pair(self):
        graph = GraphSnapshot(
            outputs=["App:output_FL", "App:output_FR"],
            inputs=["obs-game:playback_FL", "obs-game:playback_FR"],
            links={("App:output_FR", "obs-game:playback_FR")},
            node_text={},
        )
        backend = FakeBackend(graph)
        config = Config(
            interval_seconds=2.0,
            rules=[],
            devices=[],
            connections=[
                ConnectionRule(
                    name="Only left channel",
                    output="App:output_FL",
                    input="obs-game:playback_FL",
                )
            ],
        )

        messages = backend.apply_config(config)

        self.assertEqual(backend.connected, [("App:output_FL", "obs-game:playback_FL", True)])
        self.assertEqual(backend.disconnected, [])
        self.assertEqual(messages, ["Only left channel: connect App:output_FL -> obs-game:playback_FL"])

    def test_auto_reconnect_does_not_disconnect_other_links(self):
        graph = GraphSnapshot(
            outputs=["App:output_FL"],
            inputs=["headphones:playback_FL", "obs-game:playback_FL"],
            links={("App:output_FL", "headphones:playback_FL")},
            node_text={},
        )
        backend = FakeBackend(graph)
        messages = backend.apply_config(
            Config(
                interval_seconds=2.0,
                rules=[],
                devices=[],
                connections=[
                    ConnectionRule(
                        name="App to OBS",
                        output="App:output_FL",
                        input="obs-game:playback_FL",
                        disconnect_others=True,
                    )
                ],
            )
        )

        self.assertEqual(backend.disconnected, [])
        self.assertEqual(backend.connected, [("App:output_FL", "obs-game:playback_FL", True)])
        self.assertEqual(messages, ["App to OBS: connect App:output_FL -> obs-game:playback_FL"])

    def test_monitor_output_to_playback_is_supported_for_auto_reconnect(self):
        self.assertTrue(
            is_supported_auto_connection(
                "obs-discord:monitor_FL",
                "alsa_output.usb-Generic_MOONDROP_EDGE_2_20240918905926-00.analog-stereo:playback_FL",
            )
        )
        graph = GraphSnapshot(
            outputs=["obs-discord:monitor_FL"],
            inputs=["alsa_output.usb-Generic_MOONDROP_EDGE_2_20240918905926-00.analog-stereo:playback_FL"],
            links=set(),
            node_text={},
        )
        backend = FakeBackend(graph)
        messages = backend.apply_config(
            Config(
                interval_seconds=2.0,
                rules=[],
                devices=[],
                connections=[
                    ConnectionRule(
                        name="OBS monitor to headphones",
                        output="obs-discord:monitor_FL",
                        input="alsa_output.usb-Generic_MOONDROP_EDGE_2_20240918905926-00.analog-stereo:playback_FL",
                    )
                ],
            )
        )

        self.assertEqual(
            backend.connected,
            [
                (
                    "obs-discord:monitor_FL",
                    "alsa_output.usb-Generic_MOONDROP_EDGE_2_20240918905926-00.analog-stereo:playback_FL",
                    True,
                )
            ],
        )
        self.assertEqual(
            messages,
            [
                "OBS monitor to headphones: connect obs-discord:monitor_FL -> "
                "alsa_output.usb-Generic_MOONDROP_EDGE_2_20240918905926-00.analog-stereo:playback_FL"
            ],
        )

    def test_fake_device_is_created_before_connections(self):
        backend = FakeBackend(
            GraphSnapshot(
                outputs=["obs-discord:monitor_FL"],
                inputs=["obs-discord:playback_FL"],
                links=set(),
                node_text={},
            )
        )
        config = Config(
            interval_seconds=2.0,
            rules=[],
            devices=[FakeDevice(name="obs-discord", description="OBS Discord Audio", channels=2)],
            connections=[
                ConnectionRule(
                    name="OBS monitor loop",
                    output="obs-discord:monitor_FL",
                    input="obs-discord:playback_FL",
                )
            ],
        )

        messages = backend.apply_config(config)

        self.assertEqual(backend.created_devices, [("obs-discord", "OBS Discord Audio", 2)])
        self.assertEqual(messages[0], "obs-discord: create fake audio device obs-discord")

    def test_fake_device_name_validation(self):
        self.assertTrue(is_valid_device_name("obs-discord"))
        self.assertTrue(is_valid_device_name("obs_game.2"))
        self.assertFalse(is_valid_device_name("obs discord"))
        self.assertFalse(is_valid_device_name("obs;discord"))


class WatcherTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.path = Path(self.directory.name) / "rules.json"
        self.output = "obs-discord:monitor_FL"
        self.ryzen = "alsa_output.pci-0000_0e_00.6.analog-stereo:playback_FL"
        self.headphones = "headphones:playback_FL"
        self.graph = GraphSnapshot(
            outputs=[self.output], inputs=[self.ryzen, self.headphones], links=set(), node_text={}
        )
        self.backend = FakeBackend(self.graph)

    def save_connection(self, input_):
        Config(2.0, [], [ConnectionRule("Saved audio", self.output, input_)], []).save(self.path)

    def watch_events(self, *events):
        monitor = mock.MagicMock()
        monitor.__enter__.return_value = monitor
        pending = iter(events)

        def next_event(*args, **kwargs):
            event = next(pending, KeyboardInterrupt())
            if isinstance(event, BaseException):
                raise event
            return event() if callable(event) else event

        monitor.wait_for_change.side_effect = next_event
        with mock.patch.object(pipewire_auto_router, "PipeWireGraphMonitor", return_value=monitor), mock.patch(
            "sys.stdout"
        ), self.assertRaises(KeyboardInterrupt):
            watch_config(self.backend, self.path)
        return monitor

    def test_watcher_does_not_restore_a_deleted_ryzen_connection(self):
        self.save_connection(self.ryzen)

        def delete_rule_and_disconnect():
            Config(2.0, [], [], []).save(self.path)
            self.graph.links.clear()
            return True

        self.watch_events(delete_rule_and_disconnect, True)

        self.assertEqual(
            self.backend.connected, [(self.output, self.ryzen, True)], "Deleted Ryzen route was restored by the watcher"
        )

    def test_watch_applies_immediately_after_graph_change_without_polling(self):
        self.save_connection(self.ryzen)

        def disconnect():
            self.graph.links.clear()
            return True

        with mock.patch.object(pipewire_auto_router.time, "sleep") as sleep:
            self.watch_events(disconnect)

        self.assertEqual(self.backend.connected, [(self.output, self.ryzen, True)] * 2)
        sleep.assert_not_called()

    def test_idle_watcher_does_not_poll_full_graph(self):
        self.save_connection(self.ryzen)
        self.backend.snapshot = mock.Mock(wraps=self.backend.snapshot)

        monitor = self.watch_events(None, None, None)

        self.backend.snapshot.assert_called_once_with()
        self.assertTrue(all(call.kwargs.get("timeout") is not None for call in monitor.wait_for_change.call_args_list))

    def test_empty_watcher_adopts_new_rule_while_graph_is_idle(self):
        Config(2.0, [], [], []).save(self.path)

        def add_rule():
            self.save_connection(self.ryzen)
            return None

        self.watch_events(None, add_rule)

        self.assertEqual(self.backend.connected, [(self.output, self.ryzen, True)])

    def test_idle_config_edit_replaces_the_old_target(self):
        self.save_connection(self.ryzen)

        def change_target():
            self.save_connection(self.headphones)
            self.graph.links.clear()
            return None

        self.watch_events(change_target, True)

        self.assertEqual(
            self.backend.connected, [(self.output, self.ryzen, True), (self.output, self.headphones, True)]
        )

    def test_missing_config_drops_old_rules_and_recovers_when_recreated(self):
        self.save_connection(self.ryzen)

        def remove_config():
            self.path.unlink()
            self.graph.links.clear()
            return None

        def recreate_config():
            self.assertEqual(self.backend.connected, [(self.output, self.ryzen, True)])
            self.save_connection(self.headphones)
            return None

        self.watch_events(remove_config, True, recreate_config)

        self.assertEqual(
            self.backend.connected, [(self.output, self.ryzen, True), (self.output, self.headphones, True)]
        )

    def test_malformed_config_pauses_old_rules_and_recovers(self):
        self.save_connection(self.ryzen)

        def corrupt_config():
            self.path.write_text("{", encoding="utf-8")
            self.graph.links.clear()
            return True

        def repair_config():
            self.assertEqual(self.backend.connected, [(self.output, self.ryzen, True)])
            self.save_connection(self.headphones)
            return None

        with mock.patch("sys.stderr") as stderr:
            self.watch_events(corrupt_config, True, repair_config)

        errors = "".join(call.args[0] for call in stderr.write.call_args_list)
        self.assertEqual(errors.count("reconnect paused"), 1)
        self.assertEqual(
            self.backend.connected, [(self.output, self.ryzen, True), (self.output, self.headphones, True)]
        )

    def test_watcher_blocks_config_writers_during_load_and_apply(self):
        self.save_connection(self.ryzen)
        lock_path = self.path.with_name(self.path.name + ".lock")
        load = Config.load
        apply = self.backend.apply_config

        def assert_writer_is_blocked():
            with lock_path.open("a") as competing_writer:
                with self.assertRaises(BlockingIOError):
                    fcntl.flock(competing_writer, fcntl.LOCK_EX | fcntl.LOCK_NB)

        def locked_load(path):
            assert_writer_is_blocked()
            return load(path)

        def locked_apply(config, dry_run=False):
            assert_writer_is_blocked()
            return apply(config, dry_run=dry_run)

        with mock.patch.object(Config, "load", side_effect=locked_load) as load_spy, mock.patch.object(
            self.backend, "apply_config", side_effect=locked_apply
        ) as apply_spy:
            self.watch_events()

        load_spy.assert_called_once_with(self.path)
        apply_spy.assert_called_once()
        with lock_path.open("a") as writer:
            fcntl.flock(writer, fcntl.LOCK_EX | fcntl.LOCK_NB)

    def test_monitor_exit_restarts_and_reloads_current_config(self):
        self.save_connection(self.ryzen)
        first = mock.MagicMock()
        first.__enter__.return_value = first
        first.wait_for_change.return_value = False
        second = mock.MagicMock()
        second.__enter__.return_value = second
        second.wait_for_change.side_effect = KeyboardInterrupt

        def change_rules_during_retry(delay):
            self.save_connection(self.headphones)
            self.graph.links.clear()

        with mock.patch.object(pipewire_auto_router, "PipeWireGraphMonitor", side_effect=[first, second]), mock.patch.object(
            pipewire_auto_router.time, "sleep", side_effect=change_rules_during_retry
        ) as sleep, mock.patch("sys.stdout"), self.assertRaises(KeyboardInterrupt):
            watch_config(self.backend, self.path)

        sleep.assert_called_once_with(2.0)
        self.assertEqual(
            self.backend.connected, [(self.output, self.ryzen, True), (self.output, self.headphones, True)]
        )


class GraphMonitorTests(unittest.TestCase):
    @contextlib.contextmanager
    def pipe_monitor(self, data):
        read_fd, write_fd = os.pipe()
        stream = os.fdopen(read_fd, "rb", buffering=0)
        process = mock.Mock(stdout=stream)
        process.poll.return_value = 0
        try:
            os.write(write_fd, data)
            with mock.patch.object(pipewire_auto_router.subprocess, "Popen", return_value=process) as popen:
                with PipeWireGraphMonitor() as monitor:
                    yield monitor, write_fd, popen
        finally:
            stream.close()
            with contextlib.suppress(OSError):
                os.close(write_fd)

    def test_graph_monitor_ignores_client_events_and_wakes_for_new_port(self):
        data = (
            b'[{"id": 1, "type": "PipeWire:Interface:Client"}]\n'
            b'[{"id": 2, "type": "PipeWire:Interface:Client"}]\n'
            b'[{"id": 3, "type": "PipeWire:Interface:Port"}]\n'
        )
        with self.pipe_monitor(data) as (monitor, _, popen):
            self.assertTrue(monitor.wait_for_change(timeout=0.1))

        popen.assert_called_once_with(
            ["pw-dump", "--monitor", "--raw", "--no-colors"],
            stdout=pipewire_auto_router.subprocess.PIPE,
            stderr=pipewire_auto_router.subprocess.DEVNULL,
            bufsize=0,
        )

    def test_buffered_graph_events_are_processed_without_waiting_on_pipe(self):
        data = (
            b'[{"id": 1, "type": "PipeWire:Interface:Port"}]\n'
            b'[{"id": 2, "type": "PipeWire:Interface:Port"}]\n'
            b'[{"id": 1, "info": null}]\n'
        )
        with mock.patch.object(pipewire_auto_router.select, "select", wraps=pipewire_auto_router.select.select) as select:
            with self.pipe_monitor(data) as (monitor, _, _):
                self.assertTrue(monitor.wait_for_change(timeout=0.1))
                self.assertTrue(monitor.wait_for_change(timeout=0.1))

        self.assertEqual(select.call_count, 1)

    def test_partial_monitor_line_times_out_and_is_completed_later(self):
        with self.pipe_monitor(b'[]\n[{"id":') as (monitor, writer, _):
            self.assertIsNone(monitor.wait_for_change(timeout=0.01))
            os.write(writer, b'2,"type":"PipeWire:Interface:Port"}]\n')
            self.assertTrue(monitor.wait_for_change(timeout=0.1))

    def test_monitor_distinguishes_pipe_eof_from_timeout(self):
        with self.pipe_monitor(b"[]\n") as (monitor, writer, _):
            self.assertIsNone(monitor.wait_for_change(timeout=0.01))
            os.close(writer)
            self.assertFalse(monitor.wait_for_change(timeout=0.1))

    def test_busy_client_events_do_not_starve_timeout(self):
        data = (
            b'[]\n'
            b'[{"id": 1, "type": "PipeWire:Interface:Client"}]\n'
            b'[{"id": 2, "type": "PipeWire:Interface:Client"}]\n'
            b'[{"id": 3, "type": "PipeWire:Interface:Port"}]\n'
        )
        with self.pipe_monitor(data) as (monitor, _, _):
            with mock.patch.object(pipewire_auto_router.time, "monotonic", side_effect=itertools.count(0, 0.1)):
                self.assertIsNone(monitor.wait_for_change(timeout=0.5))
            self.assertTrue(monitor.wait_for_change(timeout=0.1))


class MainEntrypointTests(unittest.TestCase):
    def test_watch_starts_with_empty_config_and_uses_custom_path(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "custom-rules.json"
            Config(2.0, [], [], []).save(path)
            with mock.patch.object(pipewire_auto_router, "watch_config") as watch:
                self.assertEqual(pipewire_auto_router.main(["--watch", "--config", str(path), "--dry-run"]), 0)

        watch.assert_called_once_with(mock.ANY, path, dry_run=True)

    def test_apply_once_blocks_config_writers_while_connecting(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "rules.json"
            Config(2.0, [], [], []).save(path)
            lock_path = path.with_name(path.name + ".lock")
            backend = mock.Mock()

            def apply_while_locked(config, dry_run=False):
                with lock_path.open("a") as competing_writer:
                    with self.assertRaises(BlockingIOError):
                        fcntl.flock(competing_writer, fcntl.LOCK_EX | fcntl.LOCK_NB)
                return []

            backend.apply_config.side_effect = apply_while_locked
            with mock.patch.object(pipewire_auto_router, "PipeWireBackend", return_value=backend), mock.patch("sys.stdout"):
                self.assertEqual(pipewire_auto_router.main(["--apply", "--config", str(path)]), 0)

        backend.apply_config.assert_called_once()

    def test_ctrl_c_during_gui_returns_interrupt_status(self):
        with mock.patch.object(pipewire_auto_router, "find_qt_gui_launcher", return_value=ROOT / "run-qt.sh"), mock.patch.object(
            pipewire_auto_router.subprocess, "call", side_effect=KeyboardInterrupt
        ):
            self.assertEqual(pipewire_auto_router.main([]), 130)

    def test_default_gui_launches_qt(self):
        binary = ROOT / "qt-build" / "pipewire-auto-router-qt"
        with mock.patch.object(pipewire_auto_router, "find_qt_gui_launcher", return_value=binary), mock.patch.object(
            pipewire_auto_router.subprocess, "call", return_value=0
        ) as call:
            self.assertEqual(pipewire_auto_router.main([]), 0)
        call.assert_called_once_with([str(binary)])


if __name__ == "__main__":
    unittest.main()
