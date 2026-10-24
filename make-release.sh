#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
version="$(tr -d '[:space:]' < "$script_dir/VERSION")"
release_name="pipewire-auto-router-$version"
build_dir="$script_dir/qt-release-build"
dist_dir="$script_dir/dist"
release_tmp="$(mktemp -d)"
trap 'rm -rf -- "$release_tmp"' EXIT

if [[ ! "$version" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    printf 'Invalid release version in VERSION: %s\n' "$version" >&2
    exit 1
fi

cmake -S "$script_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build "$build_dir" --parallel
ctest --test-dir "$build_dir" --output-on-failure --no-tests=error

binary_stage="$release_tmp/$release_name-linux-x86_64"
source_stage="$release_tmp/$release_name-source"
mkdir -p "$binary_stage" "$source_stage/src" "$source_stage/tests" "$dist_dir"

install -m 0755 "$build_dir/pipewire-auto-router-qt" "$binary_stage/pipewire-auto-router-qt"
for file in pipewire_auto_router.py install-kde-app.sh install-user-service.sh uninstall.sh; do
    install -m 0755 "$script_dir/$file" "$binary_stage/$file"
done
for file in VERSION README.md CHANGELOG.md pipewire-auto-router.desktop.in; do
    install -m 0644 "$script_dir/$file" "$binary_stage/$file"
done

for file in CMakeLists.txt VERSION README.md CHANGELOG.md pipewire-auto-router.desktop.in; do
    install -m 0644 "$script_dir/$file" "$source_stage/$file"
done
for file in pipewire_auto_router.py run-qt.sh install-kde-app.sh install-user-service.sh uninstall.sh make-release.sh; do
    install -m 0755 "$script_dir/$file" "$source_stage/$file"
done
install -m 0644 "$script_dir/src/qt_main.cpp" "$source_stage/src/qt_main.cpp"
install -m 0644 "$script_dir/tests/test_backend.py" "$source_stage/tests/test_backend.py"
install -m 0644 "$script_dir/tests/test_installers.py" "$source_stage/tests/test_installers.py"
install -m 0644 "$script_dir/tests/test_qt.cpp" "$source_stage/tests/test_qt.cpp"

binary_archive="$dist_dir/$release_name-linux-x86_64.tar.gz"
source_archive="$dist_dir/$release_name-source.tar.gz"
tar -C "$release_tmp" -czf "$binary_archive" "$(basename "$binary_stage")"
tar -C "$release_tmp" -czf "$source_archive" "$(basename "$source_stage")"
(
    cd "$dist_dir"
    sha256sum "$(basename "$binary_archive")" "$(basename "$source_archive")" > SHA256SUMS
)

printf 'Release %s created:\n' "$version"
printf '  %s\n' "$binary_archive"
printf '  %s\n' "$source_archive"
printf '  %s\n' "$dist_dir/SHA256SUMS"

