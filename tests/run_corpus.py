#!/usr/bin/env python3
"""Corpus test: semantic safety first, Verible parity second.

usage: run_corpus.py <build dir> <corpus dir> [--out DIR] [--verible PATH]
                     [--timeout SEC] [--jobs N] [--flags "..."]

For every *.sv / *.svh / *.v file under <corpus dir> (recursively):

Safety (any failure here is a bug):
  * svfmt formats it        -> svfmt-lexcmp(input, output) must pass
                               (only whitespace changed), and formatting the
                               output again must not change it (idempotent).
  * svfmt refuses to format -> its output must be the input, byte for byte.
  * svfmt must not crash or time out.

Parity (statistics only): compare svfmt's output with verible-verilog-format.

Exit status is non-zero if any safety check fails.
"""
import argparse
import concurrent.futures as cf
import os
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
args = p.parse_args()

svfmt = os.path.join(args.build, 'svfmt')
lexcmp = os.path.join(args.build, 'svfmt-lexcmp')
flags = args.flags.split()
have_verible = subprocess.run(['which', args.verible], capture_output=True).returncode == 0 \
    or os.path.exists(args.verible)

files = []
for root, _, names in os.walk(args.corpus, followlinks=True):
    for n in names:
        if n.endswith(('.sv', '.svh', '.v', '.vh')):
            files.append(os.path.join(root, n))
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
    elif rc < 0 or b'Check failed' in err:
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


results = []
with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
    for r in ex.map(check, enumerate(files)):
        results.append(r)


def count(key, val):
    return sum(1 for r in results if r.get(key) == val)


BAD = ['semantic_change', 'not_idempotent', 'rejected_but_changed', 'crash', 'timeout']
print(f'files: {len(results)}')
print('safety:')
for k in ['ok_formatted', 'ok_rejected'] + BAD:
    print(f'  {k:22s} {count("safety", k)}')
if have_verible:
    print('parity with verible (formatted files only):')
    for k in ['same', 'diff', 'verible_failed']:
        print(f'  {k:22s} {count("parity", k)}')

if args.out:
    os.makedirs(args.out, exist_ok=True)
    for k in BAD + ['ok_rejected']:
        with open(os.path.join(args.out, f'{k}.txt'), 'w') as f:
            for r in results:
                if r['safety'] == k:
                    f.write(f"=== {r['path']}\n{r.get('detail', '')}\n{r['stderr']}\n")
    with open(os.path.join(args.out, 'parity_diff.txt'), 'w') as f:
        for r in results:
            if r.get('parity') == 'diff':
                f.write(f"=== {r['path']}\n")
    print(f'details in {args.out}/')

sys.exit(1 if any(r['safety'] in BAD for r in results) else 0)
