#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
data_home="${XDG_DATA_HOME:-"$HOME/.local/share"}"
app_dir="$data_home/pipewire-auto-router"
service_dir="${XDG_CONFIG_HOME:-"$HOME/.config"}/systemd/user"
service_path="$service_dir/pipewire-auto-router.service"

mkdir -p "$app_dir" "$service_dir"
install -m 0755 "$script_dir/pipewire_auto_router.py" "$app_dir/pipewire_auto_router.py"

cat > "$service_path" <<SERVICE
[Unit]
Description=PipeWire Auto Router
After=pipewire.service wireplumber.service

[Service]
Type=simple
ExecStart=/usr/bin/python3 "$app_dir/pipewire_auto_router.py" --watch
Restart=on-failure
RestartSec=2

[Install]
WantedBy=default.target
SERVICE

systemctl --user daemon-reload
systemctl --user enable --now pipewire-auto-router.service
systemctl --user status --no-pager pipewire-auto-router.service
