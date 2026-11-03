#!/usr/bin/env python3
"""Package validated images with English manual fastboot instructions."""
from pathlib import Path
import argparse
import hashlib
import json
import os
import zipfile


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--bundle', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[1]
    source, out = args.bundle.resolve(), args.output.resolve()
    manifest = json.loads((source / 'release.json').read_text())
    revision = manifest['release_id']
    assert revision.startswith('R') and revision[1:].isdigit()
    assert manifest.get('data_preserving_update_tested_on_' + revision.lower())
    specs = [
        ('firmware', 'LineageOS-' + manifest['lineage'] + '-' + revision + '-POCO-C40',
         ['system.img', 'boot.img', 'vbmeta.img'], 'FLASHING.md'),
        ('kernel', 'Kernel' + str(manifest['kernel_revision']) + '-POCO-C40',
         ['boot.img'], 'KERNEL.md'),
        ('magisk', 'Magisk-' + manifest['magisk_version'] + '-POCO-C40',
         ['boot.img', 'boot-magisk.img', 'Magisk.v26.3.apk'], 'MAGISK.md'),
    ]
    expected = {row['name']: row for row in manifest['files'].values()}
    expected.update({row['name']: row for row in manifest['optional_root'].values()
                     if isinstance(row, dict) and 'name' in row})
    for name in {name for _, _, images, _ in specs for name in images}:
        path = source / name
        assert path.is_file() and path.stat().st_size == expected[name]['bytes']
        assert digest(path) == expected[name]['sha256'], name
    assert all(not (out / 'stage' / name).exists() and not (out / (name + '.zip')).exists()
               for _, name, _, _ in specs), 'Use a new output directory'
    out.mkdir(parents=True, exist_ok=True)
    reports = []
    for kind, name, images, guide in specs:
        stage = out / 'stage' / name
        stage.mkdir(parents=True)
        for image in images:
            os.link(source / image, stage / image)
        text = (repo / 'docs' / guide).read_text()
        assert not any('\u0400' <= char <= '\u04ff' for char in text)
        if kind == 'firmware':
            text = text.replace('(KERNEL.md)', '(https://github.com/rison1337/poco-c40-lineageos-18.1/blob/main/docs/KERNEL.md)')
            text = text.replace('(MAGISK.md)', '(https://github.com/rison1337/poco-c40-lineageos-18.1/blob/main/docs/MAGISK.md)')
        (stage / 'README.md').write_text(text, encoding='utf-8')
        sums = [digest(path) + '  ' + path.name for path in sorted(stage.iterdir())]
        (stage / 'SHA256SUMS').write_text('\n'.join(sums) + '\n')
        archive = out / (name + '.zip')
        temporary = archive.with_suffix('.part')
        with zipfile.ZipFile(temporary, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=4) as z:
            for path in sorted(stage.iterdir()):
                z.write(path, (Path(name) / path.name).as_posix())
        with zipfile.ZipFile(temporary) as z:
            assert z.testzip() is None
        temporary.replace(archive)
        reports.append(dict(kind=kind, path=str(archive), bytes=archive.stat().st_size,
                            sha256=digest(archive), stage=str(stage),
                            archive_format='manual-fastboot-images-v1'))
        print(json.dumps(dict(packaged=kind, bytes=archive.stat().st_size)), flush=True)
    (out / 'packages.json').write_text(json.dumps(dict(revision=revision, packages=reports), indent=2) + '\n')
    (out / 'SHA256SUMS').write_text(''.join(row['sha256'] + '  ' + Path(row['path']).name + '\n' for row in reports))


if __name__ == '__main__':
    main()
