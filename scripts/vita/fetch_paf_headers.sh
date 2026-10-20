#!/bin/sh
# Fetches the PAF headers that VitaPlex's quick menu plugin (src/qm_plugin_psv)
# is built against, into <out_dir>/include. CMakeLists.txt builds the plugin
# when -DVITAPLEX_PAF_INCLUDE_DIR=<out_dir>/include is given, and leaves it out
# otherwise.
#
# PAF is the Vita's own UI library, which the quick menu is made of. vitasdk
# declares only a little of it; GrapheneCt's ScePaf-RE has the rest, restored
# from debug data, and Princess-of-Sleeping's vitasdk-paf-component patches it
# to build with vitasdk (MIT). ScePaf-RE has no licence file, so it is fetched
# here rather than kept in this repository. Both are pinned to the commits
# below. The plugin's import stubs (paf_stubs.S) come from the same commits.
#
# usage: scripts/vita/fetch_paf_headers.sh <out_dir>
# needs: git, patch

set -eu

OUT=${1:?usage: fetch_paf_headers.sh <out_dir>}
SCEPAF_RE_URL=https://github.com/GrapheneCt/ScePaf-RE
SCEPAF_RE_COMMIT=b76d6ae00516b107d37e7e8a1a16211e3b729a07
PAF_COMPONENT_URL=https://github.com/Princess-of-Sleeping/vitasdk-paf-component
PAF_COMPONENT_COMMIT=9a68b07aa2d7247a756765bf154f292cda20aed3

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

git clone --quiet "$SCEPAF_RE_URL" "$WORK/ScePaf-RE"
git -C "$WORK/ScePaf-RE" checkout --quiet "$SCEPAF_RE_COMMIT"
git clone --quiet "$PAF_COMPONENT_URL" "$WORK/paf-component"
git -C "$WORK/paf-component" checkout --quiet "$PAF_COMPONENT_COMMIT"

# As vitasdk-paf-component's install.sh does, without installing into the SDK:
# its install replaces the SDK's own psp2/paf.h and ScePaf stubs, which the
# rest of VitaPlex is built with.
(cd "$WORK/ScePaf-RE" && patch -p2 -s < "$WORK/paf-component/vitasdk_2.patch")
rm -rf "$OUT"
mkdir -p "$OUT"
cp -r "$WORK/ScePaf-RE/include" "$OUT/include"
cp "$WORK/paf-component/include/map" "$OUT/include/paf/std/map"
cp "$WORK/paf-component/include/math.h" "$OUT/include/paf/math/math.h"

echo "PAF headers in $OUT/include (ScePaf-RE ${SCEPAF_RE_COMMIT}, vitasdk-paf-component ${PAF_COMPONENT_COMMIT})"
