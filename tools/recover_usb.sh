#!/usr/bin/env bash
# Run explicitly on an idle rig. Use sudo for recovery, or --check for read-only status.
set -euo pipefail
script_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec /usr/bin/python3 "${script_dir}/mousehouse_usb_ready.py" "$@"
