/* The custom-music player (runtime/hle_music_player.c) without the game: plays the music\ folder of --game, pulling
 * samples the way ipc_audio.c's feed does (music_mix on a silent game buffer), and writes what the output
 * thread would play to a 48 kHz stereo 16-bit WAV. tests/test_music.py builds and drives it.
 *
 *   musictest --game DIR --out FILE.wav --seconds S [--skip T] [--prev T] [--pause T] [--resume T]
 *             [--pick T PATH] [--tracks all|PATH] [--source T PATH] [--now T] [--rescan] [--volume N] [--shuffle 0|1] [--slow MS] [--realtime]
 *
 * T is a position in the output, in seconds. The play order is the library's (shuffle off by default). Unless
 * --realtime, the player is waited for (so the file is the same on a slow machine); --realtime paces the pulls
 * by the clock and --slow MS makes the player sleep MS after every packet: that is the underrun test.
 * The last lines on stdout: "frames N underruns N probes N". Needs FFmpeg's DLLs (SSBU_FFMPEG_DIR). */
#define _CRT_SECURE_NO_WARNINGS
static int slow_ms;
#define MUSIC_STEP_HOOK() do { if (slow_ms) Sleep(slow_ms); } while (0)
#include "../../runtime/hle_music_player.c"

const char *display_ini_path(void) { return "config/display.ini"; }
const char *rt_data_dir;
int settings_ini_get(const char *path, const char *section, const char *key, char *out, int size) { (void)path; (void)section; (void)key; (void)out; (void)size; return 0; }
void audio_set_game_music(int on) { fprintf(stderr, "[test] game music %s\n", on ? "on" : "off"); }

typedef struct { double t; char what; char arg[1100]; } Event;
static Event ev[64];
static int nev;

static void put32(FILE *f, long at, uint32_t v) { fseek(f, at, SEEK_SET); fwrite(&v, 4, 1, f); fseek(f, 0, SEEK_END); }

static void sleep_until(double secs, double t0) {
    for (;;) {
        double now = (double)GetTickCount64() / 1000.0 - t0;
        if (now >= secs) return;
        Sleep(1);
    }
}

int main(int argc, char **argv) {
    const char *game = ".", *outpath = "out.wav";
    double seconds = 5;
    int realtime = 0, rescan = 0;
    char volume[16] = "100", shuffle[4] = "0", list_src[1100] = "";
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(a, "--out") && i + 1 < argc) outpath = argv[++i];
        else if (!strcmp(a, "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(a, "--volume") && i + 1 < argc) snprintf(volume, sizeof volume, "%s", argv[++i]);
        else if (!strcmp(a, "--shuffle") && i + 1 < argc) snprintf(shuffle, sizeof shuffle, "%s", argv[++i]);
        else if (!strcmp(a, "--slow") && i + 1 < argc) slow_ms = atoi(argv[++i]);
        else if (!strcmp(a, "--realtime")) realtime = 1;
        else if (!strcmp(a, "--rescan")) rescan = 1;
        else if (!strcmp(a, "--tracks") && i + 1 < argc) snprintf(list_src, sizeof list_src, "%s", argv[++i]);
        else if (nev < 64 && (!strcmp(a, "--skip") || !strcmp(a, "--prev") || !strcmp(a, "--pause") || !strcmp(a, "--resume") ||
                              !strcmp(a, "--now") || !strcmp(a, "--pick") || !strcmp(a, "--source")) && i + 1 < argc) {
            ev[nev].what = a[2];   /* s(kip) p(rev|ause|ick) r(esume) n(ow) s(ource): told apart below */
            ev[nev].what = !strcmp(a, "--skip") ? 'n' : !strcmp(a, "--prev") ? 'p' : !strcmp(a, "--pause") ? 'z' : !strcmp(a, "--resume") ? 'r' :
                           !strcmp(a, "--now") ? '?' : !strcmp(a, "--pick") ? 'k' : 's';
            ev[nev].t = atof(argv[++i]);
            if ((ev[nev].what == 'k' || ev[nev].what == 's') && i + 1 < argc) snprintf(ev[nev].arg, sizeof ev[nev].arg, "%s", argv[++i]);
            nev++;
        } else { fprintf(stderr, "musictest: what is %s\n", a); return 2; }
    }
    FILE *wav = fopen(outpath, "wb");   /* (before the game folder is the working folder) */
    if (!wav) { fprintf(stderr, "musictest: cannot write %s\n", outpath); return 2; }
    if (!SetCurrentDirectoryA(game)) { fprintf(stderr, "musictest: no folder %s\n", game); return 2; }
    _putenv("SSBU_MUSIC=1");
    char e[64];
    snprintf(e, sizeof e, "SSBU_MUSIC_VOLUME=%s", volume); _putenv(e);
    snprintf(e, sizeof e, "SSBU_MUSIC_SHUFFLE=%s", shuffle); _putenv(e);
    static const uint8_t header[44] = {'R','I','F','F', 0,0,0,0, 'W','A','V','E', 'f','m','t',' ', 16,0,0,0, 1,0, 2,0, 0x80,0xBB,0,0, 0,0xEE,2,0, 4,0, 16,0, 'd','a','t','a', 0,0,0,0};
    fwrite(header, 1, 44, wav);

    music_player_start();
    if (!music_enabled()) { fprintf(stderr, "musictest: music would not start\n"); return 3; }
    while (music_scanning()) Sleep(5);
    if (rescan) {   /* a rescan reads every file again: how many files it opened */
        unsigned before = probes;
        music_rescan();
        Sleep(20);
        while (music_scanning()) Sleep(5);
        printf("RESCAN probes %u\n", probes - before);
    }
    if (LD(M.idle)) Sleep(250);   /* (the player looks again at a new library every 50 ms) */
    MusicFolder fl[64];
    int nf = music_folders(fl, 64);
    for (int i = 0; i < nf && i < 64; i++) printf("FOLDER %s %d %s\n", fl[i].playlist ? "playlist" : "folder", fl[i].tracks, fl[i].path);
    printf("TRACKS %d\n", music_track_count());
    if (list_src[0]) {   /* --tracks all | PATH: the picker's track list */
        static MusicTrack tl[64];
        int nt = music_source_tracks(strcmp(list_src, "all") ? list_src : "", tl, 64);
        printf("TRACKLIST %d\n", nt);
        for (int i = 0; i < nt && i < 64; i++) printf("TRACK %s|%s|%.2f|%s\n", tl[i].title, tl[i].artist, tl[i].duration, tl[i].path);
    }

    uint64_t total = (uint64_t)(seconds * RATE), done = 0;
    double t0 = (double)GetTickCount64() / 1000.0;
    int flushing = 1, next_ev = 0;   /* (music_player_start asked the player for a first track) */
    static int16_t buf[480 * 2];
    while (done < total) {
        for (int i = 0; i < nev; i++) {
            Event *x = &ev[i];
            if (!x->what || x->t * RATE > done) continue;
            char c = x->what;
            x->what = 0;
            if (c == 'n') music_next(), flushing = 1;
            else if (c == 'p') music_previous(), flushing = 1;
            else if (c == 'z') music_pause(1);
            else if (c == 'r') music_pause(0);
            else if (c == 's') music_set_source(x->arg), flushing = 1;
            else if (c == 'k') {
                char why[300];
                int ok = music_play_track(x->arg, why, sizeof why);
                printf("PICK %d %s\n", ok, ok ? "" : why);
                flushing = ok;
            } else {
                MusicNow n;
                music_now(&n);
                printf("NOW %d|%s|%s|%.2f|%.2f|%u\n", n.state, n.title, n.artist, n.position, n.duration, n.serial);
            }
        }
        (void)next_ev;
        if (flushing) {   /* the fade out is played, then the player is waited for: the next sound is new */
            while (M.gen != LD(M.req) && M.gain > 0 && done < total) {   /* (a frame at a time: no silence added) */
                memset(buf, 0, 4);
                music_mix(buf, 1, 100);
                fwrite(buf, 4, 1, wav);
                done++;
            }
            if (!realtime) {
                for (int w = 0; w < 3000 && LD(M.ack) != LD(M.req); w++) Sleep(1);
                for (int w = 0; w < 3000 && LD(M.wpos) - M.flush_to < 4800 && !LD(M.idle) && LD(M.ack) == LD(M.req); w++) Sleep(1);
            }
            flushing = 0;
        }
        uint32_t n = 480;
        if (n > total - done) n = (uint32_t)(total - done);
        if (!realtime && !LD(M.paused)) {
            for (int w = 0; w < 3000 && LD(M.wpos) - LD(M.rpos) < n && !LD(M.idle) && M.gen == LD(M.req); w++) Sleep(1);
        }
        if (realtime) sleep_until((double)done / RATE, t0);
        memset(buf, 0, n * 4);
        music_mix(buf, n, 100);
        fwrite(buf, 4, n, wav);
        done += n;
    }
    long size = ftell(wav);
    put32(wav, 4, (uint32_t)(size - 8));
    put32(wav, 40, (uint32_t)(size - 44));
    fclose(wav);
    printf("frames %llu underruns %u probes %u\n", (unsigned long long)done, M.underruns, probes);
    fflush(stdout);
    ExitProcess(0);   /* (the player and scanner threads go with it) */
}
