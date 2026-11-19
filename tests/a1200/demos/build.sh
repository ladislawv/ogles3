#!/bin/bash
# Fills the ogles2-demos package directory ($1): the release's demos archive
# (release/dist/ogles3-<release>-demos.zip, made by hand: docs/GLES3_PLAN.md)
# unpacked as a user unpacks it, its libs/ogles3.library included, so that
# Run-M1 checks the archive itself. Called by ../build_package.sh.
set -eu
PKG=$1
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../../.." && pwd)
REL=$(sed -n 's/^#define OGLES3_RELEASE *"\([^"]*\)".*/\1/p' "$REPO/ogles3/ogles3_internal.h")
ZIP=$REPO/release/dist/ogles3-$REL-demos.zip
[ -f "$ZIP" ] || { echo "no $ZIP"; exit 1; }
TMP=$(mktemp -d)
python3 -c "import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "$ZIP" "$TMP"
cp -r "$TMP"/ogles3-demos/. "$PKG/"
rm -rf "$TMP"
