#!/usr/bin/env python3
"""Small separately retrievable record; full raw evidence remains a distinct artifact."""
import argparse,json,shutil
from pathlib import Path
p=argparse.ArgumentParser();p.add_argument('root',type=Path);p.add_argument('output',type=Path);a=p.parse_args();a.output.mkdir(parents=True,exist_ok=True)
for file in a.root.rglob('*'):
    if not file.is_file():continue
    rel=file.relative_to(a.root)
    include=(len(rel.parts)==1 and file.suffix in ['.json','.txt','.log']) or file.name in ['gate-summary.json','transition-summary.json','retained-summary.json','live-result.json','independent-audit.json','live-independent-audit.json','adapter-manifest.json'] or file.name=='executed-experiment-source.tar.gz'
    if include:
        dest=a.output/rel;dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(file,dest)
print('SUMMARY_FILES',sum(1 for f in a.output.rglob('*') if f.is_file()))
