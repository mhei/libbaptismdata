#!/bin/sh
command -v dbus-daemon >/dev/null 2>&1 || exit 77
exec "$PYTHON" "$(dirname "$0")/zeroconf.py" "$BDD_TEST_SERVER"
