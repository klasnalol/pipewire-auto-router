import sys
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
    PipeWireBackend,
    choose_pairs,
    is_valid_device_name,
    is_supported_auto_connection,
    parse_links,
    parse_port_lines,
    port_channel,
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


class MainEntrypointTests(unittest.TestCase):
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
