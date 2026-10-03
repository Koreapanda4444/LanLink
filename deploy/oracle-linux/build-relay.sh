#!/usr/bin/env bash
set -euo pipefail

source /etc/os-release
[[ $ID == ol && ${VERSION_ID%%.*} == 9 ]] || { printf 'Build the relay distribution on Oracle Linux 9\n' >&2; exit 1; }
repository=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)
build_directory=${1:-"$repository/build/relay"}
cmake_binary=${LANLINK_CMAKE_BIN:-cmake}
vcpkg_directory=${VCPKG_ROOT:?Set VCPKG_ROOT to the vcpkg checkout at the repository baseline.}
case "$(uname -m)" in
    x86_64) triplet=x64-linux ;;
    aarch64) triplet=arm64-linux ;;
    *) printf 'Unsupported relay architecture\n' >&2; exit 1 ;;
esac
expected_baseline=$(python3 - "$repository/vcpkg.json" <<'PY'
import json,sys
print(json.load(open(sys.argv[1]))['builtin-baseline'])
PY
)
if [[ $(git -C "$vcpkg_directory" rev-parse HEAD) != "$expected_baseline" ]]; then
    printf 'The vcpkg checkout must match builtin-baseline in vcpkg.json\n' >&2
    exit 1
fi
if [[ ! -x "$vcpkg_directory/vcpkg" ]]; then
    bash "$vcpkg_directory/bootstrap-vcpkg.sh" -disableMetrics
fi
"$cmake_binary" -S "$repository" -B "$build_directory" -G Ninja \
    -DCMAKE_BUILD_TYPE=Release -DVCPKG_TARGET_TRIPLET="$triplet" \
    -DCMAKE_TOOLCHAIN_FILE="$vcpkg_directory/scripts/buildsystems/vcpkg.cmake" \
    -DLANLINK_PACKAGE_RELAY=ON -DLANLINK_RELAY_PACKAGE_PLATFORM=oraclelinux9
"$cmake_binary" --build "$build_directory" --parallel "${LANLINK_BUILD_JOBS:-2}"
cmake_bin_directory=$(dirname -- "$(command -v "$cmake_binary")")
"$cmake_bin_directory/ctest" --test-dir "$build_directory" --output-on-failure
"$cmake_bin_directory/cpack" --config "$build_directory/CPackConfig.cmake" \
    -B "$build_directory/artifacts"
find "$build_directory/artifacts" -maxdepth 1 -type f -name '*.tar.gz' -exec sha256sum {} \;
