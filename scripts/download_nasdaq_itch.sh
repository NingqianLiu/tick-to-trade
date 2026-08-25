#!/usr/bin/env bash
# Downloads the historical ITCH files from Nasdaq's public directory into data/.
#
# The first argument can name a different directory; with no argument the files land in
# <project root>/data. wget keeps the directory structure of the server, resumes a file
# that was cut short, and skips anything whose timestamp has not moved, so running this
# again after a failure costs nothing.

# Stop on the first failure, refuse unset variables, and let a failure anywhere in a
# pipeline set the exit status, so a partial download never looks like a success.
set -Eeuo pipefail

# Nasdaq keeps the files here, organised by version and date. The %20 is a space.
readonly BASE_URL='https://emi.nasdaq.com/ITCH/Nasdaq%20ITCH/'
# Where this script itself lives, resolved to an absolute path so the default download
# directory does not depend on where the caller happened to be standing.
readonly SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_DIR="$(dirname -- "$SCRIPT_DIR")"
readonly DEST_DIR="${1:-$PROJECT_DIR/data}"

# Check for wget before creating anything or touching the network.
if ! command -v wget >/dev/null 2>&1; then
  echo 'wget was not found; please install it first.' >&2
  exit 1
fi

mkdir -p -- "$DEST_DIR"

# The download itself. Every option shapes either the local directory or what happens on
# a second run; only the last argument is the address to start from.
#   --recursive             follow the directory listing instead of saving one index page
#   --level=inf             no depth limit, so every year and version is reached
#   --no-parent             never climb out of the ITCH directory into the rest of the site
#   --no-host-directories   no emi.nasdaq.com level under the destination
#   --cut-dirs=2            drop the first two URL directories, so the local tree starts
#                           at the data itself
#   --directory-prefix      keep everything under the directory the caller chose
#   --reject='index.html*'  throw away the listing pages the walk generates
#   --continue              resume a half written file rather than fetching it again
#   --timestamping          skip files the server has not changed since last time
wget \
  --recursive \
  --level=inf \
  --no-parent \
  --no-host-directories \
  --cut-dirs=2 \
  --directory-prefix="$DEST_DIR" \
  --reject='index.html*' \
  --continue \
  --timestamping \
  "$BASE_URL"
