#!/usr/bin/env bash
set -euo pipefail

data_home="${XDG_DATA_HOME:-"$HOME/.local/share"}"
config_home="${XDG_CONFIG_HOME:-"$HOME/.config"}"
app_dir="$data_home/pipewire-auto-router"
applications_dir="$data_home/applications"
service_path="$config_home/systemd/user/pipewire-auto-router.service"

if [[ -f "$service_path" ]]; then
    systemctl --user disable --now pipewire-auto-router.service >/dev/null 2>&1 || true
    rm -f -- "$service_path"
    systemctl --user daemon-reload || true
fi

rm -f -- "$applications_dir/pipewire-auto-router.desktop"
rm -f -- "$app_dir/pipewire-auto-router-qt" "$app_dir/pipewire_auto_router.py"
rmdir -- "$app_dir" 2>/dev/null || true

if [[ -d "$applications_dir" ]] && command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$applications_dir" || true
fi
if command -v kbuildsycoca6 >/dev/null 2>&1; then
    kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
elif command -v kbuildsycoca5 >/dev/null 2>&1; then
    kbuildsycoca5 --noincremental >/dev/null 2>&1 || true
fi

printf 'PipeWire Auto Router was uninstalled.\n'
printf 'Your saved routing configuration was kept at %s\n' "$config_home/pipewire-auto-router/rules.json"
