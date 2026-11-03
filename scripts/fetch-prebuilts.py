#!/usr/bin/env python3
"""Fetch hashed WebView build inputs from the firmware release."""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import subprocess
import tempfile
import zipfile

base=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser();parser.add_argument('destination',type=Path);args=parser.parse_args()
record=json.loads((base/'source-map.json').read_text())
rows=[row for row in record['binary_inputs'] if not row['deleted']]
missing=[row for row in rows if not (args.destination/row['path']).exists()]
if missing:
    with tempfile.TemporaryDirectory() as temp:
        asset='Source-build-inputs.zip'
        subprocess.run(['gh','release','download','R1','--repo','rison1337/poco-c40-lineageos-18.1','--pattern',asset,'--dir',temp],check=True)
        with zipfile.ZipFile(Path(temp)/asset) as archive:
            for row in missing:
                path=args.destination/row['path'];path.parent.mkdir(parents=True,exist_ok=True)
                name='webview-'+Path(row['path']).parent.name+'.apk'
                with archive.open(name) as source,path.open('wb') as destination:
                    shutil.copyfileobj(source,destination,4*1024*1024)
for row in rows:
    path=args.destination/row['path']
    digest=hashlib.sha256()
    with path.open('rb') as f:
        for data in iter(lambda:f.read(4*1024*1024),b''):digest.update(data)
    if digest.hexdigest()!=row['sha256']:raise RuntimeError('Prebuilt hash mismatch: '+row['path'])
print('All prebuilt inputs match the recorded hashes.')
