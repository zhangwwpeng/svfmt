#!/usr/bin/env python3
"""Mutation test for the svfmt safety checks.

For each input file, generate small mutants and check the verdict of
svfmt-lexcmp (the raw whitespace-only check):

  * semantic mutants (a token added/removed/changed) must be reported as
    different;
  * whitespace-only mutants (indentation, trailing spaces, blank lines) must
    be accepted.

With --format, each mutant is also formatted and the formatter's output is
checked with svfmt-lexcmp against the mutant (a formatting run must never
change the token stream; a rejection is fine).

usage: run_mutation.py <build dir> <file or dir>... [--jobs N] [--limit N]
                       [--mutations N] [--seed N] [--format]
"""
import argparse
import concurrent.futures as cf
import os
import random
import re
import subprocess
import sys
import tempfile

p = argparse.ArgumentParser()
p.add_argument('build')
p.add_argument('paths', nargs='+')
p.add_argument('--jobs', type=int, default=4)
p.add_argument('--limit', type=int, default=0, help='max files per path')
p.add_argument('--mutations', type=int, default=12, help='mutants per file')
p.add_argument('--seed', type=int, default=1)
p.add_argument('--format', action='store_true', help='also format each mutant')
p.add_argument('--keep-failures', default=None, help='write failing mutants here')
args = p.parse_args()

svfmt = os.path.join(args.build, 'svfmt')
lexcmp = os.path.join(args.build, 'svfmt-lexcmp')
for tool in (svfmt, lexcmp):
    if not os.path.exists(tool):
        sys.exit(f'missing {tool}')

IDENT = re.compile(r'\b[A-Za-z_]\w*\b')
NUMBER = re.compile(r'\b\d+\b')
OPERATORS = [('+', '-'), ('-', '+'), ('==', '!='), ('!=', '=='), ('<', '>'),
             ('>', '<'), ('<=', '>='), ('>=', '<='), ('&&', '||'), ('||', '&&')]


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


def safe_lines(lines):
    """Lines where changing leading/trailing whitespace is not significant:
    outside `define continuations and outside open brackets."""
    safe = []
    depth = 0
    in_define = False
    in_block = False
    for i, line in enumerate(lines):
        stripped = line.strip()
        if stripped.startswith('`define'):
            in_define = True
        prev_continues = i > 0 and lines[i - 1].rstrip().endswith('\\')
        if (depth == 0 and not in_define and not in_block and stripped
                and not prev_continues and not line.rstrip().endswith('\\')
                and '/*' not in line and '*/' not in line
                and not stripped.startswith('//') and not stripped.startswith('*')):
            safe.append(i)
        depth += line.count('(') + line.count('[') + line.count('{')
        depth -= line.count(')') + line.count(']') + line.count('}')
        if in_define and not line.rstrip().endswith('\\'):
            in_define = False
        j = 0
        while j < len(line):
            if not in_block and line.startswith('/*', j):
                in_block = True
                j += 2
            elif in_block and line.startswith('*/', j):
                in_block = False
                j += 2
            else:
                j += 1
    return safe


def make_mutants(text, count, seed):
    lines = text.splitlines(keepends=True)
    rng = random.Random(seed)
    mutants = []
    nonblank = [i for i, l in enumerate(lines) if l.strip()]
    safe = safe_lines(lines)

    def add(name, new_lines):
        mutants.append((name, ''.join(new_lines)))

    for _ in range(count):
        kind = rng.choice(['delete', 'duplicate', 'identifier', 'number',
                           'operator', 'comment', 'indent', 'trailing', 'blank'])
        if kind == 'delete' and nonblank:
            i = rng.choice(nonblank)
            add('delete line', lines[:i] + lines[i + 1:])
        elif kind == 'duplicate' and nonblank:
            i = rng.choice(nonblank)
            add('duplicate line', lines[:i + 1] + [lines[i]] + lines[i + 1:])
        elif kind == 'identifier':
            candidates = [i for i in nonblank
                          if IDENT.search(lines[i]) and '//' not in lines[i]
                          and '"' not in lines[i]]
            if candidates:
                i = rng.choice(candidates)
                m = rng.choice(list(IDENT.finditer(lines[i])))
                new = lines[i][:m.start()] + m.group() + '_mut' + lines[i][m.end():]
                add('identifier', lines[:i] + [new] + lines[i + 1:])
        elif kind == 'number':
            candidates = [i for i in nonblank
                          if NUMBER.search(lines[i]) and '//' not in lines[i]
                          and '"' not in lines[i]]
            if candidates:
                i = rng.choice(candidates)
                m = rng.choice(list(NUMBER.finditer(lines[i])))
                new = lines[i][:m.start()] + str(int(m.group()) + 1) + lines[i][m.end():]
                add('number', lines[:i] + [new] + lines[i + 1:])
        elif kind == 'operator':
            candidates = [i for i in nonblank
                          if '//' not in lines[i] and '"' not in lines[i]]
            rng.shuffle(candidates)
            for i in candidates:
                for old, new_op in OPERATORS:
                    pos = lines[i].find(old)
                    if pos >= 0:
                        new = lines[i][:pos] + new_op + lines[i][pos + len(old):]
                        add('operator', lines[:i] + [new] + lines[i + 1:])
                        break
                else:
                    continue
                break
        elif kind == 'comment':
            candidates = [i for i in nonblank if '//' in lines[i]]
            if candidates:
                i = rng.choice(candidates)
                pos = lines[i].find('//')
                ending = '\r\n' if lines[i].endswith('\r\n') else (
                    '\n' if lines[i].endswith('\n') else '')
                add('comment', lines[:i] + [lines[i][:pos] + '// mutated comment' + ending]
                    + lines[i + 1:])
        elif kind == 'indent' and safe:
            i = rng.choice(safe)
            add('indent', lines[:i] + ['  ' + lines[i]] + lines[i + 1:])
        elif kind == 'trailing':
            candidates = [i for i in safe if '//' not in lines[i]]
            if candidates:
                i = rng.choice(candidates)
                body = lines[i].rstrip('\r\n')
                ending = lines[i][len(body):]
                add('trailing', lines[:i] + [body + '   ' + ending] + lines[i + 1:])
        elif kind == 'blank' and safe:
            i = rng.choice(safe)
            add('blank', lines[:i] + ['\n'] + lines[i:])
    return mutants


def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def check_file(path):
    with open(path, 'r', encoding='utf-8', errors='replace') as f:
        text = f.read()
    rng = random.Random(args.seed + sum(map(ord, path)))
    mutants = make_mutants(text, args.mutations, rng.randrange(1 << 30))
    results = []
    with tempfile.TemporaryDirectory() as tmp:
        orig = path
        for n, (kind, mutant) in enumerate(mutants):
            mpath = os.path.join(tmp, f'm{n}.sv')
            with open(mpath, 'w', encoding='utf-8') as f:
                f.write(mutant)
            r = run([lexcmp, orig, mpath])
            whitespace = kind in ('indent', 'trailing', 'blank')
            if whitespace:
                if r.returncode != 0:
                    results.append((kind, 'whitespace rejected', r.stdout.strip()[:200]))
                    if args.keep_failures:
                        os.makedirs(args.keep_failures, exist_ok=True)
                        keep = os.path.join(args.keep_failures,
                                            f'{os.path.basename(path)}.{kind}.{n}.sv')
                        with open(keep, 'w', encoding='utf-8') as g:
                            g.write(mutant)
            elif r.returncode == 0:
                results.append((kind, 'semantic mutant accepted', ''))
            if args.format:
                f = run([svfmt, '--failsafe_success=false', '--noverify_convergence', mpath])
                if f.returncode == 0 and f.stdout:
                    out = os.path.join(tmp, f'o{n}.sv')
                    with open(out, 'w', encoding='utf-8') as g:
                        g.write(f.stdout)
                    c = run([lexcmp, mpath, out])
                    if c.returncode != 0:
                        results.append((kind, 'formatter changed tokens',
                                        c.stdout.strip()[:200]))
    return path, results


files = []
for path in args.paths:
    found = files_under(path)
    if args.limit:
        found = found[:args.limit]
    files.extend(found)

bad = 0
with cf.ThreadPoolExecutor(max_workers=args.jobs) as pool:
    for path, results in pool.map(check_file, files):
        if not results:
            continue
        bad += 1
        print(f'{path}:')
        for kind, what, detail in results:
            print(f'  {kind}: {what}' + (f' ({detail})' if detail else ''))

print(f'checked {len(files)} files, {bad} with failures')
sys.exit(1 if bad else 0)
