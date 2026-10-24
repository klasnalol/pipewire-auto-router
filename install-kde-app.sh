#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$script_dir/qt-build"
data_home="${XDG_DATA_HOME:-"$HOME/.local/share"}"
app_dir="$data_home/pipewire-auto-router"
applications_dir="$data_home/applications"
desktop_path="$applications_dir/pipewire-auto-router.desktop"

if [[ -x "$script_dir/pipewire-auto-router-qt" ]]; then
    binary_path="$script_dir/pipewire-auto-router-qt"
elif [[ -f "$script_dir/CMakeLists.txt" ]]; then
    cmake -S "$script_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release
    cmake --build "$build_dir"
    binary_path="$build_dir/pipewire-auto-router-qt"
else
    printf 'No prebuilt application or CMake source tree was found.\n' >&2
    exit 1
fi

mkdir -p "$app_dir" "$applications_dir"
install -m 0755 "$binary_path" "$app_dir/pipewire-auto-router-qt"
install -m 0755 "$script_dir/pipewire_auto_router.py" "$app_dir/pipewire_auto_router.py"
sed "s|@EXECUTABLE@|$app_dir/pipewire-auto-router-qt|g" \
    "$script_dir/pipewire-auto-router.desktop.in" > "$desktop_path"
chmod 0644 "$desktop_path"

if command -v update-desktop-database >/dev/null 2>&1; then
    update-desktop-database "$applications_dir" || true
fi
if command -v kbuildsycoca6 >/dev/null 2>&1; then
    kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
elif command -v kbuildsycoca5 >/dev/null 2>&1; then
    kbuildsycoca5 --noincremental >/dev/null 2>&1 || true
fi

if command -v systemctl >/dev/null 2>&1 && systemctl --user is-active --quiet pipewire-auto-router.service; then
    systemctl --user restart pipewire-auto-router.service
fi

printf 'Installed PipeWire Auto Router in the KDE application menu.\n'
printf 'Desktop entry: %s\n' "$desktop_path"
