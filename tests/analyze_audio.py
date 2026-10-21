"""Independent offline reference: SciPy 32x Kaiser FIR (8193 taps), float64.
Run from an isolated build directory: python3 tests/analyze_audio.py.
Requires NumPy/SciPy for tests only, never for the plugins.
"""
import json
import subprocess
from pathlib import Path
import numpy as np
from scipy.signal import resample_poly, firwin

FS = 48000
T = 10 ** (-9 / 20)
REFERENCE = firwin(8193, 1 / 32, window=("kaiser", 10.0))


def weighted_peak(x, weighting, factor=32):
    h = REFERENCE if factor == 32 else firwin(16385, 1 / factor, window=("kaiser", 10.0))
    y = resample_poly(np.asarray(x, dtype=np.float64), factor, 1, axis=0, window=h)
    r = np.exp(-1 / (FS * factor * weighting * 1e-6)) if weighting else 0
    return float(np.max(np.abs((y - r * np.vstack([np.zeros((1, 2)), y[:-1]])) / (1-r))))


def render(x, mode, weighting, threshold=-9, attack=0.25, executable="build/render_audio"):
    np.asarray(x, dtype="<f4").tofile("probe-input.f32")
    subprocess.run([executable, "probe-input.f32", "probe-output.f32", str(mode), str(weighting), str(threshold), str(attack)], check=True)
    return np.fromfile("probe-output.f32", dtype="<f4").reshape(-1, 2).copy()


def main():
    n = np.arange(8192)
    gate = (n >= 512) & (n < 7000)
    mono = lambda x: np.column_stack([x, x])
    cases = {}
    cases["safe_samples_12k"] = mono(0.45 * np.sin(2*np.pi*0.25*n+np.pi/4)*gate)
    cases["near_nyquist"] = mono(0.6*np.sin(2*np.pi*0.49*n)*gate)
    cases["alternating"] = mono(0.6*(-1.)**n*gate)
    cases["impulse"] = mono((n == 1000).astype(float))
    cases["step"] = mono(0.7*gate)
    cases["hf_bursts"] = mono(0.8*np.sin(2*np.pi*0.37*n)*((n % 511) < 13)*gate)
    cases["sweep"] = mono(0.8*np.sin(2*np.pi*(0.005*n+0.24*n*n/len(n)))*gate)
    cases["rapid_gain"] = mono(np.sin(2*np.pi*0.31*n)*np.where(n % 401 < 200,0.02,1.2)*gate)
    rng = np.random.default_rng(770)
    cases["uncorrelated_noise"] = rng.uniform(-1, 1, (len(n),2))*gate[:,None]
    cases["stereo_tones"] = np.column_stack([np.sin(2*np.pi*0.31*n),0.7*np.sin(2*np.pi*0.17*n)])*gate[:,None]
    music = Path("music-bench.f32")
    if music.exists():
        excerpt = np.fromfile(music,dtype="<f4").reshape(-1,2)
        cases["music"] = np.vstack([np.zeros((256,2)),excerpt,np.zeros((1024,2))])
    results=[]
    for name,x in cases.items():
        for w in (0,50,75):
            for threshold,attack in ((-9,0.25),(-11.5,0.25),(-9,0)):
                peaks=[]
                for mode in (0,1):
                    out=render(x,mode,w,threshold,attack)
                    peak=weighted_peak(out,w)
                    peaks.append(20*np.log10(max(peak,1e-20)/T))
                results.append(dict(signal=name,weighting=w,threshold_dbfs=threshold,attack=attack,legacy_db=peaks[0],true_peak_db=peaks[1],improvement_db=peaks[0]-peaks[1]))
        print(name,flush=True)
    # Independent reference resolution convergence on a rapidly modulated output.
    out=render(cases["rapid_gain"],1,75)
    convergence=20*np.log10(weighted_peak(out,75,64)/weighted_peak(out,75,32))
    # Compare NEON/scalar complete outputs if scalar renderer was built.
    scalar_error=0.
    for reference_exe in ("build/render_audio_neon", "build/render_audio_scalar"):
      if Path(reference_exe).exists():
        for w in (0,50,75):
            a=render(cases["uncorrelated_noise"],1,w)
            b=render(cases["uncorrelated_noise"],1,w,executable=reference_exe)
            scalar_error=max(scalar_error,float(np.max(np.abs(a-b))))
        assert scalar_error<2e-6, scalar_error
    assert all(np.isfinite(r['true_peak_db']) for r in results)
    safe=[r for r in results if r['signal']=='safe_samples_12k' and r['threshold_dbfs']==-9 and r['attack']==.25]
    assert all(r['improvement_db']>1 for r in safe), safe
    # THD and harmonics at coherent bins; settled portion excludes startup.
    quality=[]
    nn=np.arange(FS*2)
    for w in (0,50,75):
        for freq in (100,1000,10000):
            for level in (-18,-6,0):
                x=mono(10**(level/20)*np.sin(2*np.pi*freq*nn/FS))
                outputs=[render(x,m,w)[FS:,0] for m in (2,3)]
                for mode,y in zip(('hard','soft'),outputs):
                    spec=np.abs(np.fft.rfft(y))*2/FS
                    hs=[float(spec[(k*freq)%FS if (k*freq)%FS<=FS//2 else FS-(k*freq)%FS]) for k in range(2,10)]
                    quality.append(dict(weighting=w,frequency=freq,level=level,mode=mode,rms=float(np.sqrt(np.mean(y*y))),thd=float(np.sqrt(sum(v*v for v in hs))/max(spec[freq],1e-20)),harmonics_2_to_9=hs))
    comparisons=[]
    for name,x in {**{k:cases[k] for k in ('music','uncorrelated_noise') if k in cases},'two_tone':mono(.45*(np.sin(2*np.pi*1000*nn/FS)+np.sin(2*np.pi*1300*nn/FS)))}.items():
        for w in (0,50,75):
            hard=render(x,2,w); soft=render(x,3,w)
            def metrics(y):
                spectrum=np.abs(np.fft.rfft(y[:,0]))**2
                freq=np.fft.rfftfreq(len(y),1/FS)
                r=np.exp(-1/(FS*w*1e-6)) if w else 0
                candidate=(x-r*np.vstack([np.zeros((1,2)),y[:-1]]))/(1-r)
                result=dict(rms=float(np.sqrt(np.mean(y*y))),hf_energy_fraction=float(spectrum[freq>15000].sum()/max(spectrum.sum(),1e-30)),weighted_peak=weighted_peak(y,w),modified_samples=int(np.count_nonzero(y!=np.asarray(x,dtype=np.float32))),hard_ceiling_exceedances=int(np.count_nonzero(np.abs(candidate)>T)))
                if name=='two_tone':
                    amp=2*np.abs(np.fft.rfft(y[-FS:,0]))/FS
                    result['imd_products_300_700_1600_hz']=[float(amp[k]) for k in (300,700,1600)]
                return result
            comparisons.append(dict(signal=name,weighting=w,hard=metrics(hard),soft=metrics(soft),difference_rms=float(np.sqrt(np.mean((soft-hard)**2)))))
    # Frequency response of the specified unweighted interpolator: all phase
    # coefficients interleaved form one 4x lowpass with DC gain four.
    h=np.zeros(132)
    for p in range(4):
        t=np.arange(33)-16+p/4
        window=np.where(abs(t)<=16,.42+.5*np.cos(np.pi*t/16)+.08*np.cos(2*np.pi*t/16),0)
        taps=np.sinc(t)*window; taps/=sum(taps); h[p::4]=taps
    response=np.abs(np.fft.rfft(h,131072))/4
    freq=np.fft.rfftfreq(131072,1/4)
    filter_response=dict(passband_0_to_0_4_db=[float(20*np.log10(response[freq<=.4].min())),float(20*np.log10(response[freq<=.4].max()))],stopband_from_0_6_db=float(20*np.log10(response[freq>=.6].max())))
    report=dict(peaks=results,reference_64x_difference_db=convergence,scalar_max_error=scalar_error,quality=quality,comparisons=comparisons,filter_response=filter_response)
    Path('audio-analysis.json').write_text(json.dumps(report,indent=2))
    print(json.dumps(dict(reference_64x_difference_db=convergence,scalar_max_error=scalar_error,filter_response=filter_response)))

if __name__=='__main__': main()
