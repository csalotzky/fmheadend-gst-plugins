"""Target-only statistical regression check on matched plugin benchmark data.
Run after summarize_benchmarks.py, using the offline NumPy/SciPy environment.
This tests for a detected slowdown; it does not prove real-time scheduling.
"""
import json
from pathlib import Path
from scipy import stats

data=json.loads(Path('benchmark-results.json').read_text())
failed=False
for signal in ('synthetic','music'):
    for mode,name in ((0,'limiter disabled'),(2,'clipper hard')):
        a=data[f'paired-plugin-baseline-{signal}/mode{mode}']['raw_ns']
        b=data[f'paired-plugin-new-{signal}/mode{mode}']['raw_ns']
        test=stats.ttest_ind(b,a,equal_var=False)
        difference=sum(b)/len(b)-sum(a)/len(a)
        interval=test.confidence_interval(confidence_level=.95)
        baseline=sum(a)/len(a)
        print(json.dumps(dict(signal=signal,mode=name,mean_difference_ns=difference,p_value=float(test.pvalue),difference_percent_ci95=[float(interval.low/baseline*100),float(interval.high/baseline*100)])))
        failed |= difference>0 and test.pvalue<.05
if failed:
    raise SystemExit('Detected disabled-mode slowdown; inspect target conditions and generated code.')
print('No statistically detected disabled-mode slowdown (Welch, two-sided alpha=0.05).')
