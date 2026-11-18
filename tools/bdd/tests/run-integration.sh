#!/bin/sh
exec "$PYTHON" "$(dirname "$0")/integration.py" "$BDD_TEST_SERVER" openssl
