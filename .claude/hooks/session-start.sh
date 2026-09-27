#!/bin/bash
# Claude Code on the web only: provision the board-less build/QEMU environment
# (tools/cloud/README.md). Local sessions on the machine with the board are
# left alone. Idempotent: a cached container re-checks in seconds.
set -euo pipefail
if [ "${CLAUDE_CODE_REMOTE:-}" != "true" ]; then
	exit 0
fi
cd "$CLAUDE_PROJECT_DIR"
mkdir -p build/cloud
if ! sh tools/cloud/setup.sh >build/cloud/setup.log 2>&1; then
	echo "tools/cloud/setup.sh FAILED - see build/cloud/setup.log:" >&2
	tail -20 build/cloud/setup.log >&2
	exit 1
fi
tail -12 build/cloud/setup.log
if [ -n "${CLAUDE_ENV_FILE:-}" ]; then
	echo ". \"$CLAUDE_PROJECT_DIR/tools/cloud/env.sh\"" >>"$CLAUDE_ENV_FILE"
fi
