#!/usr/bin/env python3
"""Print cases from a run_verible_cases.py report.
usage: show_cases.py <report.txt> [source-substring] [count] [skip]"""
import sys
txt = open(sys.argv[1]).read()
sub = sys.argv[2] if len(sys.argv) > 2 else ''
count = int(sys.argv[3]) if len(sys.argv) > 3 else 5
skip = int(sys.argv[4]) if len(sys.argv) > 4 else 0
blocks = [b for b in txt.split('\n=== ')[0:] if sub in b.split('\n', 1)[0]]
for b in blocks[skip:skip + count]:
    print('=== ' + b.strip()[:1800] + '\n')
