#!/usr/bin/env python3

# Copyright 2026 The svfmt Authors.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""Differential preprocessor check for svfmt.

For each input file: format it, then compare the preprocessed token streams of
the input and the output with svfmt-preproc-cmp, under several define sets
derived from the file's own `ifdef/`ifndef/`elsif guards. This checks what the
raw lexer check cannot: macro expansion, macro arguments, and branch-dependent
macro definitions.

usage: run_preproc_diff.py <build dir> <file or dir>... [--jobs N] [--limit N]
"""
import argparse
import concurrent.futures as cf
import os
import re
import subprocess
import sys
import tempfile

p = argparse.ArgumentParser()
p.add_argument('build')
p.add_argument('paths', nargs='+')
p.add_argument('--jobs', type=int, default=4)
p.add_argument('--limit', type=int, default=0, help='max files per path')
args = p.parse_args()

svfmt = os.path.join(args.build, 'svfmt')
cmp_tool = os.path.join(args.build, 'svfmt-preproc-cmp')
for tool in (svfmt, cmp_tool):
    if not os.path.exists(tool):
        sys.exit(f'missing {tool}')

GUARD = re.compile(r'^\s*`(?:ifdef|ifndef|elsif)\s+([A-Za-z_]\w*)', re.M)


def files_under(path):
    if os.path.isfile(path):
        return [path]
    out = []
    for root, _, names in os.walk(path):
        for n in names:
            path = os.path.join(root, n)
            if n.endswith(('.sv', '.svh', '.v', '.vh')) and os.path.isfile(path):
                out.append(path)
    return sorted(out)


def define_sets(text):
    names = list(dict.fromkeys(GUARD.findall(text)))
    sets = [[]]
    if names:
        sets.append([f'{n}=1' for n in names])
        for n in names[:6]:
            sets.append([f'{n}=1'])
        if '`elsif' in text:
            sets.append([f'{n}=0' for n in names])
            sets.append([f'{n}=16' for n in names])
    unique = []
    for s in sets:
        if s not in unique:
            unique.append(s)
    return unique[:10]


def check_file(path):
    try:
        with open(path, 'rb') as f:
            original = f.read()
    except OSError as e:
        return path, [('read', str(e))]
    fmt = subprocess.run([svfmt, '--noverify_convergence', path],
                         capture_output=True)
    if fmt.returncode != 0 or not fmt.stdout or fmt.stdout == original:
        return path, []
    with tempfile.NamedTemporaryFile(suffix='.sv', delete=False) as out:
        out.write(fmt.stdout)
        out_path = out.name
    try:
        text = original.decode('utf-8', 'replace')
        mismatches = []
        for defines in define_sets(text):
            cmd = [cmp_tool]
            for d in defines:
                cmd += ['--define', d]
            cmd += ['--include-dir', os.path.dirname(os.path.abspath(path))]
            cmd += [path, out_path]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode == 1:
                mismatches.append((','.join(defines) or '<none>',
                                   r.stdout.strip().splitlines()[0]
                                   if r.stdout else '<no output>'))
            elif r.returncode != 0:
                mismatches.append((','.join(defines) or '<none>',
                                   f'cmp exit {r.returncode}: {r.stderr.strip()}'))
        return path, mismatches
    finally:
        os.unlink(out_path)


files = []
for path in args.paths:
    found = files_under(path)
    if args.limit:
        found = found[:args.limit]
    files.extend(found)

mismatched = 0
checked = 0
with cf.ThreadPoolExecutor(max_workers=args.jobs) as pool:
    for path, mismatches in pool.map(check_file, files):
        checked += 1
        if not mismatches:
            continue
        mismatched += 1
        print(f'{path}:')
        for defines, detail in mismatches:
            print(f'  [{defines}] {detail}')

print(f'checked {checked} files, {mismatched} with preprocessor differences')
sys.exit(1 if mismatched else 0)
