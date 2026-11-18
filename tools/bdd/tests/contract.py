#!/usr/bin/env python3
"""Validate the standalone OpenAPI contract (optional development dependencies)."""
import sys
from pathlib import Path

import yaml
from openapi_spec_validator import validate


if __name__ == "__main__":
    path = Path(sys.argv[1])
    validate(yaml.safe_load(path.read_text()))
    print(f"Validated {path.name}")
