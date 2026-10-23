import argparse
import csv
import json
import math
from pathlib import Path
import statistics


FIELDS = {'trial_id', 'kind', 'result', 'first_result', 'attempts', 'latency_ms'}
RESULTS = {'match', 'no-match', 'timeout', 'error', 'cancelled'}


def interval(successes, total):
    if not total:
        return None
    z = 1.959963984540054
    rate = successes / total
    denominator = 1 + z * z / total
    center = (rate + z * z / (2 * total)) / denominator
    radius = z * math.sqrt(rate * (1 - rate) / total + z * z / (4 * total * total)) / denominator
    return [max(0, center - radius), min(1, center + radius)]


def summarize(stream, threshold):
    if not 1 <= threshold <= 200:
        raise ValueError('Threshold must be in 1..200')
    reader = csv.DictReader(stream)
    if reader.fieldnames is None or len(reader.fieldnames) != len(FIELDS) or set(reader.fieldnames) != FIELDS:
        raise ValueError('Expected columns: ' + ','.join(sorted(FIELDS)))
    seen = set()
    groups = {kind: [] for kind in ['genuine', 'impostor']}
    for line, row in enumerate(reader, 2):
        if None in row or any(value is None for value in row.values()):
            raise ValueError(f'Invalid CSV row at line {line}')
        identity = row['trial_id']
        if not identity or identity in seen:
            raise ValueError(f'Missing or duplicate trial ID at line {line}')
        seen.add(identity)
        if row['kind'] not in groups or row['result'] not in RESULTS or row['first_result'] not in RESULTS:
            raise ValueError(f'Invalid result label at line {line}')
        attempts = int(row['attempts'])
        latency = float(row['latency_ms'])
        if not 0 <= attempts <= 3 or not math.isfinite(latency) or latency < 0:
            raise ValueError(f'Invalid attempts or latency at line {line}')
        if (row['result'] in {'match', 'no-match'} or row['first_result'] in {'match', 'no-match'}) and attempts == 0:
            raise ValueError(f'A comparison requires a placement at line {line}')
        if row['first_result'] == 'match' and attempts != 1:
            raise ValueError(f'An accepted first placement cannot have retries at line {line}')
        if attempts == 1 and row['result'] in {'match', 'no-match'} and row['first_result'] != row['result']:
            raise ValueError(f'Conflicting single-placement results at line {line}')
        groups[row['kind']].append((row['result'], row['first_result'], latency))
    if not seen:
        raise ValueError('No trials')
    report = {'threshold': threshold,
              'interpretation': 'Trial-level results; repeated trials are not independent population estimates.',
              'groups': {}}
    for kind, trials in groups.items():
        counts = {result: sum(row[0] == result for row in trials) for result in sorted(RESULTS)}
        completed = counts['match'] + counts['no-match']
        failures = counts['match' if kind == 'impostor' else 'no-match']
        first = [row[1] for row in trials if row[1] in {'match', 'no-match'}]
        first_failures = first.count('match' if kind == 'impostor' else 'no-match')
        latencies = {}
        for result in sorted(RESULTS):
            values = sorted(row[2] for row in trials if row[0] == result)
            latencies[result] = None if not values else {
                'median_ms': statistics.median(values),
                'p95_ms': values[math.ceil(0.95 * len(values)) - 1]}
        report['groups'][kind] = {
            'trials': len(trials), 'outcomes': counts, 'completed_comparisons': completed,
            'error_rate_name': 'false_acceptance' if kind == 'impostor' else 'false_rejection',
            'error_rate': failures / completed if completed else None,
            'wilson_95_interval': interval(failures, completed),
            'first_placement_comparisons': len(first),
            'first_placement_error_rate': first_failures / len(first) if first else None,
            'latency_by_outcome': latencies}
    return report


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('csv', type=Path)
    parser.add_argument('--threshold', required=True, type=int)
    args = parser.parse_args()
    try:
        with args.csv.open(newline='', encoding='utf-8') as stream:
            result = summarize(stream, args.threshold)
    except (OSError, ValueError) as error:
        parser.exit(1, str(error) + '\n')
    print(json.dumps(result, indent=2, allow_nan=False))


if __name__ == '__main__':
    main()
