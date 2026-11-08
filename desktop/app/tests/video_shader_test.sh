#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# Check actual built packs using Qt's deserializer, without a GPU or GUI app.
set -euo pipefail
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
stage="$(mktemp -d -t nectarlink-shader-test-XXXXXX)"
trap 'rm -rf "${stage}"' EXIT
qmake_bin="${QMAKE:-qmake}"
qt_headers="$("${qmake_bin}" -query QT_INSTALL_HEADERS)"
qt_libraries="$("${qmake_bin}" -query QT_INSTALL_LIBS)"
qt_version="$("${qmake_bin}" -query QT_VERSION)"
"${CXX:-c++}" -std=c++17 -fPIC -Wall -Wextra -Werror \
    -I"${qt_headers}" -I"${qt_headers}/QtCore" -I"${qt_headers}/QtGui" \
    -I"${qt_headers}/QtGui/${qt_version}/QtGui" \
    "${root}/tests/video_shader_test.cpp" -L"${qt_libraries}" \
    -Wl,-rpath,"${qt_libraries}" -lQt6Gui -lQt6Core -o "${stage}/test"
"${stage}/test" "$@"
