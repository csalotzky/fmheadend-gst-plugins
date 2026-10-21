"""Summarize saved matched Pi measurements; stdlib only."""
import csv
import glob
import json
import statistics as stats
from pathlib import Path

groups={}
for filename in sorted(glob.glob('paired-*.csv')+glob.glob('scalar-reference-*.csv')):
    label=Path(filename).stem.rsplit('-',1)[0] if filename.startswith('paired') else Path(filename).stem
    for row in csv.reader(open(filename)):
        mode=int(row[0]); entry=groups.setdefault(f'{label}/mode{mode}',dict(ns=[],cpu=[],max_block=[]))
        entry['ns'].append(float(row[2])); entry['cpu'].append(float(row[3]))
        if len(row)>4: entry['max_block'].append(float(row[4]))
result={}
for key,x in groups.items():
    result[key]=dict(n=len(x['ns']),median_ns=stats.median(x['ns']),mean_ns=stats.mean(x['ns']),stdev_ns=stats.stdev(x['ns']),min_ns=min(x['ns']),max_ns=max(x['ns']),eight_stream_cpu_percent=stats.median(x['cpu']),max_eight_stream_block_ms=max(x['max_block']) if x['max_block'] else None,raw_ns=x['ns'])
Path('benchmark-results.json').write_text(json.dumps(result,indent=2))
for key,x in result.items(): print(key, {k:v for k,v in x.items() if k!='raw_ns'})
