#!/bin/sh
# No hardware side effects: the previous fixed-delay kill/start implementation
# was disproven as a complete UI reload by the .139 builtin/init-state trace.
# Keep the old entry point explicit rather than silently running a broken flow.
printf '%s\n' 'miwear restart is not enabled: builtin globals are not reset by the verified launch path.' 'A verified teardown and pre-resource startup barrier are required; no task was stopped.' >&2
exit 78
