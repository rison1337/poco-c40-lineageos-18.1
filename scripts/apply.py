#!/usr/bin/env python3
"""Apply the exported source set to its exact pinned upstream checkouts."""
from pathlib import Path
import argparse
import hashlib
import json
import shutil
import subprocess

base=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser()
parser.add_argument('source',type=Path)
parser.add_argument('--prebuilts',type=Path,required=True)
args=parser.parse_args()
source=args.source.resolve()
record=json.loads((base/'source-map.json').read_text())
for project in record['projects']:
    checkout=source/project['path']
    head=subprocess.check_output(['git','-C',str(checkout),'rev-parse','HEAD'],text=True).strip()
    if head!=project['base_commit']:raise RuntimeError('Checkout is not the pinned upstream: '+project['path'])
    if 'patch' in project:
        patch=base/project['patch']
        if hashlib.sha256(patch.read_bytes()).hexdigest()!=project['patch_sha256']:raise RuntimeError('Patch hash mismatch')
        subprocess.run(['git','-C',str(checkout),'apply','--check',str(patch)],check=True)
        subprocess.run(['git','-C',str(checkout),'apply',str(patch)],check=True)
for row in record['binary_inputs']:
    target=source/row['path']
    if row['deleted']:
        target.unlink(missing_ok=True)
        continue
    path=args.prebuilts/row['path']
    digest=hashlib.sha256()
    with path.open('rb') as f:
        for data in iter(lambda:f.read(4*1024*1024),b''):digest.update(data)
    if digest.hexdigest()!=row['sha256'] or path.stat().st_size!=row['bytes']:raise RuntimeError('Prebuilt hash mismatch: '+row['path'])
    shutil.copyfile(path,target)
for path in (base/'overlay').rglob('*'):
    if not path.is_file() and not path.is_symlink():continue
    relative=path.relative_to(base/'overlay');target=source/relative
    target.parent.mkdir(parents=True,exist_ok=True)
    if path.is_symlink():
        if target.exists() or target.is_symlink():target.unlink()
        target.symlink_to(path.readlink())
    else:shutil.copy2(path,target)
print('Pinned patches, verified prebuilts and device overlay applied.')
