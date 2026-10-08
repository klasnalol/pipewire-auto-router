#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
build_dir="$script_dir/qt-build"

cmake -S "$script_dir" -B "$build_dir"
cmake --build "$build_dir"
exec "$build_dir/pipewire-auto-router-qt" "$@"
