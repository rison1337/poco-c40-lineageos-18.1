# Building from source

`manifests/pinned.xml` pins 860 LineageOS, AOSP and PHH projects. Changes to
44 projects are recorded in the source export, including local commits and
working tree fixes. The 40 text patches were checked by reverse application
against the build tree. The other four projects contain WebView APK replacements,
downloaded separately and verified with SHA-256. Deleted binary resources are
also recorded in `source-map.json`.

Use a Linux Android 11 build environment with `repo`, Git, Python 3, OpenJDK,
the manifest's Clang/GCC toolchains and sufficient disk space. The tested build
environment used Ubuntu 20.04, with sources on an ext4 HDD and output/ccache
on an SSD.

Initialize an empty Android source directory:

```sh
repo init -u https://github.com/rison1337/poco-c40-lineageos-18.1 -b main -m manifests/pinned.xml
repo sync -c -j4
```

Clone this repository into a separate directory. From that checkout, apply the
export once to the clean, pinned Android tree:

```sh
python3 scripts/fetch-prebuilts.py /absolute/path/source-prebuilts
python3 scripts/apply.py /absolute/path/android --prebuilts /absolute/path/source-prebuilts
```

The pinned WebView APKs are in `Source-build-inputs.zip`, attached to release R1.
The download script uses `gh`; authentication with repository access is required
for private repositories. Extracted stock IMS/FM components and the patched
sensor library are included in `overlay`, separate from the open source patches,
with their hash records.

Release builds use the maintainer's persistent signing keys, which are not
included in this repository. For an independent build, generate your own keys
using the built host `avbtool`:

```sh
python3 scripts/create-signing-keys.py /absolute/path/private-keys --avbtool /absolute/path/avbtool
export C40_SRC=/absolute/path/android
export C40_KEYS=/absolute/path/private-keys
export C40_JOBS=6
bash scripts/build.sh
```

Keep a backup of your keys for subsequent updates. A build signed with new keys
cannot directly replace the published release series using the maintainer's
keys. This export reproduces the source changes; different timestamps,
toolchains, environments or certificates can produce different signed images.

The complete kernel tree, license and resolved configuration are in
[poco-c40-kernel](https://github.com/rison1337/poco-c40-kernel).
Follow its `README.lineageos.md`. Building `systemimage` does not replace the
kernel. The main boot image uses kernel 1 and the stock ramdisk; an optional
Magisk 26.3 boot image uses the same kernel.

Package a validated release bundle into a new output directory:

```sh
python3 scripts/package-release.py --bundle /absolute/path/validated-bundle --output /absolute/path/new-output
```

This creates firmware, kernel and optional Magisk archives containing images,
English instructions and checksums. See [flashing instructions](FLASHING.md)
for the manual fastboot/fastbootd commands.
