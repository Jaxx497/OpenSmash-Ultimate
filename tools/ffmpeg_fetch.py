#!/usr/bin/env python3
"""Put the FFmpeg DLLs the custom-music player loads at run time into deps/ffmpeg/bin.

    python tools/ffmpeg_fetch.py [--dev] [--tool] [--out DIR]

The four libraries (avformat-61, avcodec-61, avutil-59, swresample-5) come from the project's own audio-only
build (.github/workflows/ffmpeg-release.yml, tools/ffmpeg_build.sh), pinned by tag and SHA-256 below. While that
release does not exist yet (RELEASE_SHA256 is still the placeholder), or with --dev, they come from a pinned
BtbN FFmpeg-Builds LGPL shared build for development. Either way the download is checked against its SHA-256,
and nothing is done when the four DLLs are already there with the recorded hashes. --tool also keeps bin\ffmpeg.exe
and the build's other DLLs from the zip, for tests/test_music.py to make test files with (the game never uses it; the project's own
audio-only release has none, so --tool takes the BtbN build). Only the standard library is used.
"""
import hashlib
import io
import json
import os
import sys
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DLLS = ("avformat-61.dll", "avcodec-61.dll", "avutil-59.dll", "swresample-5.dll")
TOOL = "ffmpeg.exe"   # (--tool)

# The project's release (ffmpeg-release.yml run with this tag). PLACEHOLDERS until the release exists: then put the
# zip's SHA-256 (the release's SHA256SUMS) here.
RELEASE_REPO = "OpenSmash-Ultimate/OpenSmash-Ultimate"
RELEASE_TAG = "ffmpeg-audio-v1"
RELEASE_ASSET = "opensmash-ffmpeg-audio-7.1.5-win64.zip"
RELEASE_SHA256 = "PLACEHOLDER-release-does-not-exist-yet"

# Development fallback: BtbN's FFmpeg-Builds, FFmpeg release branch 7.1 (n7.1.5-12), LGPL, shared, win64.
DEV_URL = ("https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-07-31-14-10/"
           "ffmpeg-n7.1.5-12-g1fdbca85aa-win64-lgpl-shared-7.1.zip")
DEV_SHA256 = "0f376f96fb38554ccefb1b2ae9c7c6a7b351f0e60a372b38262c320e8392c5d0"


class Error(Exception):
    pass


def download(url):
    if not url.startswith("https://github.com/"):
        raise Error(f"not a GitHub address: {url}")
    try:
        with urllib.request.urlopen(url, timeout=120) as r:
            return r.read()
    except OSError as e:   # (Python refused the certificate: through curl.exe, Windows' own HTTPS)
        import msvc_deps
        if not msvc_deps.cert_refused(e):
            raise
        try:
            return msvc_deps.curl_get(url)
        except msvc_deps.Failed as f:
            raise Error(str(f))


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def have(out, tool=False):
    """The recorded hashes of the DLLs (and ffmpeg.exe, with tool) in out/bin match the files there."""
    try:
        with open(os.path.join(out, "installed.json"), encoding="utf-8") as f:
            rec = json.load(f)
        want = DLLS + ((TOOL,) if tool else ())
        return (rec.get("tool") or not tool) and all(d in rec["dlls"] for d in want) and \
            all(sha256(open(os.path.join(out, "bin", d), "rb").read()) == h for d, h in rec["dlls"].items())
    except (OSError, ValueError, KeyError, AttributeError):
        return False


def fetch(out=os.path.join(ROOT, "deps", "ffmpeg"), dev=False, get=download, log=print, tool=False):
    placeholder = RELEASE_SHA256.startswith("PLACEHOLDER")
    use_dev = dev or tool or placeholder
    if have(out, tool) and json.load(open(os.path.join(out, "installed.json"))).get("source") == ("dev" if use_dev else "release"):
        log(f"FFmpeg: the four DLLs are already in {os.path.join(out, 'bin')} (hashes match); nothing to do")
        return out
    if use_dev:
        url, want, source = DEV_URL, DEV_SHA256, "dev"
        why = "--dev" if dev else "--tool (only that build has ffmpeg.exe)" if tool else "the project's FFmpeg release is not published yet (placeholder in tools/ffmpeg_fetch.py)"
        log(f"FFmpeg: using the BtbN development build because of {why}")
    else:
        url = f"https://github.com/{RELEASE_REPO}/releases/download/{RELEASE_TAG}/{RELEASE_ASSET}"
        want, source = RELEASE_SHA256, "release"
    log(f"FFmpeg: downloading {url}")
    data = get(url)
    if sha256(data) != want:
        raise Error(f"the download does not match its SHA-256 ({sha256(data)} instead of {want}): nothing changed")
    bindir = os.path.join(out, "bin")
    os.makedirs(bindir, exist_ok=True)
    rec = {}
    with zipfile.ZipFile(io.BytesIO(data)) as z:
        names = z.namelist()
        for dll in DLLS:   # (BtbN: <folder>/bin/x.dll; ours: <folder>/bin/x.dll too)
            hit = [n for n in names if n.endswith("/bin/" + dll)]
            if len(hit) != 1:
                raise Error(f"the zip holds no {dll}")
            blob = z.read(hit[0])
            with open(os.path.join(bindir, dll), "wb") as f:
                f.write(blob)
            rec[dll] = sha256(blob)
        if tool:   # (the shared ffmpeg.exe needs the build's other libraries too: avfilter, swscale, ...)
            for n in names:
                base = n.split("/")[-1]
                if n.count("/") == 2 and n.split("/")[1] == "bin" and base not in rec and (base == TOOL or base.endswith(".dll")):
                    blob = z.read(n)
                    with open(os.path.join(bindir, base), "wb") as f:
                        f.write(blob)
                    rec[base] = sha256(blob)
            if TOOL not in rec:
                raise Error(f"the zip holds no {TOOL}")
        lic = [n for n in names if n.split("/")[-1] in ("COPYING.LGPLv2.1", "LICENSE.txt") and n.count("/") == 1]
        if lic:
            with open(os.path.join(out, "LICENSE-ffmpeg.txt"), "wb") as f:
                f.write(z.read(lic[0]))
    with open(os.path.join(out, "installed.json"), "w", encoding="utf-8") as f:
        json.dump({"source": source, "url": url, "zip_sha256": want, "tool": tool, "dlls": rec}, f, indent=1)
    log(f"FFmpeg: put {', '.join(rec)} into {bindir} ({source} build)")
    return out


def main(argv):
    args = argv[1:]
    dev, tool = "--dev" in args, "--tool" in args
    out = os.path.join(ROOT, "deps", "ffmpeg")
    if "--out" in args:
        i = args.index("--out")
        if i + 1 >= len(args):
            print(__doc__)
            return 2
        out = args[i + 1]
    try:
        fetch(out, dev, tool=tool)
    except (Error, OSError) as e:
        print(e)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
