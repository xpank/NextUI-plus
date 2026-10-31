#!/usr/bin/env bash
# Run the shared preview contracts against the Flip binary/runtime.
set -euo pipefail
root=${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}
VIDEO_PREVIEW_PLATFORM=my355 exec bash "$root/workspace/h700/check-video-preview.sh" "$root"
