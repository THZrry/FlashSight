#!/usr/bin/env bash
# FlashSight 3rdparty fetch (wrapper around fetch_3rdparty.py).
#
#   bash scripts/fetch_3rdparty.sh
#   PROXY=http://127.0.0.1:10809 bash scripts/fetch_3rdparty.sh
#   SKIP_WEBVIEW=1 bash scripts/fetch_3rdparty.sh
#
# The real implementation is Python: it is the one download/extract stack that
# works unchanged on Windows, macOS and Linux (curl + unzip + tar is not a safe
# assumption on a fresh Windows runner).
set -e
cd "$(dirname "$0")/.."
exec python3 scripts/fetch_3rdparty.py "$@"
