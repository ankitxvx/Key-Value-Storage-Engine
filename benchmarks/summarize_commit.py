"""Summarize complete CSV trials without pooling percentiles or hiding ranges."""
import argparse
import csv
import statistics
from pathlib import Path

parser = argparse.ArgumentParser()
parser.add_argument('csv', type=Path)
parser.add_argument('--rounds', type=int, default=10)
parser.add_argument('--output', type=Path)
args = parser.parse_args()
with args.csv.open(encoding='utf-8-sig', newline='') as stream:
    rows = list(csv.DictReader(stream))
if len(rows) != 8 * args.rounds:
    raise SystemExit(f'Incomplete matrix: found {len(rows)} rows, expected {8 * args.rounds}')

lines = [
    '# Durable commit results', '',
    f'Source: `{args.csv.name}`. {args.rounds} runs per configuration.',
    'All latency entries are medians of per-run percentiles, not pooled percentiles.',
    'Parentheses show the minimum and maximum per-run values.', '',
    '| Writers | Group | Writes/s | Mean ms | p50 ms | p99 ms | p99.9 ms | WAL syncs |',
    '|---:|---|---:|---:|---:|---:|---:|---:|',
]
for writers in (1, 4, 8, 16):
    for grouped in (0, 1):
        cases = [r for r in rows if int(r['writers']) == writers and int(r['group_commit']) == grouped]
        if len(cases) != args.rounds or len({r['round'] for r in cases}) != args.rounds:
            raise SystemExit('Missing or duplicate trial')
        if len({r['operations'] for r in cases}) != 1:
            raise SystemExit('Trials used different write counts')
        for r in cases:
            if not grouped and int(r['wal_syncs']) != int(r['operations']):
                raise SystemExit('Ungrouped trial did not sync every request')
        cells = [str(writers), 'On' if grouped else 'Off']
        for key in ('ops_per_sec', 'mean_us', 'p50_us', 'p99_us', 'p999_us', 'wal_syncs'):
            scale = 1000 if key.endswith('_us') else 1
            values = [float(r[key]) / scale for r in cases]
            digits = 3 if scale == 1000 else 1
            cells.append(f'{statistics.median(values):,.{digits}f} ({min(values):,.{digits}f}–{max(values):,.{digits}f})')
        lines.append('| ' + ' | '.join(cells) + ' |')
lines += ['', 'The mean measures call latency directly. Writers/throughput is only an',
          'approximation when requests have think time, uneven completion, or ramp-down.',
          'Sync counts describe grouping; throughput and latency establish the comparison.', '']
text = '\n'.join(lines)
if args.output:
    args.output.write_text(text, encoding='utf-8')
else:
    print(text)
