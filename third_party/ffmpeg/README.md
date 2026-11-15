# FFmpeg headers

The public headers of libavformat, libavcodec, libavutil and libswresample, FFmpeg **7.1.5**
(tag `n7.1.5`), for compiling the custom-music player. No library is linked: the runtime loads the DLLs
(avformat-61, avcodec-61, avutil-59, swresample-5) at run time. `docs/CUSTOM-MUSIC-PLAN.md`, "Getting
FFmpeg", says where the DLLs come from.

## How they were taken

From the official source tarball https://ffmpeg.org/releases/ffmpeg-7.1.5.tar.xz
(SHA-256 `de668509caf9e35e3cd162473441fdb29538c6d96ed080292b3cf9e6fc5d558f`; its detached signature
`ffmpeg-7.1.5.tar.xz.asc` verified with the FFmpeg release signing key, fingerprint
`FCF9 86EA 15E6 E293 A564  4F10 B432 2F04 D676 58D8`). Then, in the extracted tree:

```sh
./configure --prefix=/tmp/inst --disable-everything --disable-programs --disable-doc --disable-x86asm --disable-autodetect
make install-headers
```

and the `include/libavformat`, `libavcodec`, `libavutil` and `libswresample` folders were copied here
unchanged (that includes the generated `libavutil/avconfig.h` and `libavutil/ffversion.h`; the
`ffversion.h` says `7.1.5`). `avconfig.h` only holds `AV_HAVE_BIGENDIAN` and `AV_HAVE_FAST_UNALIGNED`, the
same for every x86-64 build. Compared with the headers in BtbN's `n7.1.5-12-g1fdbca85aa` development build
(`tools/ffmpeg_fetch.py --dev`), only `ffversion.h` differs (the git version string), so that build's DLLs fit
these headers.

`COPYING.LGPLv2.1` is the license text from the same tarball. FFmpeg is LGPL-2.1-or-later as built here
(`tools/ffmpeg_build.sh`: no GPL, version3 or non-free options, no external libraries).
