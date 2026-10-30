#!/usr/bin/env python3
"""Verify the local patched ARM64 SDK and the runtime staged next to the game."""

import argparse
import hashlib
import json
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent
LIBRARIES = ('librexruntime.dylib', 'librexgpu-xenos.dylib')


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def patch_identity():
    paths = [ROOT / 'patches/rexglue-sdk/baseline.env']
    paths += [p for p in sorted((ROOT / 'patches/rexglue-sdk').glob('*.patch'))
              if not p.name.endswith('.windows.patch')]
    paths += sorted((ROOT / 'patches/rexglue-sdk-macos').glob('*.patch'))
    return {str(p.relative_to(ROOT)): digest(p) for p in paths}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', required=True, type=pathlib.Path)
    parser.add_argument('--staged', type=pathlib.Path)
    parser.add_argument('--record-source', type=pathlib.Path,
                        help='record installed files after comparing them with this SDK source build')
    args = parser.parse_args()
    sdk = args.sdk.resolve()
    stamp = sdk / '.crackdown-macos-runtime.json'
    if args.record_source:
        source = args.record_source.resolve()
        commit = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], text=True).strip()
        baseline = dict(line.split('=', 1) for line in (ROOT / 'patches/rexglue-sdk/baseline.env').read_text().splitlines()
                        if line and not line.startswith('#'))
        if commit != baseline['SDK_COMMIT']:
            raise ValueError('SDK source does not match the pinned baseline commit')
        artifacts = {}
        for name in LIBRARIES:
            installed = sdk / 'lib' / name
            if digest(installed) != digest(source / 'out/mac-arm64' / name):
                raise ValueError(f'{name} does not match the source build')
            artifacts[f'lib/{name}'] = digest(installed)
        for header in (source / 'include/rex').rglob('*'):
            if header.is_file() and not header.name.endswith('.in'):
                relative = header.relative_to(source)
                if digest(sdk / relative) != digest(header):
                    raise ValueError(f'{relative} does not match the source build')
                artifacts[str(relative)] = digest(header)
        stamp.write_text(json.dumps({'commit': commit, 'patches': patch_identity(), 'artifacts': artifacts}, indent=2) + '\n')
    record = json.loads(stamp.read_text())
    if record['patches'] != patch_identity():
        raise ValueError('SDK patches changed: rebuild and stage the macOS SDK')
    for relative, expected in record['artifacts'].items():
        if digest(sdk / relative) != expected:
            raise ValueError(f'{relative} changed: rebuild and stage the macOS SDK')
    if args.staged:
        for name in LIBRARIES:
            if digest(args.staged / name) != record['artifacts'][f'lib/{name}']:
                raise ValueError(f'staged {name} is stale')
    print('macOS patched SDK verified' + ('; staged runtime verified' if args.staged else ''))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        raise SystemExit(f'macOS SDK verification failed: {error}')
