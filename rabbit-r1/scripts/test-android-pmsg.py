#!/usr/bin/env python3
"""Test the pinned liblog pmsg fork commit without an Android checkout or device I/O."""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import re
import resource
import shutil
import signal
import subprocess
import tempfile

ROOT = Path('/rabbitr1')
R1 = Path(__file__).resolve().parents[1]
INPUT_PREFIX = 'downloads/pmsg-ci/'
FORK_PREFIX = 'downloads/pmsg-ci-fork/'
SOURCES = ('liblog/pmsg_reader.cpp', 'liblog/pmsg_writer.cpp')
PATCH_FILES = (*SOURCES, 'liblog/include/private/android_logger.h', 'liblog/liblog.map.txt')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inputs', type=Path, default=ROOT / 'downloads/pmsg-ci')
    parser.add_argument('--fork-inputs', type=Path, default=ROOT / 'downloads/pmsg-ci-fork')
    parser.add_argument('--out', type=Path, default=ROOT / 'out/android-pmsg')
    parser.add_argument('--cxx', default='clang++')
    args = parser.parse_args()
    spec = importlib.util.spec_from_file_location('android_patches',
                                                R1 / 'scripts/apply-android-patches.py')
    patches = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(patches)
    inputs = patches.confined(args.inputs)
    fork_inputs = patches.confined(args.fork_inputs)
    output = patches.confined(args.out)
    output.mkdir(parents=True, exist_ok=True)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    env = os.environ.copy()
    env.update(TMPDIR=str(ROOT / '.tmp'), ASAN_OPTIONS='detect_leaks=1:abort_on_error=1',
               UBSAN_OPTIONS='halt_on_error=1')

    forks = patches.load_forks(patches.confined(R1 / 'android/patches/series.json'))
    logging = [record for record in forks if record['project'] == 'system/logging']
    if len(logging) != 1:
        raise ValueError('Expected one system/logging fork for the pmsg reader, writer and private API')
    fork = logging[0]
    fork_repository = fork['repository'].removeprefix('https://github.com/').removesuffix('.git')
    lock = json.loads((R1 / 'sources.lock.json').read_text())
    pins = {name.removeprefix(INPUT_PREFIX): value for name, value in lock['ci_files'].items()
            if name.startswith(INPUT_PREFIX)}
    fork_pins = {name.removeprefix(FORK_PREFIX): value for name, value in lock['ci_files'].items()
                 if name.startswith(FORK_PREFIX)}
    if not pins:
        raise ValueError('No pinned pmsg CI inputs')
    if set(fork_pins) != {'system/logging/' + name for name in PATCH_FILES}:
        raise ValueError('Fork inputs must be exactly the pmsg reader, writer and private API')
    verified = {}
    for name, pin in pins.items():
        patches.relative_name(name)
        if name.startswith('system/logging/'):
            expected = ('https://raw.githubusercontent.com/LineageOS/android_system_logging/' +
                        fork['base_commit'] + '/' + name.removeprefix('system/logging/'))
            if pin['url'] != expected:
                raise ValueError('Input revision differs from fork base: ' + name)
        elif name != 'system/core/libcutils/include/cutils/list.h':
            raise ValueError('Unexpected pmsg input: ' + name)
        path = patches.confined(inputs / name)
        data = path.read_bytes()
        if len(data) != pin['bytes'] or digest(data) != pin['sha256']:
            raise ValueError('Pinned input mismatch: ' + name)
        verified[name] = data
    forked = {}
    for name, pin in fork_pins.items():
        expected = ('https://raw.githubusercontent.com/' + fork_repository + '/' + fork['commit'] +
                    '/' + name.removeprefix('system/logging/'))
        if pin['url'] != expected:
            raise ValueError('Fork input revision differs from the pinned fork commit: ' + name)
        data = patches.confined(fork_inputs / name).read_bytes()
        if len(data) != pin['bytes'] or digest(data) != pin['sha256']:
            raise ValueError('Pinned fork input mismatch: ' + name)
        if name not in verified or data == verified[name]:
            raise ValueError('Fork input does not change a pinned original: ' + name)
        forked[name.removeprefix('system/logging/')] = data

    fixture = patches.confined(R1 / 'tests/pmsg/pmsg.cpp')
    results = []
    with tempfile.TemporaryDirectory(prefix='pmsg-', dir=output) as temporary:
        scratch = Path(temporary)
        original = scratch / 'original'
        for name, data in verified.items():
            dest = original / name
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(data)
        library = original / 'system/logging/liblog'
        patched = scratch / 'patched'
        shutil.copytree(original / 'system/logging', patched)
        for name, data in forked.items():
            (patched / name).write_bytes(data)

        flags = [args.cxx, '-std=gnu++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                 # These warnings concern existing upstream initializers and a VLA.
                 '-Wno-missing-field-initializers', '-Wno-vla',
                 '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                 '-ffunction-sections', '-fdata-sections', '-DSNET_EVENT_LOG_TAG=1397638484',
                 '-I' + str(patched / 'liblog/include'),
                 '-I' + str(library), '-I' + str(library / 'include'),
                 '-I' + str(original / 'system/core/libcutils/include')]
        cases = [('userdebug', 1, False, False, None),
                 ('user', 0, False, False, None),
                 ('unpatched-reader', 1, True, False, 'read a record'),
                 ('unpatched-writer', 1, False, True, 'late backend begins receiving logs')]
        for name, debuggable, old_reader, old_writer, expected_failure in cases:
            reader = library / 'pmsg_reader.cpp' if old_reader else patched / SOURCES[0]
            writer = library / 'pmsg_writer.cpp' if old_writer else patched / SOURCES[1]
            binary = scratch / name
            command = [*flags, '-DANDROID_DEBUGGABLE=' + str(debuggable), str(fixture),
                       str(reader), str(writer), '-Wl,--gc-sections',
                       '-Wl,--wrap=open,--wrap=close,--wrap=read,--wrap=lseek,'
                       '--wrap=writev,--wrap=clock_gettime', '-pthread', '-o', str(binary)]
            compile_result = subprocess.run(command, env=env, capture_output=True, text=True,
                                            timeout=180)
            patches.confined(output / (name + '-compile.log')).write_text(
                compile_result.stdout + compile_result.stderr)
            compile_result.check_returncode()
            result = subprocess.run([str(binary)], env=env, capture_output=True, text=True,
                                    timeout=60)
            text = result.stdout + result.stderr
            patches.confined(output / (name + '.log')).write_text(text)
            if expected_failure:
                if (result.returncode != -signal.SIGABRT or
                        not re.search(r'FAIL case \d+: ' + re.escape(expected_failure), text)):
                    raise ValueError('Negative control did not reject the intended regression: ' + name)
                print('PASS: ' + name + ' rejects ' + expected_failure, flush=True)
                count = None
            else:
                result.check_returncode()
                count = re.search(r'PASS: (\d+) production reader/writer scenarios', text)
                if not count or int(count.group(1)) != 33:
                    raise ValueError('Incomplete pmsg regression output: ' + name)
                count = int(count.group(1))
                print(text.strip(), flush=True)
            results.append({'name': name, 'debuggable': debuggable, 'scenarios': count,
                            'expected_failure': expected_failure, 'returncode': result.returncode,
                            'compile_command': command})

    report = {'source_base': fork['base_commit'], 'fork_repository': fork['repository'],
              'fork_commit': fork['commit'], 'inputs': pins, 'fork_inputs': fork_pins,
              'fixture_sha256': digest(fixture.read_bytes()), 'results': results,
              'sanitizers': ['address', 'undefined'], 'hardware_tested': False,
              'limitations': 'Host syscall mocks; no target ABI, SELinux, or persistence validation.'}
    patches.confined(output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'PASS: {len(verified)} pinned inputs and {len(forked)} pinned fork files; '
          'no Android checkout or host device I/O')


if __name__ == '__main__':
    main()
