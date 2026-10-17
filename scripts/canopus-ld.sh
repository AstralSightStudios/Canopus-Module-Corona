#!/bin/sh
# Keep CLI output within the custom loader's fixed section budget.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
SDK=${CANOPUS_ROOT:-"$ROOT/../Canopus-Private"}
exec "${LD_LLD:-ld.lld}" -T "$SDK/scripts/canopus_supervisor_sections.ld" "$@"
