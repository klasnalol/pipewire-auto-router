#!/usr/bin/env python3
"""Rule-based PipeWire reconnect helper used by the Qt patchbay and service."""

from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import time
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable


APP_NAME = "pipewire-auto-router"
DEFAULT_CONFIG_DIR = Path(os.environ.get("XDG_CONFIG_HOME", Path.home() / ".config")) / APP_NAME
DEFAULT_CONFIG_PATH = DEFAULT_CONFIG_DIR / "rules.json"

SOURCE_KINDS = ("app-output", "monitor-output", "any-output")
TARGET_KINDS = ("sink-playback", "app-input", "any-input")
MEDIA_KINDS = ("audio", "any")


class PipeWireCommandError(RuntimeError):
    """Raised when a PipeWire command fails."""


@dataclass
class Rule:
    name: str
    source: str
    target: str
    enabled: bool = True
    regex: bool = False
    disconnect_others: bool = True
    linger: bool = True
    source_kind: str = "app-output"
    target_kind: str = "sink-playback"
    media: str = "audio"

    @classmethod
    def from_dict(cls, data: dict) -> "Rule":
        return cls(
            name=str(data.get("name") or "Unnamed rule"),
            source=str(data.get("source") or ""),
            target=str(data.get("target") or ""),
            enabled=bool(data.get("enabled", True)),
            regex=bool(data.get("regex", False)),
            disconnect_others=bool(data.get("disconnect_others", True)),
            linger=bool(data.get("linger", True)),
            source_kind=normal_choice(data.get("source_kind"), SOURCE_KINDS, "app-output"),
            target_kind=normal_choice(data.get("target_kind"), TARGET_KINDS, "sink-playback"),
            media=normal_choice(data.get("media"), MEDIA_KINDS, "audio"),
        )

    def clean(self) -> "Rule":
        self.name = self.name.strip() or "Unnamed rule"
        self.source = self.source.strip()
        self.target = self.target.strip()
        self.source_kind = normal_choice(self.source_kind, SOURCE_KINDS, "app-output")
        self.target_kind = normal_choice(self.target_kind, TARGET_KINDS, "sink-playback")
        self.media = normal_choice(self.media, MEDIA_KINDS, "audio")
        return self


@dataclass
class ConnectionRule:
    name: str
    output: str
    input: str
    enabled: bool = True
    disconnect_others: bool = False
    linger: bool = True

    @classmethod
    def from_dict(cls, data: dict) -> "ConnectionRule":
        return cls(
            name=str(data.get("name") or "Unnamed connection"),
            output=str(data.get("output") or ""),
            input=str(data.get("input") or ""),
            enabled=bool(data.get("enabled", True)),
            disconnect_others=bool(data.get("disconnect_others", False)),
            linger=bool(data.get("linger", True)),
        )

    def clean(self) -> "ConnectionRule":
        self.name = self.name.strip() or f"{self.output} -> {self.input}"
        self.output = self.output.strip()
        self.input = self.input.strip()
        return self


@dataclass
class FakeDevice:
    name: str
    description: str
    channels: int = 2
    enabled: bool = True

    @classmethod
    def from_dict(cls, data: dict) -> "FakeDevice":
        return cls(
            name=str(data.get("name") or ""),
            description=str(data.get("description") or data.get("name") or ""),
            channels=int(data.get("channels", 2)),
            enabled=bool(data.get("enabled", True)),
        )

    def clean(self) -> "FakeDevice":
        self.name = self.name.strip()
        self.description = self.description.strip() or self.name
        self.channels = max(1, min(int(self.channels), 8))
        return self


@dataclass
class Config:
    interval_seconds: float
    rules: list[Rule]
    connections: list[ConnectionRule]
    devices: list[FakeDevice]

    @classmethod
    def load(cls, path: Path = DEFAULT_CONFIG_PATH) -> "Config":
        if not path.exists():
            return cls(interval_seconds=2.0, rules=[], connections=[], devices=[])
        with path.open("r", encoding="utf-8") as handle:
            raw = json.load(handle)
        if isinstance(raw, list):
            rules = [Rule.from_dict(item) for item in raw if isinstance(item, dict)]
            return cls(interval_seconds=2.0, rules=rules, connections=[], devices=[])
        if not isinstance(raw, dict):
            raise ValueError(f"{path} must contain a JSON object or list")
        interval = float(raw.get("interval_seconds", 2.0))
        rules = [Rule.from_dict(item) for item in raw.get("rules", []) if isinstance(item, dict)]
        connections = [ConnectionRule.from_dict(item) for item in raw.get("connections", []) if isinstance(item, dict)]
        devices = [FakeDevice.from_dict(item) for item in raw.get("devices", []) if isinstance(item, dict)]
        return cls(interval_seconds=max(interval, 0.5), rules=rules, connections=connections, devices=devices)

    def save(self, path: Path = DEFAULT_CONFIG_PATH) -> None:
        path.parent.mkdir(parents=True, exist_ok=True)
        data = {
            "interval_seconds": self.interval_seconds,
            "connections": [asdict(connection.clean()) for connection in self.connections],
            "devices": [asdict(device.clean()) for device in self.devices],
            "rules": [asdict(rule.clean()) for rule in self.rules],
        }
        tmp = path.with_suffix(path.suffix + ".tmp")
        with tmp.open("w", encoding="utf-8") as handle:
            json.dump(data, handle, indent=2, sort_keys=True)
            handle.write("\n")
        tmp.replace(path)


@dataclass
class GraphSnapshot:
    outputs: list[str]
    inputs: list[str]
    links: set[tuple[str, str]]
    node_text: dict[str, str]


def normal_choice(value: object, choices: Iterable[str], default: str) -> str:
    text = str(value or "")
    return text if text in choices else default


def run_command(args: list[str]) -> str:
    completed = subprocess.run(args, check=False, text=True, capture_output=True)
    if completed.returncode != 0:
        detail = completed.stderr.strip() or completed.stdout.strip() or f"exit {completed.returncode}"
        raise PipeWireCommandError(f"{' '.join(args)} failed: {detail}")
    return completed.stdout


class PipeWireGraphMonitor:
    """Wait for graph changes that can make a saved connection actionable."""

    def __init__(self) -> None:
        self.process = None
        self.object_types: dict[object, str] = {}

    def __enter__(self) -> "PipeWireGraphMonitor":
        self.process = subprocess.Popen(
            ["pw-dump", "--monitor", "--raw", "--no-colors"],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            bufsize=1,
        )
        try:
            initial_objects = self._read_objects()
            if initial_objects is None:
                raise PipeWireCommandError("pw-dump --monitor exited before its initial snapshot")
            self._update_object_types(initial_objects, detect_changes=False)
        except BaseException:
            self.close()
            raise
        return self

    def __exit__(self, _exc_type, _exc_value, _traceback) -> None:
        self.close()

    def close(self) -> None:
        if self.process is None:
            return
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        if self.process.stdout is not None:
            self.process.stdout.close()
        self.process = None

    def wait_for_change(self) -> bool:
        """Block until a relevant graph change occurs or the monitor exits."""
        while True:
            objects = self._read_objects()
            if objects is None:
                return False
            if self._update_object_types(objects, detect_changes=True):
                return True

    def _read_objects(self) -> list[dict] | None:
        if self.process is None or self.process.stdout is None:
            return None
        while True:
            line = self.process.stdout.readline()
            if line == "":
                return None
            try:
                objects = json.loads(line)
            except json.JSONDecodeError:
                continue
            if isinstance(objects, list):
                return [item for item in objects if isinstance(item, dict)]

    def _update_object_types(self, objects: list[dict], detect_changes: bool) -> bool:
        changed = False
        for item in objects:
            object_id = item.get("id")
            if object_id is None:
                continue
            type_name = item.get("type")
            if isinstance(type_name, str):
                previous_type = self.object_types.get(object_id)
                self.object_types[object_id] = type_name
                if detect_changes and previous_type is None and type_name.endswith(":Port"):
                    changed = True
                continue
            if item.get("info", object()) is None:
                previous_type = self.object_types.pop(object_id, "")
                if detect_changes and previous_type.endswith((":Port", ":Link")):
                    changed = True
        return changed


def strip_port_id(line: str) -> str:
    return re.sub(r"^\s*\d+\s+", "", line.strip())


def parse_port_lines(text: str) -> list[str]:
    return [strip_port_id(line) for line in text.splitlines() if line.strip()]


def parse_links(text: str) -> set[tuple[str, str]]:
    links: set[tuple[str, str]] = set()
    current: str | None = None
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped:
            continue
        no_id = strip_port_id(stripped)
        if no_id.startswith("|->"):
            if current:
                target = strip_port_id(no_id[3:].strip())
                links.add((current, target))
            continue
        if no_id.startswith("|<-"):
            continue
        current = no_id
    return links


def port_node(port: str) -> str:
    return port.rsplit(":", 1)[0] if ":" in port else port


def port_leaf(port: str) -> str:
    return port.rsplit(":", 1)[-1]


def port_channel(port: str) -> str | None:
    leaf = port_leaf(port)
    for channel in ("FL", "FR", "FC", "LFE", "SL", "SR", "RL", "RR", "FLC", "FRC", "RC"):
        if re.search(rf"(?:^|[_-]){re.escape(channel)}$", leaf):
            return channel
    number = re.search(r"(?:^|[_-])(\d+)$", leaf)
    if number:
        return number.group(1)
    return None


def source_kind_matches(port: str, kind: str) -> bool:
    leaf = port_leaf(port).lower()
    if kind == "any-output":
        return True
    if kind == "monitor-output":
        return leaf.startswith("monitor")
    return leaf.startswith("output")


def target_kind_matches(port: str, kind: str) -> bool:
    leaf = port_leaf(port).lower()
    if kind == "any-input":
        return True
    if kind == "app-input":
        return leaf.startswith("input") or leaf.startswith("capture")
    return leaf.startswith("playback")


def is_supported_auto_connection(output: str, input_: str) -> bool:
    return (
        source_kind_matches(output, "app-output")
        or source_kind_matches(output, "monitor-output")
    ) and target_kind_matches(input_, "sink-playback")


def is_valid_device_name(name: str) -> bool:
    return re.fullmatch(r"[A-Za-z0-9_.-]+", name) is not None


def pattern_matches(pattern: str, candidate: str, regex: bool) -> bool:
    if not pattern:
        return False
    if regex:
        return re.search(pattern, candidate, re.IGNORECASE) is not None
    return pattern.lower() in candidate.lower()


class PipeWireBackend:
    def list_sinks(self) -> set[str]:
        try:
            text = run_command(["pactl", "list", "short", "sinks"])
        except PipeWireCommandError:
            return set()
        sinks: set[str] = set()
        for line in text.splitlines():
            pieces = line.split()
            if len(pieces) >= 2:
                sinks.add(pieces[1])
        return sinks

    def ensure_devices(self, devices: list[FakeDevice], dry_run: bool = False) -> list[str]:
        messages: list[str] = []
        sinks = self.list_sinks()
        for device in devices:
            device.clean()
            if not device.enabled:
                continue
            if not device.name:
                messages.append("Fake device skipped: name is required")
                continue
            if not is_valid_device_name(device.name):
                messages.append(f"{device.name}: invalid fake device name")
                continue
            if device.name in sinks:
                continue
            action = f"create fake audio device {device.name}"
            if dry_run:
                messages.append(f"{device.name}: would {action}")
                continue
            try:
                args = [
                    "pactl",
                    "load-module",
                    "module-null-sink",
                    f"sink_name={device.name}",
                    f"sink_properties=device.description={device.description}",
                    f"channels={device.channels}",
                ]
                if device.channels == 2:
                    args.append("channel_map=front-left,front-right")
                run_command(args)
                sinks.add(device.name)
                messages.append(f"{device.name}: {action}")
            except PipeWireCommandError as exc:
                messages.append(f"{device.name}: {exc}")
        return messages

    def snapshot(self) -> GraphSnapshot:
        outputs = parse_port_lines(run_command(["pw-link", "-o"]))
        inputs = parse_port_lines(run_command(["pw-link", "-i"]))
        links = parse_links(run_command(["pw-link", "-l"]))
        return GraphSnapshot(
            outputs=outputs,
            inputs=inputs,
            links=links,
            node_text=self._node_text_from_dump(),
        )

    def connect(self, output: str, input_: str, linger: bool) -> None:
        args = ["pw-link"]
        if linger:
            args.append("-L")
        args.extend(["-w", output, input_])
        run_command(args)

    def disconnect(self, output: str, input_: str) -> None:
        run_command(["pw-link", "-d", output, input_])

    def apply_config(self, config: Config, dry_run: bool = False) -> list[str]:
        messages = self.ensure_devices(config.devices, dry_run=dry_run)
        graph = self.snapshot()
        for connection in config.connections:
            if connection.enabled:
                messages.extend(self.apply_connection(connection.clean(), graph, dry_run=dry_run))
        return messages

    def apply_connection(self, connection: ConnectionRule, graph: GraphSnapshot, dry_run: bool = False) -> list[str]:
        messages: list[str] = []
        if not connection.output or not connection.input:
            return [f"{connection.name}: output and input are required"]
        if connection.output not in graph.outputs:
            return [f"{connection.name}: output port is not present: {connection.output}"]
        if connection.input not in graph.inputs:
            return [f"{connection.name}: input port is not present: {connection.input}"]
        if not is_supported_auto_connection(connection.output, connection.input):
            return [f"{connection.name}: skipped unsupported auto-reconnect connection"]

        if (connection.output, connection.input) in graph.links:
            return messages

        action = f"connect {connection.output} -> {connection.input}"
        if dry_run:
            messages.append(f"{connection.name}: would {action}")
            return messages

        try:
            self.connect(connection.output, connection.input, linger=connection.linger)
            graph.links.add((connection.output, connection.input))
            messages.append(f"{connection.name}: {action}")
        except PipeWireCommandError as exc:
            messages.append(f"{connection.name}: {exc}")
        return messages

    def apply_rules(self, rules: list[Rule], dry_run: bool = False) -> list[str]:
        graph = self.snapshot()
        messages: list[str] = []
        for rule in rules:
            if not rule.enabled:
                continue
            messages.extend(self.apply_rule(rule.clean(), graph, dry_run=dry_run))
        return messages

    def apply_rule(self, rule: Rule, graph: GraphSnapshot, dry_run: bool = False) -> list[str]:
        messages: list[str] = []
        try:
            outputs = [
                port
                for port in graph.outputs
                if source_kind_matches(port, rule.source_kind)
                and self._media_matches(port, graph, rule.media)
                and pattern_matches(rule.source, self._match_text(port, graph), rule.regex)
            ]
            inputs = [
                port
                for port in graph.inputs
                if target_kind_matches(port, rule.target_kind)
                and self._media_matches(port, graph, rule.media)
                and pattern_matches(rule.target, self._match_text(port, graph), rule.regex)
            ]
        except re.error as exc:
            return [f"{rule.name}: invalid regex: {exc}"]

        if not outputs:
            return [f"{rule.name}: no source ports matched {rule.source!r}"]
        if not inputs:
            return [f"{rule.name}: no target ports matched {rule.target!r}"]

        pairs, missing = choose_pairs(outputs, inputs)
        for output in missing:
            messages.append(f"{rule.name}: no matching target channel for {output}")
        if not pairs:
            return messages

        desired_by_output = {output: input_ for output, input_ in pairs}
        if rule.disconnect_others:
            existing_by_output = links_by_output(graph.links)
            for output, desired_input in desired_by_output.items():
                for current_input in sorted(existing_by_output.get(output, ())):
                    if current_input == desired_input:
                        continue
                    action = f"disconnect {output} -> {current_input}"
                    if dry_run:
                        messages.append(f"{rule.name}: would {action}")
                    else:
                        try:
                            self.disconnect(output, current_input)
                            graph.links.discard((output, current_input))
                            messages.append(f"{rule.name}: {action}")
                        except PipeWireCommandError as exc:
                            messages.append(f"{rule.name}: {exc}")

        for output, input_ in pairs:
            if (output, input_) in graph.links:
                continue
            action = f"connect {output} -> {input_}"
            if dry_run:
                messages.append(f"{rule.name}: would {action}")
                continue
            try:
                self.connect(output, input_, linger=rule.linger)
                graph.links.add((output, input_))
                messages.append(f"{rule.name}: {action}")
            except PipeWireCommandError as exc:
                messages.append(f"{rule.name}: {exc}")
        return messages

    def _node_text_from_dump(self) -> dict[str, str]:
        try:
            objects = json.loads(run_command(["pw-dump"]))
        except (json.JSONDecodeError, PipeWireCommandError):
            return {}

        node_text: dict[str, str] = {}
        for item in objects:
            if not str(item.get("type", "")).endswith(":Node"):
                continue
            props = item.get("info", {}).get("props", {}) or {}
            pieces = [
                props.get("node.name"),
                props.get("node.description"),
                props.get("application.name"),
                props.get("application.process.binary"),
                props.get("media.name"),
                props.get("media.class"),
                props.get("media.role"),
            ]
            text = " ".join(str(piece) for piece in pieces if piece)
            for key in pieces[:4]:
                if key:
                    node_text[str(key)] = text
        return node_text

    def _match_text(self, port: str, graph: GraphSnapshot) -> str:
        node = port_node(port)
        return " ".join(part for part in (port, node, graph.node_text.get(node, "")) if part)

    def _media_matches(self, port: str, graph: GraphSnapshot, media: str) -> bool:
        if media == "any":
            return True
        text = self._match_text(port, graph).lower()
        if media == "audio" and "audio" in text:
            return True
        return port_channel(port) in {"FL", "FR", "FC", "LFE", "SL", "SR", "RL", "RR", "FLC", "FRC", "RC"}


def links_by_output(links: set[tuple[str, str]]) -> dict[str, set[str]]:
    grouped: dict[str, set[str]] = {}
    for output, input_ in links:
        grouped.setdefault(output, set()).add(input_)
    return grouped


def choose_pairs(outputs: list[str], inputs: list[str]) -> tuple[list[tuple[str, str]], list[str]]:
    inputs_by_channel: dict[str, list[str]] = {}
    for input_ in inputs:
        channel = port_channel(input_)
        if channel:
            inputs_by_channel.setdefault(channel, []).append(input_)

    pairs: list[tuple[str, str]] = []
    missing: list[str] = []
    for index, output in enumerate(outputs):
        channel = port_channel(output)
        target: str | None = None
        if channel:
            choices = inputs_by_channel.get(channel)
            if choices:
                target = choices[0]
        if target is None and len(inputs) == 1:
            target = inputs[0]
        if target is None and index < len(inputs) and not channel:
            target = inputs[index]
        if target is None:
            missing.append(output)
        else:
            pairs.append((output, target))
    return pairs, missing


def print_graph(backend: PipeWireBackend) -> None:
    graph = backend.snapshot()
    print("Outputs:")
    for port in sorted(graph.outputs, key=str.lower):
        print(f"  {port}")
    print("\nInputs:")
    for port in sorted(graph.inputs, key=str.lower):
        print(f"  {port}")
    print("\nLinks:")
    for output, input_ in sorted(graph.links):
        print(f"  {output} -> {input_}")


def find_qt_gui_launcher() -> Path | None:
    script_dir = Path(__file__).resolve().parent
    candidates = [
        script_dir / "qt-build" / "pipewire-auto-router-qt",
        script_dir / "build" / "pipewire-auto-router-qt",
        script_dir / "pipewire-auto-router-qt",
        script_dir / "run-qt.sh",
    ]
    for candidate in candidates:
        if candidate.exists() and os.access(candidate, os.X_OK):
            return candidate
    return None


def print_watch_messages(messages: list[str]) -> None:
    visible_messages = [
        message
        for message in messages
        if ": connect " in message or ": disconnect " in message or "failed:" in message
    ]
    for message in visible_messages:
        print(f"{time.strftime('%H:%M:%S')} {message}", flush=True)


def watch_config(backend: PipeWireBackend, config: Config, dry_run: bool = False) -> None:
    """Apply the config initially and whenever PipeWire reports a useful change."""
    retry_delay = max(config.interval_seconds, 0.5)
    while True:
        try:
            with PipeWireGraphMonitor() as monitor:
                print_watch_messages(backend.apply_config(config, dry_run=dry_run))
                while monitor.wait_for_change():
                    print_watch_messages(backend.apply_config(config, dry_run=dry_run))
        except (OSError, PipeWireCommandError) as exc:
            print(f"{time.strftime('%H:%M:%S')} PipeWire monitor failed: {exc}", file=sys.stderr, flush=True)
        time.sleep(retry_delay)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Reconnect exact PipeWire port connections saved by the Qt patchbay.")
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG_PATH, help=f"config file, default {DEFAULT_CONFIG_PATH}")
    parser.add_argument("--list", action="store_true", help="print current ports and links")
    parser.add_argument("--apply", action="store_true", help="apply saved auto-reconnect connections once and exit")
    parser.add_argument("--watch", action="store_true", help="apply saved auto-reconnect connections continuously without the GUI")
    parser.add_argument("--dry-run", action="store_true", help="show planned actions without changing links")
    args = parser.parse_args(argv)

    backend = PipeWireBackend()
    if args.list:
        print_graph(backend)
        return 0

    if args.apply or args.watch:
        config = Config.load(args.config)
        if not config.connections and not config.devices:
            print(f"No saved fake devices or auto-reconnect connections in {args.config}")
            return 1
        if args.watch:
            watch_config(backend, config, dry_run=args.dry_run)
            return 0
        messages = backend.apply_config(config, dry_run=args.dry_run)
        for message in messages or ["No changes needed"]:
            print(f"{time.strftime('%H:%M:%S')} {message}", flush=True)
        return 0

    qt_launcher = find_qt_gui_launcher()
    if not qt_launcher:
        print("Qt GUI is not built. Run ./run-qt.sh from this directory.", file=sys.stderr)
        return 1
    try:
        return subprocess.call([str(qt_launcher)])
    except KeyboardInterrupt:
        return 130
    except Exception as exc:
        print(f"Failed to start Qt GUI: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
