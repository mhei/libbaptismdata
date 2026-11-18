#!/bin/sh
exec "$PYTHON" "$(dirname "$0")/configuration.py" "$BDD_CONFIG_TEST" "$BDD_CONFIG_ROOT"
