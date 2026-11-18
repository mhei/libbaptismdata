#!/usr/bin/env python3
"""Exercise production configuration precedence in isolated fixture directories."""
import json
from pathlib import Path
import shutil
import subprocess
import sys

binary, root = sys.argv[1], Path(sys.argv[2])
root.mkdir(parents=True, exist_ok=True)


def write(relative, body):
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text('[bdd]\n' + body)


def load():
    return json.loads(subprocess.check_output([binary]))


try:
    assert load() == {'address': '0.0.0.0', 'port': 8080, 'keys': [], 'password': False}
    write('vendor/bdd.conf', 'Port=10001\nWritableKey=""\nWritableKey=vendor, serial, vendor, ,\nWritableKey=model\n')
    assert load()['keys'] == ['vendor', 'serial', 'model']
    assert load()['port'] == 10001
    write('run/bdd.conf', 'Port=10002\n')
    assert load()['port'] == 10002
    assert load()['keys'] == []  # Main files replace rather than merge.
    write('etc/bdd.conf', 'Port=10003\nWritableKey=local\n')
    assert load()['port'] == 10003
    write('etc/bdd.conf.d/10-local.conf', 'Port=10004\n')
    write('vendor/bdd.conf.d/90-vendor.conf', 'Port=10005\nWritableKey=late\n')
    assert load()['port'] == 10004  # Local directory overrides vendor snippets.
    write('run/bdd.conf.d/90-vendor.conf', 'Port=10006\n')
    assert load()['port'] == 10004
    write('etc/bdd.conf.d/90-vendor.conf', 'Port=10007\nWritableKey=discarded\nWritableKey=\nWritableKey="serial#"\nPassword=""\n')
    assert load()['port'] == 10007
    assert load()['keys'] == ['serial#']
    assert load()['password'] is False
    masked = root / 'etc/bdd.conf.d/90-vendor.conf'
    masked.unlink()
    masked.symlink_to('/dev/null')
    assert load()['port'] == 10004
    assert load()['keys'] == ['local']
    write('etc/bdd.conf.d/99-invalid.conf', 'Port=wrong\n')
    assert subprocess.run([binary], capture_output=True).returncode != 0
    print('Configuration precedence, list reset, masking and validation passed')
finally:
    shutil.rmtree(root)
