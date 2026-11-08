#!/usr/bin/env python3
"""Corpus test: semantic safety first, Verible parity second.

usage: run_corpus.py <build dir> <corpus dir> [--out DIR] [--verible PATH]
                     [--timeout SEC] [--jobs N] [--flags "..."]
                     [--no-parity] [--cases DIR] [--jsonl FILE]

For every *.sv / *.svh / *.v file under <corpus dir> (recursively):

Safety (any failure here is a bug):
  * svfmt formats it        -> svfmt-lexcmp(input, output) must pass
                               (only whitespace changed), and formatting the
                               output again must not change it (idempotent).
  * svfmt refuses to format -> its output must be the input, byte for byte.
  * svfmt must not crash or time out.

Parity (statistics only): compare svfmt's output with verible-verilog-format.

Rejections of files listed in tests/corpus_expected_rejects.txt (invalid on
purpose, e.g. sv2v error tests) are counted as expected_rejected, not as
ok_rejected, so the remaining ok_rejected are the rejections to look at.

Exit status is non-zero if any safety check fails.
"""
import argparse
import json
import shlex
import concurrent.futures as cf
import fnmatch
import os
import shutil
import subprocess
import sys
import tempfile

p = argparse.ArgumentParser()
p.add_argument('build')
p.add_argument('corpus')
p.add_argument('--out', default=None, help='write details of every failure here')
p.add_argument('--verible', default=os.environ.get('VERIBLE_FORMAT', 'verible-verilog-format'))
p.add_argument('--timeout', type=float, default=60)
p.add_argument('--jobs', type=int, default=os.cpu_count())
p.add_argument('--flags', default='', help='extra flags for both formatters')
p.add_argument('--no-parity', action='store_true', help='skip the Verible comparison')
p.add_argument('--cases', default=None,
               help='write one directory per safety failure (input, outputs, logs, repro)')
p.add_argument('--jsonl', default=None, help='write one JSON result per file')
p.add_argument('--expected-rejects', default=os.path.join(
                   os.path.dirname(os.path.abspath(__file__)), 'corpus_expected_rejects.txt'),
               help='glob patterns (relative to <corpus dir>) of files that are invalid on '
                    'purpose; a rejection of one is reported as expected_rejected')
args = p.parse_args()

svfmt = os.path.join(args.build, 'svfmt')
lexcmp = os.path.join(args.build, 'svfmt-lexcmp')
flags = args.flags.split()
have_verible = not args.no_parity and (
    subprocess.run(['which', args.verible], capture_output=True).returncode == 0
    or os.path.exists(args.verible))

files = []
seen_dirs = set()  # symlinked directories may point back up the tree
for root, dirs, names in os.walk(args.corpus, followlinks=True):
    real = os.path.realpath(root)
    if real in seen_dirs:
        dirs[:] = []
        continue
    seen_dirs.add(real)
    for n in names:
        path = os.path.join(root, n)
        # isfile() also skips dangling symlinks.
        if n.endswith(('.sv', '.svh', '.v', '.vh')) and os.path.isfile(path):
            files.append(path)
files.sort()
tmpdir = tempfile.mkdtemp(prefix='svfmt_corpus_')


def run(cmd, data=None):
    try:
        r = subprocess.run(cmd, capture_output=True, timeout=args.timeout, input=data)
        return r.returncode, r.stdout, r.stderr
    except subprocess.TimeoutExpired:
        return 'timeout', b'', b''


def check(idx_path):
    idx, path = idx_path
    original = open(path, 'rb').read()
    res = {'path': path}
    rc, out, err = run([svfmt, '--failsafe_success=false', '--verify_convergence=false',
                        *flags, path])
    res['stderr'] = err.decode(errors='replace')[:3000]
    if rc == 'timeout':
        res['safety'] = 'timeout'
    elif rc < 0 or b'] Check failed:' in err:
        res['safety'] = 'crash'
    elif rc != 0:
        res['safety'] = 'ok_rejected' if out == original else 'rejected_but_changed'
    else:
        tmp = os.path.join(tmpdir, f'{idx}.sv')
        open(tmp, 'wb').write(out)
        lrc, lout, _ = run([lexcmp, path, tmp])
        if lrc != 0:
            res['safety'] = 'semantic_change'
            res['detail'] = lout.decode(errors='replace')
        else:
            rc2, out2, err2 = run([svfmt, '--failsafe_success=false', '--verify_convergence=false',
                                   *flags, tmp])
            res['formatted2'] = out2
            if rc2 != 0 or out2 != out:
                res['safety'] = 'not_idempotent'
                res['detail'] = err2.decode(errors='replace')[:2000]
            else:
                res['safety'] = 'ok_formatted'
    res['formatted'] = out
    if have_verible and res['safety'] == 'ok_formatted':
        vrc, vout, _ = run([args.verible, '--failsafe_success=false', *flags, path])
        if vrc != 0:
            res['parity'] = 'verible_failed'
        else:
            res['parity'] = 'same' if vout == out else 'diff'
            res['verible'] = vout
    return res


BAD = ['semantic_change', 'not_idempotent', 'rejected_but_changed', 'crash', 'timeout']


def save_case(res):
    name = os.path.relpath(res['path'], args.corpus).replace(os.sep, '__')
    d = os.path.join(args.cases, res['safety'], name)
    os.makedirs(d, exist_ok=True)
    shutil.copyfile(res['path'], os.path.join(d, 'input.sv'))
    open(os.path.join(d, 'output.sv'), 'wb').write(res.get('formatted', b''))
    if 'formatted2' in res:
        open(os.path.join(d, 'output2.sv'), 'wb').write(res['formatted2'])
    open(os.path.join(d, 'lexcmp.txt'), 'w').write(res.get('detail', ''))
    open(os.path.join(d, 'stderr.txt'), 'w').write(res['stderr'])
    fmt = ' '.join(shlex.quote(x) for x in [os.path.abspath(svfmt), '--failsafe_success=false',
                                             '--verify_convergence=false', *flags])
    open(os.path.join(d, 'cmd.sh'), 'w').write(
        '#!/bin/sh\n# original: %s\ncd "$(dirname "$0")"\n'
        '%s input.sv > output.sv; echo "svfmt exit $?"\n'
        '%s input.sv output.sv; echo "lexcmp exit $?"\n'
        '%s output.sv > output2.sv; cmp output.sv output2.sv && echo idempotent\n'
        % (res['path'], fmt, shlex.quote(os.path.abspath(lexcmp)), fmt))
    os.chmod(os.path.join(d, 'cmd.sh'), 0o755)


results = []
with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
    for r in ex.map(check, enumerate(files)):
        results.append(r)

# A rejection of a file listed as invalid on purpose is not a finding.
expected_patterns = []
if os.path.exists(args.expected_rejects):
    with open(args.expected_rejects) as f:
        expected_patterns = [line.split('#')[0].strip() for line in f]
        expected_patterns = [p for p in expected_patterns if p]
for r in results:
    if r['safety'] != 'ok_rejected':
        continue
    rel = os.path.relpath(r['path'], args.corpus)
    if any(fnmatch.fnmatch(rel, pat) for pat in expected_patterns):
        r['safety'] = 'expected_rejected'


def count(key, val):
    return sum(1 for r in results if r.get(key) == val)


print(f'files: {len(results)}')
print('safety:')
for k in ['ok_formatted', 'ok_rejected', 'expected_rejected'] + BAD:
    print(f'  {k:22s} {count("safety", k)}')
if have_verible:
    print('parity with verible (formatted files only):')
    for k in ['same', 'diff', 'verible_failed']:
        print(f'  {k:22s} {count("parity", k)}')

if args.out:
    os.makedirs(args.out, exist_ok=True)
    for k in BAD + ['ok_rejected', 'expected_rejected']:
        with open(os.path.join(args.out, f'{k}.txt'), 'w') as f:
            for r in results:
                if r['safety'] == k:
                    f.write(f"=== {r['path']}\n{r.get('detail', '')}\n{r['stderr']}\n")
    with open(os.path.join(args.out, 'parity_diff.txt'), 'w') as f:
        for r in results:
            if r.get('parity') == 'diff':
                f.write(f"=== {r['path']}\n")
    print(f'details in {args.out}/')

if args.cases:
    for r in results:
        if r['safety'] in BAD:
            save_case(r)
    print(f'failure cases in {args.cases}/')

if args.jsonl:
    with open(args.jsonl, 'w') as f:
        for r in results:
            f.write(json.dumps({'path': r['path'], 'safety': r['safety'],
                                'parity': r.get('parity')}) + '\n')

sys.exit(1 if any(r['safety'] in BAD for r in results) else 0)
