#!/bin/sh
command -v node >/dev/null 2>&1 || exit 77
exec node "$(dirname "$0")/frontend.cjs"
