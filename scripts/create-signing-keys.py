#!/usr/bin/env python3
"""Create a builder's own APK/APEX keys; never fetch the maintainer's signing keys."""
from pathlib import Path
import argparse
import json
import os
import subprocess

os.umask(0o077)
base=Path(__file__).resolve().parents[1]
parser=argparse.ArgumentParser();parser.add_argument('keys',type=Path);parser.add_argument('--avbtool',type=Path,required=True);args=parser.parse_args()
args.keys.mkdir(parents=True,exist_ok=True);args.keys.chmod(0o700)
inputs=json.loads((base/'apex-signing-inputs.json').read_text())
def run(*cmd):subprocess.run([str(p) for p in cmd],check=True,stdout=subprocess.DEVNULL,stderr=subprocess.PIPE)
names={name:2048 for name in ('releasekey','platform','shared','media','networkstack','testkey','c40.OsuLogin','c40.ServiceWifiResources')}
for module in inputs:names['com.android.art' if module=='com.android.art.release' else module]=4096
for name,bits in names.items():
    pem=args.keys/(name+'.pem');cert=args.keys/(name+'.x509.pem');pk8=args.keys/(name+'.pk8')
    if not pem.exists():
        if cert.exists() or pk8.exists():raise RuntimeError('Incomplete signing identity: '+name)
        run('openssl','genpkey','-algorithm','RSA','-pkeyopt','rsa_keygen_bits:'+str(bits),'-out',pem)
    if not cert.exists():run('openssl','req','-new','-x509','-sha256','-key',pem,'-out',cert,'-days','10000','-subj','/O=Frost local build/CN='+name)
    if not pk8.exists():run('openssl','pkcs8','-topk8','-inform','PEM','-outform','DER','-in',pem,'-out',pk8,'-nocrypt')
    if bits==4096:
        public=args.keys/(name+('.pubkey' if name=='com.android.vndk.current' else '.avbpubkey'))
        if not public.exists():run(args.avbtool,'extract_public_key','--key',pem,'--output',public)
print('Keys prepared. Keep a separate backup; do not commit this directory.')
