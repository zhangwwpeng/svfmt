#!/usr/bin/env python3
"""Run svfmt against verible-verilog-format on Verible's own test inputs.

usage: run_verible_cases.py <svfmt> <cases.jsonl> [--out DIR] [--show N] [--filter SUBSTR]

Each case input is formatted by both tools with default flags. Cases that
Verible itself rejects are skipped. Results are bucketed:
  same       identical output
  diff       both succeeded, outputs differ
  rejected   svfmt refused (parse error / unsupported construct), echoed input
  crash      svfmt crashed
"""
import argparse
import concurrent.futures as cf
import json
import os
import subprocess
import tempfile

# Oracle binary; override with VERIBLE_FORMAT=/path/to/verible-verilog-format.
VERIBLE = os.environ.get('VERIBLE_FORMAT', 'verible-verilog-format')

p = argparse.ArgumentParser()
p.add_argument('svfmt')
p.add_argument('cases')
p.add_argument('--out', default=None)
p.add_argument('--show', type=int, default=0)
p.add_argument('--filter', default='')
p.add_argument('--min-same', type=int, default=0, help='exit non-zero below this count')
args = p.parse_args()

cases = [json.loads(l) for l in open(args.cases)]
cases = [c for c in cases if args.filter in c['source']]
tmpdir = tempfile.mkdtemp(prefix='svfmt_cases_')


def run(idx_case):
    idx, case = idx_case
    path = os.path.join(tmpdir, f'c{idx}.sv')
    with open(path, 'w') as f:
        f.write(case['input'])
    v = subprocess.run([VERIBLE, '--failsafe_success=false', path],
                       capture_output=True, text=True)
    if v.returncode != 0:
        return idx, 'verible_rejects', '', '', v.stderr
    s = subprocess.run([args.svfmt, '--failsafe_success=false', path],
                       capture_output=True, text=True)
    if s.returncode < 0 or 'Check failed' in s.stderr or 'SIGSEGV' in s.stderr:
        return idx, 'crash', v.stdout, s.stdout, s.stderr
    if s.returncode != 0:
        return idx, 'rejected', v.stdout, s.stdout, s.stderr
    return idx, ('same' if s.stdout == v.stdout else 'diff'), v.stdout, s.stdout, s.stderr


results = {}
with cf.ThreadPoolExecutor(max_workers=os.cpu_count()) as ex:
    for idx, kind, want, got, err in ex.map(run, enumerate(cases)):
        results.setdefault(kind, []).append((idx, want, got, err))

total = sum(len(v) for k, v in results.items() if k != 'verible_rejects')
for kind in ['same', 'diff', 'rejected', 'crash', 'verible_rejects']:
    print(f'{kind:16s} {len(results.get(kind, []))}')
print(f'pass rate: {len(results.get("same", []))}/{total}')

if args.out:
    os.makedirs(args.out, exist_ok=True)
    for kind in ['diff', 'rejected', 'crash']:
        with open(os.path.join(args.out, f'{kind}.txt'), 'w') as f:
            for idx, want, got, err in results.get(kind, []):
                c = cases[idx]
                f.write(f"=== [{idx}] {c['source']}\n--- input\n{c['input']}\n--- verible\n{want}"
                        f"--- svfmt\n{got}--- stderr\n{err[:2000]}\n")
shown = 0
for kind in ['crash', 'rejected', 'diff']:
    for idx, want, got, err in results.get(kind, []):
        if shown >= args.show:
            break
        shown += 1
        c = cases[idx]
        print(f"\n=== {kind} [{idx}] {c['source']}\n--- input\n{c['input']}\n--- verible\n{want}--- svfmt\n{got}--- stderr\n{err[:800]}")
raise SystemExit(0 if len(results.get('same', [])) >= args.min_same else 1)
