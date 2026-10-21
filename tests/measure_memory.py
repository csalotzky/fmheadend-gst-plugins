"""Linux peak RSS of the isolated eight-instance shared-library harness."""
import json
import os
from pathlib import Path
import resource
import subprocess

environment=os.environ.copy()
environment.update(GST_PLUGIN_PATH=str(Path('build').resolve()),
                   GST_PLUGIN_SYSTEM_PATH='',
                   GST_REGISTRY=str(Path('build/memory-native-registry.bin').resolve()))
subprocess.run(['nice','-n','15','./build/benchmark_plugins'],env=environment,
               stdout=subprocess.DEVNULL,check=True)
print(json.dumps(dict(eight_instance_harness_peak_rss_kib=resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss)))
