"""Extend an existing audio-analysis.json with detector accuracy, scalar parity,
explicit -15 dBFS threshold tests, activity counts and two-tone intermodulation.
The expensive original output matrix need not be repeated.
"""
import json
from pathlib import Path
import numpy as np
from analyze_audio import FS, T, render, weighted_peak

path=Path('audio-analysis.json')
report=json.loads(path.read_text())
n=np.arange(8192)
gate=(n>=512)&(n<7000)
mono=lambda x:np.column_stack([x,x])
cases={
    'alternating':mono(.6*(-1.)**n*gate),
    'safe_samples_12k':mono(.45*np.sin(2*np.pi*.25*n+np.pi/4)*gate),
    'near_nyquist':mono(.6*np.sin(2*np.pi*.49*n)*gate),
    'uncorrelated_noise':np.random.default_rng(770).uniform(-1,1,(len(n),2))*gate[:,None],
}
report['detector_accuracy']=[]
report['conservative_threshold']=[]
report['weighted_sample_safe']=[]
error=0.
for w in (0,50,75):
    # Scale so the *weighted native* detector is safe, not just flat PCM.
    r=np.exp(-1/(FS*w*1e-6)) if w else 0
    original=cases['safe_samples_12k']
    native=(original-r*np.vstack([np.zeros((1,2)),original[:-1]]))/(1-r)
    x=np.asarray(original*(.98*T/np.max(np.abs(native))),dtype=np.float32)
    native=(x-r*np.vstack([np.zeros((1,2)),x[:-1]]))/(1-r)
    assert np.max(np.abs(native))<T
    legacy=render(x,0,w)
    assert np.array_equal(legacy[240:],x[:-240])
    old_peak=weighted_peak(legacy,w)
    new_peak=weighted_peak(render(x,1,w),w)
    assert old_peak>T and new_peak<old_peak*.95
    report['weighted_sample_safe'].append(dict(weighting=w,native_peak=float(np.max(np.abs(native))),legacy_overshoot_db=float(20*np.log10(old_peak/T)),true_peak_overshoot_db=float(20*np.log10(new_peak/T))))
    for name in ('alternating','safe_samples_12k','near_nyquist'):
        x=cases[name]
        estimated=float(np.max(render(x,4,w)))
        reference=weighted_peak(x,w)
        report['detector_accuracy'].append(dict(signal=name,weighting=w,estimate=estimated,reference=reference,error_db=float(20*np.log10(estimated/reference))))
    for attack in (0,.25):
        peak=weighted_peak(render(cases['alternating'],1,w,-15,attack),w)
        report['conservative_threshold'].append(dict(weighting=w,attack=attack,overshoot_db=float(20*np.log10(peak/T))))
        assert peak<=T
    for executable in ('build/render_audio_neon','build/render_audio_scalar'):
        a=render(cases['uncorrelated_noise'],1,w)
        b=render(cases['uncorrelated_noise'],1,w,executable=executable)
        error=max(error,float(np.max(np.abs(a-b))))
assert error<2e-6
report['scalar_max_error']=error
nn=np.arange(FS*2)
cases['two_tone']=mono(.45*(np.sin(2*np.pi*1000*nn/FS)+np.sin(2*np.pi*1300*nn/FS)))
excerpt=np.fromfile('music-bench.f32',dtype='<f4').reshape(-1,2)
cases['music']=np.vstack([np.zeros((256,2)),excerpt,np.zeros((1024,2))])
for comparison in report['comparisons']:
    x=cases[comparison['signal']]; w=comparison['weighting']
    r=np.exp(-1/(FS*w*1e-6)) if w else 0
    for mode,key in ((2,'hard'),(3,'soft')):
        y=render(x,mode,w)
        candidate=(x-r*np.vstack([np.zeros((1,2)),y[:-1]]))/(1-r)
        metrics=comparison[key]
        metrics['modified_samples']=int(np.count_nonzero(y!=np.asarray(x,dtype=np.float32)))
        metrics['hard_ceiling_exceedances']=int(np.count_nonzero(np.abs(candidate)>T))
        if comparison['signal']=='two_tone':
            amp=2*np.abs(np.fft.rfft(y[-FS:,0]))/FS
            metrics['imd_products_300_700_1600_hz']=[float(amp[k]) for k in (300,700,1600)]
path.write_text(json.dumps(report,indent=2))
print(json.dumps({k:report[k] for k in ('detector_accuracy','conservative_threshold','weighted_sample_safe','scalar_max_error')},indent=2))
