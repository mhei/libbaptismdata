#!/bin/sh
"$PYTHON" -c 'import yaml, openapi_spec_validator' >/dev/null 2>&1 || exit 77
exec "$PYTHON" "$(dirname "$0")/contract.py" "$(dirname "$0")/../openapi.yaml"
