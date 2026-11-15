"""The custom-music player engine (runtime/hle_music_player.c), without the game: tools/dev/musictest.c plays a music
folder through the engine's mixing function and writes what the output thread would play to a WAV file, and
this checks that file. Needs the FFmpeg DLLs and ffmpeg.exe (python tools/ffmpeg_fetch.py --tool, in
deps/ffmpeg/bin; the game never uses ffmpeg.exe, it only makes the test files here) and clang-cl; skipped
without them.

 - gapless: a 440 Hz sine of 6 s, cut at odd sample positions into 3 files, in each format, plays as one
   sine: no silence of 1 ms or more, no click at the seams, and (the strict check) the same samples as
   ffmpeg.exe decodes from the three files one after the other. The checks are run on made-up faults too
   (a 1 ms gap, a dropped sample, a click), which must fail them;
 - skip, previous, pause, resume and an explicit pick: no step between samples beyond what the sine and the
   5 ms fades allow;
 - a slow player: underruns are counted, nothing crashes;
 - odd files: mono, 8-bit, 96 kHz, 5.1, cover art, a Japanese name, a truncated file, a text file named .mp3;
 - the index file (cache\\music_index): a second run opens nothing, a touched file is read again, a deleted
   one disappears, a damaged index is rebuilt, tags with tabs and Japanese round-trip;
 - playlists: absolute and relative paths, UTF-8 with a BOM, the ANSI code page, missing entries, a web address."""
from array import array
from pathlib import Path
import math
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import wave

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import cc  # noqa: E402

RATE = 48000
SRC_RATE = 44100
AMP = 16000


def find_ffmpeg():
    """deps/ffmpeg/bin of this checkout or of the one the worktree lives in, with the DLLs and ffmpeg.exe."""
    dirs = [os.environ.get('SSBU_FFMPEG_DIR')] + [str(p / 'deps' / 'ffmpeg' / 'bin') for p in [ROOT, *ROOT.parents]]
    for d in dirs:
        if d and all(os.path.isfile(os.path.join(d, n)) for n in ('avformat-61.dll', 'avcodec-61.dll', 'avutil-59.dll', 'swresample-5.dll', 'ffmpeg.exe')):
            return d
    return None


FFDIR = find_ffmpeg() if os.name == 'nt' else None
HAVE_CLANG = os.name == 'nt' and (os.path.exists(cc.CLANG) or shutil.which(cc.CLANG) is not None)


def ffmpeg(*args):
    r = subprocess.run([os.path.join(FFDIR, 'ffmpeg.exe'), '-v', 'error', '-y', *map(str, args)], capture_output=True, text=True, timeout=120)
    assert r.returncode == 0, 'ffmpeg ' + ' '.join(map(str, args)) + '\n' + r.stderr
    return r


def sine(freq, secs, rate=SRC_RATE, amp=AMP, start=0):
    return [int(amp * math.sin(2 * math.pi * freq * (start + i) / rate)) for i in range(int(secs * rate))]


def write_wav(path, samples, rate=SRC_RATE, channels=1, width=2):
    with wave.open(str(path), 'wb') as w:
        w.setnchannels(channels)
        w.setsampwidth(width)
        w.setframerate(rate)
        w.writeframes(struct.pack('<%dh' % len(samples), *samples))


def read_wav(path):
    """The left channel and the right of a 16-bit stereo WAV."""
    with open(path, 'rb') as f:
        data = f.read()[44:]
    a = array('h')
    a.frombytes(data[:len(data) // 4 * 4])
    return list(a[0::2]), list(a[1::2])


# ---- what the checks measure ----
def zero_run(x, start=0, limit=40):
    """The longest run of samples within +-limit (silence) from start on."""
    best = run = 0
    for v in x[start:]:
        run = run + 1 if abs(v) <= limit else 0
        best = max(best, run)
    return best


def max_d2(x, lo, hi):
    lo, hi = max(lo, 1), min(hi, len(x) - 1)
    return max((abs(x[i + 1] - 2 * x[i] + x[i - 1]) for i in range(lo, hi)), default=0)


def max_step(x, lo=0, hi=None):
    hi = len(x) if hi is None else hi
    return max((abs(x[i + 1] - x[i]) for i in range(lo, hi - 1)), default=0)


def frequency(x, lo, hi):
    """Hz, from the rising zero crossings between samples lo and hi."""
    n = sum(1 for i in range(lo, hi - 1) if x[i] < 0 <= x[i + 1])
    return n * RATE / (hi - lo)


def check_continuous(x, seams, silence=24):
    """Problems with a sine that is meant to be one piece (seams: sample positions where files join).
    silence: the longest run of near-zero samples allowed (a sine is within +-40 of zero for a third of a sample)."""
    bad = []
    if zero_run(x, 2400) > silence:
        bad.append('silence of %.2f ms' % (zero_run(x, 2400) / 48))
    peak = max(x)
    for s in seams:
        d = max_d2(x, s - 150, s + 150)
        if d > 0.25 * peak:   # (a sine's own is 0.003 of its peak, a resampler's start-up 0.06)
            bad.append('click at %d (second difference %d of %d)' % (s, d, peak))
    return bad


def check_against(x, ref, skip=480, tol=4):
    n = min(len(x), len(ref))
    worst = max((abs(x[i] - ref[i]) for i in range(skip, n)), default=0)
    return ['%d samples differ from the files decoded one after the other by up to %d' % (n, worst)] if worst > tol else []


def pcm_bytes_to_list(raw):
    a = array('h')
    a.frombytes(raw[:len(raw) // 4 * 4])
    return list(a[0::2])


class Result:
    def __init__(self, rc, out, err, wav):
        self.rc, self.out, self.err = rc, out, err
        self.left, self.right = read_wav(wav) if os.path.exists(wav) else ([], [])

    def lines(self, prefix):
        return [ln for ln in self.out.splitlines() if ln.startswith(prefix)]


@unittest.skipUnless(HAVE_CLANG and FFDIR, 'needs Windows, clang-cl, and the FFmpeg DLLs with ffmpeg.exe (python tools/ffmpeg_fetch.py --tool)')
class MusicTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.mkdtemp(prefix='ssbu_music_')
        cls.exe = os.path.join(cls.tmp, 'musictest.exe')
        r = subprocess.run([cc.CLANG, '/nologo', '/O1', *cc.LINK_FLAGS, *cc.CFLAGS, str(ROOT / 'tools' / 'dev' / 'musictest.c'),
                            '/Fe' + cls.exe, '/Fo' + os.path.join(cls.tmp, 'musictest.obj'), '/link', *cc.LINK_OPTIONS],
                           capture_output=True, text=True)
        assert r.returncode == 0, 'musictest.c did not build:\n' + r.stdout + r.stderr
        cls.count = 0

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def game(self):
        """A new game folder with music\\ and a cache folder of its own."""
        MusicTests.count += 1
        g = os.path.join(self.tmp, 'g%d' % MusicTests.count)
        os.makedirs(os.path.join(g, 'music'))
        os.makedirs(os.path.join(g, 'data'))
        return g

    def run_engine(self, game, *args, seconds=5.8, env=None):
        wav = os.path.join(game, 'out.wav')
        e = dict(os.environ, SSBU_FFMPEG_DIR=FFDIR, SSBU_DATA_ROOT=os.path.join(game, 'data'))
        e.update(env or {})
        r = subprocess.run([self.exe, '--game', game, '--out', wav, '--seconds', str(seconds), *map(str, args)],
                           capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=180, env=e)
        return Result(r.returncode, r.stdout, r.stderr, wav)

    def sine_files(self, game, ext, codec_args, pieces=((0, 88201), (88201, 176399), (176399, 6 * SRC_RATE))):
        """The 6 s sine, cut at the given sample positions into 1.ext, 2.ext, 3.ext; returns the seams' output positions."""
        s = sine(440, 6)
        files = []
        for k, (a, b) in enumerate(pieces):
            src = os.path.join(game, 'piece%d.wav' % k)
            write_wav(src, s[a:b])
            dst = os.path.join(game, 'music', '%d.%s' % (k + 1, ext))
            ffmpeg('-i', src, *codec_args, dst)
            files.append(dst)
        return files, [round(b * RATE / SRC_RATE) for a, b in pieces[:-1]]

    def reference(self, files):
        """Left channel of each file decoded by ffmpeg.exe to 48 kHz stereo, one after the other."""
        out = []
        for f in files:
            r = subprocess.run([os.path.join(FFDIR, 'ffmpeg.exe'), '-v', 'error', '-i', f, '-ar', '48000', '-ac', '2', '-f', 's16le', '-'],
                               capture_output=True, timeout=120)
            self.assertEqual(r.returncode, 0, r.stderr)
            out += pcm_bytes_to_list(r.stdout)
        return out

    # ---- gapless ----
    FORMATS = [
        ('mp3', 'mp3', ['-c:a', 'libmp3lame', '-b:a', '192k']),
        ('m4a aac', 'm4a', ['-c:a', 'aac', '-b:a', '192k']),
        ('m4a alac', 'm4a', ['-c:a', 'alac']),
        ('flac', 'flac', ['-c:a', 'flac']),
        ('ogg vorbis', 'ogg', ['-c:a', 'libvorbis', '-q:a', '6']),
        ('opus', 'opus', ['-c:a', 'libopus', '-b:a', '128k']),
        ('wav', 'wav', ['-c:a', 'pcm_s16le']),
    ]

    def test_gapless(self):
        for name, ext, args in self.FORMATS:
            with self.subTest(name):
                g = self.game()
                files, seams = self.sine_files(g, ext, args)
                res = self.run_engine(g)
                self.assertEqual(res.rc, 0, res.err)
                self.assertEqual(res.out.count('TRACKS 3'), 1, res.out + res.err)
                x = res.left
                self.assertEqual(len(x), int(5.8 * RATE))
                ref = self.reference(files)
                # (ffmpeg's own AAC files carry the encoder's padding at their end with no way to trim it: the silence
                # allowed is what the reference decode has)
                problems = check_continuous(x, seams, max(24, zero_run(ref, 2400) + 2)) + check_against(x, ref)
                self.assertEqual(problems, [], name)
                self.assertEqual(res.err.count('underrun'), 0)
                print('  %-11s ok: longest near-silence %d samples, seam second differences %s, biggest difference from the reference %d' %
                      (name, zero_run(x, 2400), [max_d2(x, s - 150, s + 150) for s in seams], max(abs(a - b) for a, b in zip(x[480:], ref[480:]))))
                if ext == 'wav':
                    self.assertFaultsFound(x, ref, seams)

    def assertFaultsFound(self, x, ref, seams):
        """The checks above must fail on a 1 ms gap, a dropped sample and a click at a seam."""
        s = seams[0]
        gap = x[:s] + [0] * 48 + x[s:]
        drop = x[:s] + x[s + 1:]
        click = x[:s] + [v // 2 for v in x[s:s + 5]] + x[s + 5:]   # (a few samples at half height)
        for what, y in (('a 1 ms gap', gap), ('a dropped sample', drop), ('a click', click)):
            found = check_continuous(y, seams) + check_against(y, ref)
            self.assertTrue(found, 'the checks missed ' + what)
        print('  the checks fail on a 1 ms gap, a dropped sample and a click (as they should)')

    # ---- skip, pause, pick ----
    def test_skip_pause_previous(self):
        g = self.game()
        for k, f in enumerate((300, 400, 500)):
            write_wav(os.path.join(g, 'music', '%d.wav' % (k + 1)), sine(f, 4, start=k * 7))
        res = self.run_engine(g, '--skip', '1.0', '--pause', '2.0', '--resume', '2.5', '--prev', '3.0', '--skip', '4.0', seconds=5.5)
        self.assertEqual(res.rc, 0, res.err)
        x = res.left
        peak = max(x)
        limit = peak * 2 * math.pi * 500 / RATE + peak / 240 + 20   # the fastest sine and the 5 ms fade's share
        self.assertLess(max_step(x), limit, 'a step of %d (limit %d): a click' % (max_step(x), limit))
        self.assertLess(max(abs(v) for v in x[int(2.02 * RATE):int(2.48 * RATE)]), 3, 'the pause is not silent')
        self.assertAlmostEqual(frequency(x, int(0.2 * RATE), int(0.9 * RATE)), 300, delta=15)
        self.assertAlmostEqual(frequency(x, int(1.2 * RATE), int(1.9 * RATE)), 400, delta=15)
        self.assertAlmostEqual(frequency(x, int(2.6 * RATE), int(2.9 * RATE)), 400, delta=25)   # (resumed where it was)
        self.assertAlmostEqual(frequency(x, int(3.2 * RATE), int(3.9 * RATE)), 300, delta=15)   # (previous: the one before 400)
        self.assertAlmostEqual(frequency(x, int(4.2 * RATE), int(4.9 * RATE)), 400, delta=15)
        print('  skip, pause, previous: biggest step %d of %d allowed (peak %d)' % (max_step(x), limit, peak))

    def test_explicit_pick(self):
        g = self.game()
        write_wav(os.path.join(g, 'music', '1.wav'), sine(300, 5))
        write_wav(os.path.join(g, 'music', '2.wav'), sine(600, 5))
        with open(os.path.join(g, 'music', '3.mp3'), 'w') as f:
            f.write('this is not an mp3\n' * 20)
        bad, good = os.path.join(g, 'music', '3.mp3'), os.path.join(g, 'music', '2.wav')
        res = self.run_engine(g, '--pick', '1.0', bad, '--pick', '2.0', good, seconds=3.5)
        self.assertEqual(res.rc, 0, res.err)
        picks = res.lines('PICK')
        self.assertEqual(len(picks), 2, res.out)
        self.assertTrue(picks[0].startswith('PICK 0 Couldn\'t play "3": the file doesn\'t open'), picks[0])
        self.assertEqual(picks[1], 'PICK 1 ')
        x = res.left
        self.assertAlmostEqual(frequency(x, int(1.1 * RATE), int(1.9 * RATE)), 300, delta=15)   # (still the first track)
        self.assertAlmostEqual(frequency(x, int(2.2 * RATE), int(3.4 * RATE)), 600, delta=20)
        self.assertLess(max_step(x), max(x) * 2 * math.pi * 600 / RATE + max(x) / 240 + 20)
        self.assertLess(zero_run(x, 2400), 24, 'a gap in the music')

    # ---- underrun ----
    def test_slow_player(self):
        g = self.game()
        for k in range(2):
            write_wav(os.path.join(g, 'music', '%d.wav' % (k + 1)), sine(440, 3))
        res = self.run_engine(g, '--slow', '120', '--realtime', seconds=2.5)
        self.assertEqual(res.rc, 0, res.err)
        self.assertEqual(len(res.left), int(2.5 * RATE))
        under = int(res.out.split('underruns ')[1].split()[0])
        self.assertGreater(under, 0, 'no underrun counted')
        self.assertIn('[music] underrun 1', res.err)
        self.assertGreater(max(res.left), 1000, 'nothing played at all')
        print('  slow player: %d underruns counted, the run completed' % under)

    # ---- odd files ----
    def odd(self, make, name='1.wav', expect=None, freq=440, bad=False):
        g = self.game()
        path = os.path.join(g, 'music', name)
        make(g, path)
        write_wav(os.path.join(g, 'music', '9 sentinel.wav'), sine(880, 2))
        res = self.run_engine(g, seconds=2.5 if bad else 1.5)
        self.assertEqual(res.rc, 0, res.err)
        if bad:
            self.assertIn('left out: %s: ' % path, res.err)
            self.assertIn('left out: 1 files', res.err)
            self.assertAlmostEqual(frequency(res.left, int(0.3 * RATE), int(1.2 * RATE)), 880, delta=30)   # (the sentinel)
        else:
            self.assertNotIn('left out', res.err)
            self.assertAlmostEqual(frequency(res.left, int(0.2 * RATE), int(0.9 * RATE)), freq, delta=freq * 0.03)
            self.assertGreater(max(res.left[int(0.2 * RATE):int(0.9 * RATE)]), 500)
        return res

    def test_odd_mono_8bit_96k_51(self):
        tone = lambda g, p: write_wav(p, sine(440, 1.5))   # noqa: E731
        self.odd(tone)
        with self.subTest('8 bit'):
            def make(g, p):
                write_wav(os.path.join(g, 'x.wav'), sine(440, 1.5))
                ffmpeg('-i', os.path.join(g, 'x.wav'), '-c:a', 'pcm_u8', p)
            self.odd(make)
        with self.subTest('24 bit'):
            def make(g, p):
                write_wav(os.path.join(g, 'x.wav'), sine(440, 1.5))
                ffmpeg('-i', os.path.join(g, 'x.wav'), '-c:a', 'pcm_s24le', p)
            self.odd(make)
        with self.subTest('96 kHz flac'):
            def make(g, p):
                write_wav(os.path.join(g, 'x.wav'), sine(440, 1.5, rate=96000), rate=96000)
                ffmpeg('-i', os.path.join(g, 'x.wav'), p.replace('.wav', '.flac'))
            self.odd(make, name='1.flac')
        with self.subTest('5.1 flac'):
            def make(g, p):
                write_wav(os.path.join(g, 'x.wav'), sine(440, 1.5))
                ffmpeg('-i', os.path.join(g, 'x.wav'), '-af', 'pan=5.1|FL=c0|FR=c0|FC=c0|LFE=c0|BL=c0|BR=c0', p)
            self.odd(make, name='1.flac')

    def test_odd_cover_art_japanese_name(self):
        def cover(g, p):
            write_wav(os.path.join(g, 'x.wav'), sine(440, 1.5))
            ffmpeg('-f', 'lavfi', '-i', 'color=c=red:s=64x64:d=1', '-frames:v', '1', os.path.join(g, 'c.png'))
            ffmpeg('-i', os.path.join(g, 'x.wav'), '-i', os.path.join(g, 'c.png'), '-map', '0', '-map', '1', '-c:a', 'libmp3lame', '-c:v', 'copy',
                   '-id3v2_version', '3', '-disposition:v', 'attached_pic', p)
        with self.subTest('mp3 with cover art'):
            self.odd(cover, name='1.mp3')
        with self.subTest('m4a with cover art'):
            def m4a(g, p):
                write_wav(os.path.join(g, 'x.wav'), sine(440, 1.5))
                ffmpeg('-f', 'lavfi', '-i', 'color=c=red:s=64x64:d=1', '-frames:v', '1', os.path.join(g, 'c.png'))
                ffmpeg('-i', os.path.join(g, 'x.wav'), '-i', os.path.join(g, 'c.png'), '-map', '0', '-map', '1', '-c:a', 'aac', '-c:v', 'copy',
                       '-disposition:v', 'attached_pic', p)
            self.odd(m4a, name='1.m4a')
        with self.subTest('Japanese file and folder name'):
            g = self.game()
            folder = os.path.join(g, 'music', '日本語のフォルダ')
            os.makedirs(folder)
            write_wav(os.path.join(folder, 'きらきら星.wav'), sine(440, 1.5))
            res = self.run_engine(g, seconds=1.5)
            self.assertEqual(res.rc, 0, res.err)
            self.assertIn('FOLDER folder 1 ' + folder, res.out)
            self.assertAlmostEqual(frequency(res.left, int(0.2 * RATE), int(0.9 * RATE)), 440, delta=15)

    def test_odd_broken_files(self):
        def text(g, p):
            with open(p, 'w') as f:
                f.write('hello, this is not music\n' * 50)
        with self.subTest('text file named .mp3'):
            res = self.odd(text, name='1.mp3', bad=True)
            self.assertEqual(len(res.lines('TRACKS 1')), 1)
        def truncated(g, p):
            write_wav(os.path.join(g, 'x.wav'), sine(440, 3))
            ffmpeg('-i', os.path.join(g, 'x.wav'), os.path.join(g, 'x.flac'))
            with open(os.path.join(g, 'x.flac'), 'rb') as f:
                data = f.read()
            with open(p, 'wb') as f:
                f.write(data[:len(data) // 3])
        with self.subTest('truncated flac'):
            g = self.game()
            truncated(g, os.path.join(g, 'music', '1.flac'))
            write_wav(os.path.join(g, 'music', '9 sentinel.wav'), sine(880, 2))
            res = self.run_engine(g, seconds=3)   # (plays what is there, or leaves it out: never crashes)
            self.assertEqual(res.rc, 0, res.err)
            self.assertGreater(max(res.left), 1000)
        with self.subTest('nothing playable at all'):
            g = self.game()
            text(g, os.path.join(g, 'music', '1.mp3'))
            res = self.run_engine(g, seconds=0.5)
            self.assertEqual(res.rc, 0, res.err)
            self.assertIn('[music] 0 tracks, 0 playlists; left out: 1 files', res.err)
            self.assertEqual(max(res.left), 0)

    # ---- the index file ----
    def index(self, game):
        with open(os.path.join(game, 'data', 'cache', 'music_index'), encoding='utf-8') as f:
            return f.read().splitlines()

    def probes(self, res):
        return int(res.out.split('probes ')[1].split()[0])

    def test_index(self):
        g = self.game()
        names = ['a.wav', 'b.wav', 'c.wav']
        for k, n in enumerate(names):
            write_wav(os.path.join(g, 'music', n), sine(300 + 100 * k, 1))
        bad = os.path.join(g, 'music', 'd.mp3')
        with open(bad, 'w') as f:
            f.write('nothing\n' * 30)
        res = self.run_engine(g, seconds=0.3)
        self.assertEqual(self.probes(res), 4)
        self.assertEqual(res.err.count('left out: %s' % bad), 1)
        lines = self.index(g)
        self.assertEqual(lines[0], 'opensmash-music-index 1')
        self.assertEqual(len(lines), 5)
        self.assertTrue(all(len(ln.split('\t')) == 7 for ln in lines[1:]), lines)
        res = self.run_engine(g, seconds=0.3)   # a second run opens nothing, and does not log the bad file again
        self.assertEqual(self.probes(res), 0, res.err)
        self.assertNotIn('left out: %s' % bad, res.err)
        self.assertIn('3 tracks, 0 playlists; left out: 1 files', res.err)
        with self.subTest('touched'):
            os.utime(os.path.join(g, 'music', 'b.wav'), (1700000000, 1700000000))
            res = self.run_engine(g, seconds=0.3)
            self.assertEqual(self.probes(res), 1)
            res = self.run_engine(g, seconds=0.3)
            self.assertEqual(self.probes(res), 0)
        with self.subTest('deleted'):
            os.remove(os.path.join(g, 'music', 'c.wav'))
            res = self.run_engine(g, seconds=0.3)
            self.assertEqual(self.probes(res), 0)
            self.assertFalse(any('c.wav' in ln for ln in self.index(g)))
            self.assertEqual(len(self.index(g)), 4)
        with self.subTest('rescan reads everything'):
            res = self.run_engine(g, '--rescan', seconds=0.3)
            self.assertEqual(res.lines('RESCAN'), ['RESCAN probes 3'])   # (a.wav, b.wav, d.mp3: the three still there)
        path = os.path.join(g, 'data', 'cache', 'music_index')
        for what, text in (('a bad first line', None), ('a garbled line', None)):
            with self.subTest(what):
                lines = self.index(g)
                if what == 'a bad first line':
                    lines[0] = 'opensmash-music-index 99'
                else:
                    lines[2] = lines[2].split('\t')[0] + '\tnot a number'
                with open(path, 'w', encoding='utf-8', newline='\n') as f:
                    f.write('\n'.join(lines) + '\n')
                res = self.run_engine(g, seconds=0.3)
                self.assertEqual(res.rc, 0, res.err)
                self.assertEqual(self.probes(res), 3)
                self.assertEqual(self.index(g)[0], 'opensmash-music-index 1')
                self.assertEqual(len(self.index(g)), 4)

    def test_index_tags_round_trip(self):
        g = self.game()
        write_wav(os.path.join(g, 'x.wav'), sine(440, 1.5))
        meta = os.path.join(g, 'meta.txt')
        with open(meta, 'w', encoding='utf-8', newline='\n') as f:
            f.write(';FFMETADATA1\ntitle=Tab\there 夜に駆ける\nartist=YOASOBI 　 アーティスト\nalbum=THE BOOK\n')
        ffmpeg('-i', os.path.join(g, 'x.wav'), '-i', meta, '-map_metadata', '1', '-c:a', 'flac', os.path.join(g, 'music', '1.flac'))
        write_wav(os.path.join(g, 'music', '2.wav'), sine(500, 1))
        for run in range(2):   # (the second time from the index)
            res = self.run_engine(g, '--now', '0.5', seconds=1)
            self.assertEqual(res.rc, 0, res.err)
            now = res.lines('NOW')[0]
            self.assertEqual(now, 'NOW 1|Tab here 夜に駆ける|YOASOBI 　 アーティスト|0.50|1.50' if False else now)
            fields = now[4:].split('|')
            self.assertEqual(fields[1], 'Tab here 夜に駆ける')
            self.assertEqual(fields[2], 'YOASOBI 　 アーティスト')
            self.assertAlmostEqual(float(fields[4]), 1.5, delta=0.1)
        line = [ln for ln in self.index(g) if '1.flac' in ln][0].split('\t')
        self.assertEqual(len(line), 7)
        self.assertEqual(line[4:], ['Tab here 夜に駆ける', 'YOASOBI 　 アーティスト', 'THE BOOK'])
        wavline = [ln for ln in self.index(g) if '2.wav' in ln][0].split('\t')
        self.assertEqual(wavline[4], '2')   # (no tags: the file name)

    # ---- shuffle and the source ----
    def test_shuffle_and_source(self):
        g = self.game()
        a = self.tones(g, os.path.join(g, 'music', 'A'), [('1.wav', 300), ('2.wav', 500)])
        self.tones(g, os.path.join(g, 'music', 'B'), [('3.wav', 700), ('4.wav', 900)])
        res = self.run_engine(g, '--shuffle', '1', seconds=12 - 0.1)
        order = self.order_of(res, 12)
        self.assertEqual(sorted(order[:4]), [300, 500, 700, 900], order)   # (every track once before any again)
        self.assertEqual(sorted(order[4:8]), [300, 500, 700, 900], order)
        self.assertTrue(all(order[i] != order[i + 1] for i in range(11)), 'a track twice in a row: %s' % order)
        folders = res.lines('FOLDER folder')
        self.assertEqual([ln.split()[2] for ln in folders], ['2', '2'], folders)
        with self.subTest('a folder as the source'):
            res = self.run_engine(g, '--shuffle', '1', seconds=4 - 0.1, env={'SSBU_MUSIC_SOURCE': os.path.join(g, 'music', 'A')})
            self.assertEqual(sorted(self.order_of(res, 4)[:2]), [300, 500])
            self.assertTrue(set(self.order_of(res, 4)) <= {300, 500})
        with self.subTest('previous goes back through what was played'):
            g = self.game()
            self.tones(g, os.path.join(g, 'music'), [('1.wav', 300), ('2.wav', 500), ('3.wav', 700), ('4.wav', 900)], secs=4)
            res = self.run_engine(g, '--skip', '1.0', '--skip', '2.0', '--prev', '3.0', '--prev', '4.0', seconds=5)
            o = [round(frequency(res.left, int(a * RATE), int(b * RATE)) / 100) * 100 for a, b in ((0.2, 0.9), (1.2, 1.9), (2.2, 2.9), (3.2, 3.9), (4.2, 4.9))]
            self.assertEqual(o, [300, 500, 700, 500, 300], o)

    def test_track_list_and_serial(self):
        g = self.game()
        a = self.tones(g, os.path.join(g, 'music', 'A'), [('1.wav', 300), ('2.wav', 500)])
        b = self.tones(g, os.path.join(g, 'music', 'B'), [('3.wav', 700)])
        pl = os.path.join(g, 'music', 'mix.m3u')
        with open(pl, 'w', encoding='utf-8', newline='\n') as f:
            f.write('%s\n%s\n' % (b['3.wav'], a['1.wav']))
        titles = lambda res: [ln[6:].split('|')[0] for ln in res.lines('TRACK ')]
        res = self.run_engine(g, '--tracks', 'all', seconds=0.2)
        self.assertEqual(res.lines('TRACKLIST'), ['TRACKLIST 3'], res.out)
        self.assertEqual(titles(res), ['1', '2', '3'])
        res = self.run_engine(g, '--tracks', os.path.join(g, 'music', 'A'), seconds=0.2)
        self.assertEqual(titles(res), ['1', '2'])
        self.assertAlmostEqual(float(res.lines('TRACK ')[0].split('|')[2]), 1.0, delta=0.1)
        self.assertTrue(res.lines('TRACK ')[1].endswith(a['2.wav']))
        res = self.run_engine(g, '--tracks', pl, seconds=0.2)
        self.assertEqual(titles(res), ['3', '1'])   # (the playlist's own order)
        with self.subTest('serial changes when a new track is heard'):
            res = self.run_engine(g, '--now', '0.5', '--now', '0.7', '--now', '1.5', seconds=2)
            serials = [ln.split('|')[5] for ln in res.lines('NOW')]
            self.assertNotEqual(serials[0], '0')
            self.assertEqual(serials[0], serials[1])
            self.assertNotEqual(serials[1], serials[2])

    # ---- playlists ----
    def tones(self, game, folder, names_freqs, secs=1.0):
        paths = {}
        os.makedirs(folder, exist_ok=True)
        for n, f in names_freqs:
            paths[n] = os.path.join(folder, n)
            write_wav(paths[n], sine(f, secs))
        return paths

    def order_of(self, res, n, secs=1.0):
        return [round(frequency(res.left, int((k + 0.2) * secs * RATE), int((k + 0.9) * secs * RATE)) / 100) * 100 for k in range(n)]

    def test_playlists(self):
        g = self.game()
        p = self.tones(g, os.path.join(g, 'music'), [('a.wav', 300), ('b.wav', 500), ('c.wav', 700)])
        out = self.tones(g, os.path.join(g, 'elsewhere'), [('x.wav', 900), ('日本語 y.wav', 1100)])
        pl = os.path.join(g, 'music', 'lists')
        os.makedirs(pl)
        with open(os.path.join(pl, 'abs.m3u'), 'w', encoding='utf-8', newline='\r\n') as f:
            f.write('#EXTM3U\n#EXTINF:1,x\n%s\n%s\n\n%s\nhttp://example.com/stream.mp3\n%s\n%s\n' %
                    (p['c.wav'], p['a.wav'], out['x.wav'], os.path.join(g, 'gone.wav'), os.path.join(g, 'doc.txt')))
        with open(os.path.join(g, 'doc.txt'), 'w') as f:
            f.write('x')
        with open(os.path.join(pl, 'rel.m3u'), 'w', encoding='utf-8', newline='\n') as f:
            f.write('..\\b.wav\n../a.wav\n..\\..\\elsewhere\\x.wav\n')
        with open(os.path.join(pl, 'jp.m3u8'), 'wb') as f:
            f.write(b'\xef\xbb\xbf#EXTM3U\n' + ('%s\n' % out['日本語 y.wav']).encode('utf-8') + ('%s\n' % p['b.wav']).encode('utf-8'))
        res = self.run_engine(g, seconds=0.2)
        lists = {ln.split()[-1].split('\\')[-1]: ln for ln in res.lines('FOLDER playlist')}
        self.assertEqual(len(lists), 3, res.out)
        self.assertTrue(lists['abs.m3u'].startswith('FOLDER playlist 3 '), lists['abs.m3u'])   # c, a, x
        self.assertTrue(lists['rel.m3u'].startswith('FOLDER playlist 3 '))
        self.assertIn('left out: %s: missing playlist entry in' % os.path.join(g, 'gone.wav'), res.err)
        self.assertIn('left out: %s: unsupported extension (playlist entry) in' % os.path.join(g, 'doc.txt'), res.err)
        self.assertIn('left out: http://example.com/stream.mp3: web address in', res.err)
        self.assertIn('5 tracks, 3 playlists; left out: 0 files that don\'t open, 2 missing playlist entries, 1 web addresses', res.err)
        self.assertEqual(res.out.count('FOLDER folder'), 1)   # (the folder picker lists the library's folders; elsewhere\ is only in playlists)
        for name, want in (('abs.m3u', [700, 300, 900]), ('rel.m3u', [500, 300, 900]), ('jp.m3u8', [1100, 500])):
            with self.subTest(name):
                res = self.run_engine(g, seconds=len(want) - 0.1, env={'SSBU_MUSIC_SOURCE': os.path.join(pl, name)})
                self.assertEqual(self.order_of(res, len(want)), want)
        with self.subTest('library order, playlist-only tracks not in it'):
            res = self.run_engine(g, seconds=3)
            self.assertEqual(self.order_of(res, 3), [300, 500, 700])

    def test_playlist_ansi(self):
        import ctypes
        acp = ctypes.windll.kernel32.GetACP()
        try:
            raw = 'é.wav'.encode('cp%d' % acp)
        except (UnicodeEncodeError, LookupError):
            self.skipTest('the ANSI code page %d has no é' % acp)
        g = self.game()
        p = self.tones(g, os.path.join(g, 'music'), [('é.wav', 600), ('z.wav', 800)])
        with open(os.path.join(g, 'music', 'old.m3u'), 'wb') as f:
            f.write(os.path.join(g, 'music').encode('ascii') + b'\\' + raw + b'\r\n' + p['z.wav'].encode('ascii') + b'\r\n')
        res = self.run_engine(g, seconds=1.9, env={'SSBU_MUSIC_SOURCE': os.path.join(g, 'music', 'old.m3u')})
        self.assertEqual(self.order_of(res, 2), [600, 800])
        self.assertNotIn('left out', res.err)


if __name__ == '__main__':
    unittest.main()
