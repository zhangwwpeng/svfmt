#!/usr/bin/env python3
"""Extract formatter test inputs from Verible's formatter_*test.cc files.

Verible keeps its formatter tests as C++ tables of {input, expected} string
literal pairs. This collects every such pair into a JSON-lines file:
  {"source": "formatter_test.cc:123", "input": "...", "expected": "..."}

The expected outputs are not always for the default style (tests set their
own FormatStyle), so tests/run_verible_cases.py uses the verible binary as the
oracle and only keeps "expected" for reference.

usage: extract_verible_tests.py <verible>/verible/verilog/formatting out.jsonl
"""
import json
import os
import re
import sys

ESCAPES = {'n': '\n', 't': '\t', 'r': '\r', '\\': '\\', '"': '"', "'": "'", '0': '\0',
           'a': '\a', 'b': '\b', 'f': '\f', 'v': '\v', '?': '?'}


def lex(src):
    """Yields (kind, value, line) for string literals and punctuation."""
    i, n, line = 0, len(src), 1
    while i < n:
        c = src[i]
        if c == '\n':
            line += 1; i += 1
        elif src.startswith('//', i):
            j = src.find('\n', i); i = n if j < 0 else j
        elif src.startswith('/*', i):
            j = src.find('*/', i); line += src.count('\n', i, j); i = j + 2
        elif src.startswith('R"', i):
            m = re.match(r'R"([^(]*)\(', src[i:])
            delim = ')' + m.group(1) + '"'
            start = i + m.end(); end = src.find(delim, start)
            val = src[start:end]; line += val.count('\n')
            yield ('str', val, line); i = end + len(delim)
        elif c == '"':
            j = i + 1; out = []
            while src[j] != '"':
                if src[j] == '\\':
                    e = src[j + 1]
                    if e == 'x':
                        m = re.match(r'[0-9a-fA-F]+', src[j + 2:]); out.append(chr(int(m.group(0), 16))); j += 2 + len(m.group(0))
                        continue
                    out.append(ESCAPES.get(e, e)); j += 2
                else:
                    out.append(src[j]); j += 1
            yield ('str', ''.join(out), line); i = j + 1
        elif c == "'":
            j = i + 1
            while src[j] != "'":
                j += 2 if src[j] == '\\' else 1
            i = j + 1
        elif c in '{},':
            yield ('punct', c, line); i += 1
        elif c.isspace():
            i += 1
        else:
            yield ('other', c, line); i += 1


def pairs(src):
    toks = list(lex(src))
    out = []
    k = 0
    while k < len(toks):
        if toks[k][1] == '{' and toks[k][0] == 'punct':
            # { str+ , str+ ,? }
            j = k + 1; parts = [[], []]; field = 0; ok = True
            while j < len(toks):
                kind, val, _ = toks[j]
                if kind == 'str':
                    parts[field].append(val)
                elif kind == 'punct' and val == ',' and field == 0 and parts[0]:
                    field = 1
                elif kind == 'punct' and val == ',' and field == 1 and parts[1]:
                    pass
                elif kind == 'punct' and val == '}':
                    break
                else:
                    ok = False; break
                j += 1
            if ok and j < len(toks) and parts[0] and parts[1]:
                out.append((toks[k][2], ''.join(parts[0]), ''.join(parts[1])))
                k = j + 1; continue
        k += 1
    return out


def main():
    src_dir, out_path = sys.argv[1], sys.argv[2]
    n = 0
    with open(out_path, 'w') as out:
        for name in sorted(os.listdir(src_dir)):
            if not (name.startswith('formatter') and name.endswith('_test.cc')):
                continue
            src = open(os.path.join(src_dir, name)).read()
            for line, inp, exp in pairs(src):
                out.write(json.dumps({'source': f'{name}:{line}', 'input': inp, 'expected': exp}) + '\n')
                n += 1
    print(f'{n} cases -> {out_path}')


main()
