#!/bin/sh
# Fetches the newest PS4 release of Shell Companion (tag starting "Ps4"): the
# module (sc_companion.prx) and the client library at the same tag, into
# third_party/shell_companion/. CMake builds it in when the files are there.
# Plain sh + curl (no python: the pacbrew image may not have it). Set TAG to
# pin a release; when the release lookup fails (rate limit), FALLBACK_TAG.
set -eu
REPO=Breezyslasher/playstation-shell-companion
DEST=${1:-third_party/shell_companion}
FALLBACK_TAG=Ps4-Alpha-.0.0.1
TAG=${TAG:-}

if [ -z "$TAG" ]; then
  # Releases come newest first; the first PS4 one that is not a draft.
  # CI passes GITHUB_TOKEN so the lookup is not rate-limited.
  AUTH=""
  [ -n "${GITHUB_TOKEN:-}" ] && AUTH="Authorization: Bearer $GITHUB_TOKEN"
  TAG=$(curl -fsSL ${AUTH:+-H "$AUTH"} "https://api.github.com/repos/$REPO/releases?per_page=50" 2>/dev/null \
    | tr -d '\n' | sed 's/},[[:space:]]*{"url"/}\n{"url"/g' \
    | grep -v '"draft":[[:space:]]*true' \
    | sed -n 's/.*"tag_name":[[:space:]]*"\([^"]*\)".*/\1/p' \
    | grep -i '^ps4' | head -n 1 || true)
  [ -n "$TAG" ] || { echo "PS4 release lookup failed; using $FALLBACK_TAG" >&2; TAG=$FALLBACK_TAG; }
fi
echo "Shell Companion PS4: $TAG"

mkdir -p "$DEST/ps4/client" "$DEST/ps4/bgm"
curl -fsSL -o "$DEST/sc_companion.prx" \
  "https://github.com/$REPO/releases/download/$TAG/sc_companion.prx"
RAW=https://raw.githubusercontent.com/$REPO/$TAG
for f in ps4/client/shell_companion.c ps4/client/shell_companion.h ps4/client/LICENSE ps4/bgm/bgm.h; do
  curl -fsSL -o "$DEST/$f" "$RAW/$f"
done
echo "$TAG" > "$DEST/VERSION"
