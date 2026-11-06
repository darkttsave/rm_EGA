#!/usr/bin/env bash
# B3-B reuses the isolated, local-only B2 MQTT acceptance scenarios.
set -euo pipefail
script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
exec "$script_dir/rm27_b2_acceptance.sh" "$@"
