/* The custom-music player (music.h, docs/CUSTOM-MUSIC-PLAN.md): the player's own files, decoded with
 * FFmpeg and mixed into the game's audio output.
 *
 * FFmpeg is loaded at run time (the four DLLs / shared libraries, major versions checked), so the game
 * builds and runs without it. Three threads: the scanner (lists the library folders, reads what the index
 * file cache\music_index does not know), the player (decodes ahead into a ring of 48 kHz stereo floats,
 * the next track already open when the current one ends: gapless) and the output thread, which calls
 * music_mix and only reads the ring. Skip, pause, stop and volume ramp over 5 ms where the samples are
 * mixed, so there are no clicks.
 *
 * Skip: the control thread queues a command and bumps `req`. The mixer fades out and waits; the player
 * closes its tracks, notes where its write position is (flush_to) and sets `ack`. The mixer then drops
 * everything before flush_to and fades the new track in. Nothing is locked on the mixer's side. */
#define _CRT_SECURE_NO_WARNINGS
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>
#ifndef _WIN32
#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/channel_layout.h>
#include <libavutil/dict.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>

#include "audio.h"
#include "music.h"
#include "settings.h"

const char *display_ini_path(void);   /* (display.h) */
extern const char *rt_data_dir;       /* (host.c) native-port/<version>/..., the cache folder is two levels up */

#define RATE 48000
#define RING_FRAMES (1u << 18)   /* 5.5 s */
#define RAMP (RATE / 200)        /* frames: 5 ms */
#define NENT 64
#define INDEX_HEAD "opensmash-music-index 1"
#ifdef _WIN32
#define SEP '\\'
#define PATHCMP _stricmp
#define PATHNCMP _strnicmp
#else
#define SEP '/'
#define PATHCMP strcmp
#define PATHNCMP strncmp
#endif
#ifndef MUSIC_STEP_HOOK
#define MUSIC_STEP_HOOK()   /* (musictest.c slows the player here) */
#endif

#define LD(p) __atomic_load_n(&(p), __ATOMIC_ACQUIRE)
#define ST(p, v) __atomic_store_n(&(p), (v), __ATOMIC_RELEASE)

/* ---- FFmpeg, through a table of only what is used ------------------------------------------ */
#define FF_FUNCS(X) \
    X(0, avutil_version) X(0, av_frame_alloc) X(0, av_frame_free) X(0, av_frame_unref) X(0, av_dict_get) \
    X(0, av_channel_layout_default) X(0, av_channel_layout_uninit) X(0, av_channel_layout_copy) \
    X(0, av_opt_set_double) X(0, av_log_set_level) X(0, av_strerror) \
    X(1, swresample_version) X(1, swr_alloc_set_opts2) X(1, swr_init) X(1, swr_free) X(1, swr_convert) \
    X(1, swr_get_out_samples) \
    X(2, avcodec_version) X(2, avcodec_alloc_context3) X(2, avcodec_free_context) \
    X(2, avcodec_parameters_to_context) X(2, avcodec_open2) X(2, avcodec_send_packet) \
    X(2, avcodec_receive_frame) X(2, av_packet_alloc) X(2, av_packet_free) X(2, av_packet_unref) \
    X(3, avformat_version) X(3, avformat_open_input) X(3, avformat_find_stream_info) \
    X(3, avformat_close_input) X(3, av_find_best_stream) X(3, av_read_frame)
static struct {
#define X(l, f) __typeof__(f) *f;
    FF_FUNCS(X)
#undef X
} ff;

static CRITICAL_SECTION L;   /* the command queue, the library, the source, the now-playing entries */
static int ff_state;         /* 0 not tried, 1 loaded, -1 missing (L held) */

#ifdef _WIN32
typedef HMODULE Lib;
static void *sym(Lib l, const char *n) { return (void *)GetProcAddress(l, n); }
#else
typedef void *Lib;
static void *sym(Lib l, const char *n) { return dlsym(l, n); }
#endif

static int ff_bind(Lib *lib) {
    static const int want[4] = {59, 5, 61, 61};   /* avutil, swresample, avcodec, avformat */
    int bad = 0;
#define X(l, f) { void *s = sym(lib[l], #f); if (!s) { fprintf(stderr, "[music] FFmpeg has no %s\n", #f); bad = 1; } *(void **)&ff.f = s; }
    FF_FUNCS(X)
#undef X
    if (bad) return 0;
    int have[4] = {ff.avutil_version() >> 16, ff.swresample_version() >> 16, ff.avcodec_version() >> 16, ff.avformat_version() >> 16};
    for (int i = 0; i < 4; i++)
        if (have[i] != want[i]) {
            fprintf(stderr, "[music] FFmpeg library %d has major version %d, not %d\n", i, have[i], want[i]);
            return 0;
        }
    return 1;
}

/* Loads the four libraries from one folder (NULL: the system's search, off Windows), in dependency order. */
#ifdef _WIN32
static int load_from(const wchar_t *dir) {
    static const wchar_t *name[4] = {L"avutil-59.dll", L"swresample-5.dll", L"avcodec-61.dll", L"avformat-61.dll"};
    wchar_t path[4][MAX_PATH + 32];
    for (int i = 0; i < 4; i++) {
        _snwprintf(path[i], MAX_PATH + 32, L"%ls\\%ls", dir, name[i]);
        if (GetFileAttributesW(path[i]) == INVALID_FILE_ATTRIBUTES) return 0;
    }
    Lib lib[4];
    for (int i = 0; i < 4; i++)   /* (the altered search path: a library's imports of the others are found beside it) */
        if (!(lib[i] = LoadLibraryExW(path[i], NULL, LOAD_WITH_ALTERED_SEARCH_PATH))) {
            fprintf(stderr, "[music] %ls would not load (error %lu)\n", path[i], (unsigned long)GetLastError());
            return 0;
        }
    return ff_bind(lib);
}
#else
static int load_from(const char *dir) {
#ifdef __APPLE__
    static const char *name[4] = {"libavutil.59.dylib", "libswresample.5.dylib", "libavcodec.61.dylib", "libavformat.61.dylib"};
#else
    static const char *name[4] = {"libavutil.so.59", "libswresample.so.5", "libavcodec.so.61", "libavformat.so.61"};
#endif
    Lib lib[4];
    for (int i = 0; i < 4; i++) {
        char path[1100];
        snprintf(path, sizeof path, dir ? "%s/%s" : "%.0s%s", dir ? dir : "", name[i]);
        if (!(lib[i] = dlopen(path, RTLD_NOW | RTLD_LOCAL))) return 0;
    }
    return ff_bind(lib);
}
#endif

/* (L held) Looks in SSBU_FFMPEG_DIR, <game folder>\deps\ffmpeg\bin, then beside the exe. */
static int load_ffmpeg(void) {
    if (ff_state) return ff_state > 0;
    const char *env = getenv("SSBU_FFMPEG_DIR");
    int ok = 0;
#ifdef _WIN32
    wchar_t dir[MAX_PATH + 32];
    if (!ok && env && *env && MultiByteToWideChar(CP_UTF8, 0, env, -1, dir, MAX_PATH)) ok = load_from(dir);
    if (!ok && GetCurrentDirectoryW(MAX_PATH, dir)) { wcscat(dir, L"\\deps\\ffmpeg\\bin"); ok = load_from(dir); }
    if (!ok && GetModuleFileNameW(NULL, dir, MAX_PATH)) { wchar_t *s = wcsrchr(dir, L'\\'); if (s) { *s = 0; ok = load_from(dir); } }
#else
    if (env && *env) ok = load_from(env);
    if (!ok) ok = load_from(NULL);
#endif
    ff_state = ok ? 1 : -1;
    if (ok) ff.av_log_set_level(AV_LOG_FATAL);   /* (what fails is logged here, once) */
    else fprintf(stderr, "[music] FFmpeg is not available (deps\\ffmpeg\\bin\\avformat-61.dll and the others): custom music is off\n");
    return ok;
}

/* ---- the library: tracks, folders, playlists ------------------------------------------------ */
typedef struct {
    char *path, *title, *artist, *album;   /* utf-8; never NULL once an item is in a library */
    uint64_t size;
    int64_t mtime;                         /* seconds */
    double duration;                       /* seconds, 0 when unknown */
    int folder;                            /* into lib.folder; -1 for a track only a playlist names */
} Item;
typedef struct { char *path; int count; } Folder;
typedef struct { char *path, *name; int *item, n; } Playlist;
typedef struct {
    Item *item; int n;
    Folder *folder; int nfolder;
    Playlist *list; int nlist;
    uint32_t gen;
} Library;
static Library lib;   /* (L) */
static char source[1024];   /* (L) a folder or a playlist file; "" = every track in a folder */
static unsigned probes;   /* files opened to read their tags */

static void item_free(Item *it) { free(it->path); free(it->title); free(it->artist); free(it->album); }
static void items_free(Item *v, int n) { for (int i = 0; i < n; i++) item_free(&v[i]); free(v); }
static void lists_free(Playlist *v, int n) { for (int i = 0; i < n; i++) { free(v[i].path); free(v[i].name); free(v[i].item); } free(v); }

static void norm(char *p) {
#ifdef _WIN32
    for (; *p; p++) if (*p == '/') *p = '\\';
#else
    (void)p;
#endif
}
static void strip_sep(char *p) { size_t n = strlen(p); while (n > 1 && (p[n - 1] == '\\' || p[n - 1] == '/')) p[--n] = 0; }

/* whether path is inside folder (a prefix at a separator) */
static int under(const char *path, const char *folder) {
    size_t n = strlen(folder);
    return n && !PATHNCMP(path, folder, n) && (path[n] == '\\' || path[n] == '/');
}

static const char *base_name(const char *p) {
    const char *s = p, *q;
    for (q = p; *q; q++) if (*q == '\\' || *q == '/') s = q + 1;
    return s;
}
/* the file's name without its extension */
static char *stem(const char *p) {
    char *s = strdup(base_name(p)), *dot = strrchr(s, '.');
    if (dot && dot != s) *dot = 0;
    return s;
}

static int music_ext(const char *name) {
    static const char *const ext[] = {"mp3", "m4a", "mp4", "aac", "flac", "wav", "ogg", "oga", "opus"};
    const char *dot = strrchr(name, '.');
    if (!dot) return 0;
    for (size_t i = 0; i < sizeof ext / sizeof *ext; i++) if (!_stricmp(dot + 1, ext[i])) return 1;
    return 0;
}
static int playlist_ext(const char *name) {
    const char *dot = strrchr(name, '.');
    return dot && (!_stricmp(dot, ".m3u") || !_stricmp(dot, ".m3u8"));
}

static int cmp_item(const void *a, const void *b) {
    const Item *x = a, *y = b;
    int c = PATHCMP(x->path, y->path);
    return c ? c : (y->folder >= 0) - (x->folder >= 0);   /* (a library track before the same file named by a playlist) */
}

static int cmp_path(const void *a, const void *b) { return PATHCMP(((const Item *)a)->path, ((const Item *)b)->path); }

/* ---- finding files --------------------------------------------------------------------------- */
typedef struct { Item *v; int n, cap; char **list; int nlist, caplist; } Found;   /* tracks; playlist files */

static void found_item(Found *f, const char *path, uint64_t size, int64_t mtime, int listed) {
    if (strpbrk(path, "\t\r\n")) return;   /* (the index file could not hold it) */
    if (f->n == f->cap) f->v = realloc(f->v, (f->cap = f->cap ? f->cap * 2 : 1024) * sizeof *f->v);
    Item *it = &f->v[f->n++];
    memset(it, 0, sizeof *it);
    it->path = strdup(path);
    it->title = stem(path);   /* (until the tags are read) */
    it->artist = strdup("");
    it->album = strdup("");
    it->size = size;
    it->mtime = mtime;
    it->folder = listed ? 0 : -1;
}
static void found_list(Found *f, const char *path) {
    if (f->nlist == f->caplist) f->list = realloc(f->list, (f->caplist = f->caplist ? f->caplist * 2 : 16) * sizeof *f->list);
    f->list[f->nlist++] = strdup(path);
}

/* size and modified time of a file; 0 when it is not one */
static int file_info(const char *path, uint64_t *size, int64_t *mtime) {
#ifdef _WIN32
    wchar_t w[1100];
    WIN32_FILE_ATTRIBUTE_DATA d;
    if (!MultiByteToWideChar(CP_UTF8, 0, path, -1, w, 1100) || !GetFileAttributesExW(w, GetFileExInfoStandard, &d) ||
        (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return 0;
    *size = (uint64_t)d.nFileSizeHigh << 32 | d.nFileSizeLow;
    *mtime = (int64_t)(((uint64_t)d.ftLastWriteTime.dwHighDateTime << 32 | d.ftLastWriteTime.dwLowDateTime) / 10000000ull) - 11644473600ll;
#else
    struct stat st;
    if (stat(path, &st) || !S_ISREG(st.st_mode)) return 0;
    *size = (uint64_t)st.st_size;
    *mtime = (int64_t)st.st_mtime;
#endif
    return 1;
}

/* Every music file and playlist file under dir, subfolders included. */
static void walk(Found *f, const char *dir, int depth) {
    char full[1100];
#ifdef _WIN32
    wchar_t w[1100];
    snprintf(full, sizeof full, "%s\\*", dir);
    if (!MultiByteToWideChar(CP_UTF8, 0, full, -1, w, 1100)) return;
    WIN32_FIND_DATAW d;
    HANDLE h = FindFirstFileW(w, &d);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        char name[520];
        if (!WideCharToMultiByte(CP_UTF8, 0, d.cFileName, -1, name, sizeof name, NULL, NULL) || !strcmp(name, ".") || !strcmp(name, "..")) continue;
        if (snprintf(full, sizeof full, "%s\\%s", dir, name) >= (int)sizeof full) continue;
        if (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!(d.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && depth < 32) walk(f, full, depth + 1);
        } else if (music_ext(name)) {
            found_item(f, full, (uint64_t)d.nFileSizeHigh << 32 | d.nFileSizeLow,
                       (int64_t)(((uint64_t)d.ftLastWriteTime.dwHighDateTime << 32 | d.ftLastWriteTime.dwLowDateTime) / 10000000ull) - 11644473600ll, 1);
        } else if (playlist_ext(name)) found_list(f, full);
    } while (FindNextFileW(h, &d));
    FindClose(h);
#else
    DIR *dp = opendir(dir);
    if (!dp) return;
    struct dirent *e;
    while ((e = readdir(dp))) {
        struct stat st;
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..") || snprintf(full, sizeof full, "%s/%s", dir, e->d_name) >= (int)sizeof full || stat(full, &st)) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < 32) walk(f, full, depth + 1); }
        else if (music_ext(e->d_name)) found_item(f, full, (uint64_t)st.st_size, (int64_t)st.st_mtime, 1);
        else if (playlist_ext(e->d_name)) found_list(f, full);
    }
    closedir(dp);
#endif
}

static void absolute(const char *p, char *out, size_t n) {
#ifdef _WIN32
    wchar_t w[1100], a[1100];
    if (MultiByteToWideChar(CP_UTF8, 0, p, -1, w, 1100) && GetFullPathNameW(w, 1100, a, NULL) && WideCharToMultiByte(CP_UTF8, 0, a, -1, out, (int)n, NULL, NULL)) return;
#endif
    snprintf(out, n, "%s", p);
}

/* ---- music.ini: [libraries] and [playlists], one path per line --------------------------------- */
static char **ini_section(const char *section, int *count) {
    char path[1100], line[1100], **v = NULL;
    snprintf(path, sizeof path, "%s", display_ini_path());
    char *s = strrchr(path, '/'), *b = strrchr(path, '\\');
    s = b > s ? b : s;
    snprintf(s ? s + 1 : path, path + sizeof path - (s ? s + 1 : path), "music.ini");
    FILE *f = fopen(path, "rb");
    int in = 0, n = 0;
    while (f && fgets(line, sizeof line, f)) {
        char *p = line;
        if (!n && !in && !memcmp(p, "\xEF\xBB\xBF", 3)) p += 3;
        while (*p == ' ' || *p == '\t') p++;
        size_t len = strlen(p);
        while (len && strchr(" \t\r\n", p[len - 1])) p[--len] = 0;
        if (!*p || *p == ';' || *p == '#') continue;
        if (*p == '[') { in = !_stricmp(p, section); continue; }
        if (!in) continue;
        norm(p);
        strip_sep(p);
        v = realloc(v, (n + 1) * sizeof *v);
        v[n++] = strdup(p);
    }
    if (f) fclose(f);
    *count = n;
    return v;
}
static void strings_free(char **v, int n) { for (int i = 0; i < n; i++) free(v[i]); free(v); }

/* ---- playlists --------------------------------------------------------------------------------- */
static int valid_utf8(const unsigned char *s) {
    while (*s) {
        int k = *s < 0x80 ? 0 : *s >= 0xC2 && *s <= 0xDF ? 1 : (*s & 0xF0) == 0xE0 ? 2 : *s >= 0xF0 && *s <= 0xF4 ? 3 : -1;
        if (k < 0) return 0;
        s++;
        while (k--) if ((*s++ & 0xC0) != 0x80) return 0;
    }
    return 1;
}

typedef struct { char **path; int n, cap; } Names;

/* The tracks a playlist file names, in its order: `names` gets their full paths. Entries whose file does
 * not exist or is not music, and web addresses, are logged and counted (the scan's summary line). */
static void playlist_read(const char *file, Found *f, Names *names, int *web_total, int *missing_total) {
    FILE *fp = fopen(file, "rb");
    if (!fp) return;
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size < 0 || size > 64 << 20) { fclose(fp); return; }
    unsigned char *data = malloc(size + 1);
    size_t got = fread(data, 1, size, fp);
    fclose(fp);
    data[got] = 0;
    unsigned char *text = data;
    if (got >= 3 && !memcmp(text, "\xEF\xBB\xBF", 3)) text += 3;
#ifdef _WIN32
    char *ansi = NULL;
    if (!valid_utf8(text)) {   /* an old .m3u: the Windows code page */
        wchar_t *w = malloc((strlen((char *)text) + 1) * sizeof *w);
        ansi = malloc(strlen((char *)text) * 4 + 4);
        if (MultiByteToWideChar(CP_ACP, 0, (char *)text, -1, w, (int)strlen((char *)text) + 1) &&
            WideCharToMultiByte(CP_UTF8, 0, w, -1, ansi, (int)strlen((char *)text) * 4 + 4, NULL, NULL)) text = (unsigned char *)ansi;
        free(w);
    }
#endif
    char dir[1100];
    snprintf(dir, sizeof dir, "%s", file);
    char *cut = (char *)base_name(dir);
    if (cut > dir) cut[-1] = 0; else strcpy(dir, ".");
    int web = 0, missing = 0, ok = 0;
    for (char *line = (char *)text, *next; line && *line; line = next) {
        next = strchr(line, '\n');
        if (next) *next++ = 0;
        size_t len = strlen(line);
        while (len && strchr(" \t\r", line[len - 1])) line[--len] = 0;
        while (*line == ' ' || *line == '\t') line++;
        if (!*line || *line == '#') continue;
        if (strstr(line, "://")) {
            web++;
            fprintf(stderr, "[music] left out: %s: web address in %s\n", line, file);
            continue;
        }
        char full[1100], entry[1100];
        snprintf(entry, sizeof entry, "%s", line);
        norm(entry);
#ifdef _WIN32
        int abs = (entry[0] && entry[1] == ':') || entry[0] == '\\';
#else
        int abs = entry[0] == '/';
#endif
        if (abs) snprintf(full, sizeof full, "%s", entry);
        else snprintf(full, sizeof full, "%s%c%s", dir, SEP, entry);
        uint64_t sz;
        int64_t mt;
        absolute(full, entry, sizeof entry);   /* (..\ folded away: the same file as the library's spelling) */
        norm(entry);
        snprintf(full, sizeof full, "%s", entry);
        if (!music_ext(full) || !file_info(full, &sz, &mt) || strpbrk(full, "\t\r\n")) {
            missing++;
            fprintf(stderr, "[music] left out: %s: %s in %s\n", full, music_ext(full) ? "missing playlist entry" : "unsupported extension (playlist entry)", file);
            continue;
        }
        if (names->n == names->cap) names->path = realloc(names->path, (names->cap = names->cap ? names->cap * 2 : 64) * sizeof *names->path);
        names->path[names->n++] = strdup(full);
        found_item(f, full, sz, mt, 0);   /* (the same file found in a library goes in front of this one: cmp_item) */
        ok++;
    }
    *web_total += web;
    *missing_total += missing;
    (void)ok;
#ifdef _WIN32
    free(ansi);
#endif
    free(data);
}

/* ---- reading tags (the scan, once per new or changed file) ------------------------------------ */
static void clean(char *s) {   /* tabs and line ends become spaces; at most 255 bytes, not cut inside a character */
    for (char *p = s; *p; p++) if (*p == '\t' || *p == '\r' || *p == '\n') *p = ' ';
    size_t n = strlen(s);
    if (n > 255) { n = 255; while (n && (s[n] & 0xC0) == 0x80) n--; s[n] = 0; }
}
static void set_str(char **dst, const char *v) {
    if (!v || !*v) return;
    free(*dst);
    *dst = strdup(v);
    clean(*dst);
}
static const char *tag(AVFormatContext *f, int stream, const char *key) {
    AVDictionaryEntry *e = ff.av_dict_get(f->metadata, key, NULL, 0);
    if (!e && stream >= 0) e = ff.av_dict_get(f->streams[stream]->metadata, key, NULL, 0);
    return e ? e->value : NULL;
}

/* Opens the file for its length and tags; nothing is decoded. A file that will not open keeps its
 * file name as the title and -1 as the length: the index remembers it (it is not opened at every start
 * or logged again), the library leaves it out. */
static void probe(Item *it) {
    AVFormatContext *f = NULL;
    __atomic_add_fetch(&probes, 1, __ATOMIC_RELAXED);
    it->duration = -1;   /* (until it opens: the index keeps the file, the library does not) */
    if (ff.avformat_open_input(&f, it->path, NULL, NULL) < 0) {
        fprintf(stderr, "[music] left out: %s: doesn't open\n", it->path);
        return;
    }
    if (ff.avformat_find_stream_info(f, NULL) < 0 || ff.av_find_best_stream(f, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0) < 0)
        fprintf(stderr, "[music] left out: %s: no audio stream\n", it->path);
    else {
        it->duration = 0;
        int st = ff.av_find_best_stream(f, AVMEDIA_TYPE_AUDIO, -1, -1, NULL, 0);
        if (f->duration > 0) it->duration = f->duration / (double)AV_TIME_BASE;
        set_str(&it->title, tag(f, st, "title"));
        set_str(&it->artist, tag(f, st, "artist"));
        set_str(&it->album, tag(f, st, "album"));
    }
    ff.avformat_close_input(&f);
}

/* ---- the index file: cache\music_index -------------------------------------------------------- */
static void index_path(char *out, size_t n) {
    const char *root = getenv("SSBU_DATA_ROOT");
    if (root) snprintf(out, n, "%s/cache", root);
    else if (rt_data_dir) snprintf(out, n, "%s/../../cache", rt_data_dir);
    else snprintf(out, n, "cache");
#ifdef _WIN32
    CreateDirectoryA(out, NULL);
#else
    mkdir(out, 0777);
#endif
    size_t k = strlen(out);
    snprintf(out + k, n - k, "/music_index");
}

/* The index, sorted by path; any malformed line makes it empty (it is rebuilt, never an error). */
static Item *index_read(int *count) {
    char path[1100];
    index_path(path, sizeof path);
    *count = 0;
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    char *data = size > 0 && size < (1l << 30) ? malloc(size + 1) : NULL;
    if (!data || fread(data, 1, size, fp) != (size_t)size) { free(data); fclose(fp); return NULL; }
    fclose(fp);
    data[size] = 0;
    Item *v = NULL;
    int n = 0, cap = 0, bad = 0;
    char *line = data, *next;
    for (int first = 1; line < data + size && !bad; line = next, first = 0) {
        next = strchr(line, '\n');
        if (next) *next++ = 0; else next = data + size;
        size_t len = strlen(line);
        if (len && line[len - 1] == '\r') line[--len] = 0;
        if (first) { bad = strcmp(line, INDEX_HEAD) != 0; continue; }
        char *f[7];
        int k = 0;
        f[k++] = line;
        for (char *t = line; (t = strchr(t, '\t')) && k < 8; ) { *t++ = 0; if (k < 7) f[k] = t; k++; }
        char *e1, *e2, *e3;
        if (k != 7) { bad = 1; break; }
        Item it = {0};
        it.size = strtoull(f[1], &e1, 10);
        it.mtime = strtoll(f[2], &e2, 10);
        it.duration = strtod(f[3], &e3);
        if (!*f[0] || !*f[1] || !*f[2] || !*f[3] || *e1 || *e2 || *e3) { bad = 1; break; }
        it.path = strdup(f[0]); it.title = strdup(f[4]); it.artist = strdup(f[5]); it.album = strdup(f[6]);
        it.folder = 0;
        if (n == cap) v = realloc(v, (cap = cap ? cap * 2 : 1024) * sizeof *v);
        v[n++] = it;
    }
    free(data);
    if (bad) {
        items_free(v, n);
        return NULL;
    }
    qsort(v, n, sizeof *v, cmp_item);
    *count = n;
    return v;
}

static void index_write(const Item *v, int n) {
    char path[1100], tmp[1120];
    index_path(path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *fp = fopen(tmp, "wb");
    if (!fp) return;
    fprintf(fp, "%s\n", INDEX_HEAD);
    for (int i = 0; i < n; i++)
        fprintf(fp, "%s\t%llu\t%lld\t%.1f\t%s\t%s\t%s\n", v[i].path, (unsigned long long)v[i].size, (long long)v[i].mtime, v[i].duration, v[i].title, v[i].artist, v[i].album);
    int ok = !ferror(fp);
    ok = fclose(fp) == 0 && ok;
#ifdef _WIN32
    ok = ok && MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
#else
    ok = ok && rename(tmp, path) == 0;
#endif
    if (!ok) remove(tmp);   /* (the index is only a cache) */
}

/* ---- the scan ---------------------------------------------------------------------------------- */
static Item item_copy(const Item *s) {
    Item it = *s;
    it.path = strdup(s->path); it.title = strdup(s->title); it.artist = strdup(s->artist); it.album = strdup(s->album);
    return it;
}

static char **folder_key;   /* (the scanner's, one at a time) */
static int cmp_idx_folder(const void *a, const void *b) { return PATHCMP(folder_key[*(const int *)a], folder_key[*(const int *)b]); }

/* Makes a library of copies of v (sorted; the files that do not open, duration -1, left out) and the
 * playlists (names: their tracks' paths), and puts it in place of the old one. Returns the tracks kept. */
static int publish(const Item *v, int n, char **lists, Names *names, int nlists) {
    Library nl = {0};
    nl.item = malloc((n ? n : 1) * sizeof *nl.item);
    int *map = malloc((n ? n : 1) * sizeof *map), *idx = malloc((n ? n : 1) * sizeof *idx), nidx = 0;
    char **key = calloc(n ? n : 1, sizeof *key);
    for (int i = 0; i < n; i++) {
        map[i] = -1;
        if (v[i].duration < 0) continue;
        int k = nl.n++;
        map[i] = k;
        nl.item[k] = item_copy(&v[i]);
        nl.item[k].folder = -1;
        if (v[i].folder < 0) continue;
        key[k] = strdup(v[i].path);
        char *cut = (char *)base_name(key[k]);
        if (cut > key[k]) cut[-1] = 0;
        idx[nidx++] = k;
    }
    folder_key = key;
    qsort(idx, nidx, sizeof *idx, cmp_idx_folder);
    for (int j = 0; j < nidx; j++) {
        if (!j || PATHCMP(key[idx[j]], key[idx[j - 1]])) {
            nl.folder = realloc(nl.folder, (nl.nfolder + 1) * sizeof *nl.folder);
            nl.folder[nl.nfolder++] = (Folder){strdup(key[idx[j]]), 0};
        }
        nl.item[idx[j]].folder = nl.nfolder - 1;
        nl.folder[nl.nfolder - 1].count++;
    }
    for (int i = 0; i < nl.n; i++) free(key[i]);
    free(key); free(idx);
    nl.list = calloc(nlists ? nlists : 1, sizeof *nl.list);
    for (int p = 0; p < nlists; p++) {
        Playlist *pl = &nl.list[nl.nlist++];
        pl->path = strdup(lists[p]);
        pl->name = stem(lists[p]);
        pl->item = malloc((names[p].n ? names[p].n : 1) * sizeof *pl->item);
        for (int i = 0; i < names[p].n; i++) {
            Item key1 = {.path = names[p].path[i]}, *hit = bsearch(&key1, v, n, sizeof *v, cmp_path);
            if (hit && map[hit - v] >= 0) pl->item[pl->n++] = map[hit - v];
        }
    }
    free(map);
    int kept = nl.n;
    EnterCriticalSection(&L);
    Library old = lib;
    nl.gen = old.gen + 1;
    lib = nl;
    LeaveCriticalSection(&L);
    items_free(old.item, old.n);
    for (int i = 0; i < old.nfolder; i++) free(old.folder[i].path);
    free(old.folder);
    lists_free(old.list, old.nlist);
    return kept;
}

static volatile LONG scan_running, scan_again, scan_force, scan_done;

static int roots_get(char ***roots) {   /* music\ in the game folder, then music.ini's [libraries] */
    int n;
    char **v = ini_section("[libraries]", &n);
    v = realloc(v, (n + 1) * sizeof *v);
    memmove(v + 1, v, n * sizeof *v);
    v[0] = strdup("music");
    for (int i = 0; i <= n; i++) {
        char a[1100];
        absolute(v[i], a, sizeof a);
        norm(a);
        strip_sep(a);
        free(v[i]);
        v[i] = strdup(a);
    }
    *roots = v;
    return n + 1;
}

/* The cached tracks that are in the libraries, played from at once while the scan runs. */
static void publish_cached(void) {
    int nc, nr;
    char **roots;
    Item *c = index_read(&nc);
    if (!c) return;
    nr = roots_get(&roots);
    int k = 0;
    for (int i = 0; i < nc; i++) {
        int in = 0;
        for (int r = 0; r < nr && !in; r++) in = under(c[i].path, roots[r]);
        if (in) c[k++] = c[i]; else item_free(&c[i]);
    }
    if (k) publish(c, k, NULL, NULL, 0);
    items_free(c, k);
    strings_free(roots, nr);
}

static DWORD WINAPI scanner(void *arg) {
    (void)arg;
    do {
        time_t t0 = time(NULL);
        InterlockedExchange(&scan_again, 0);
        int force = (int)InterlockedExchange(&scan_force, 0);
        char **roots, **inilists;
        int nr = roots_get(&roots), nini;
        Found f = {0};
        for (int i = 0; i < nr; i++) walk(&f, roots[i], 0);
        inilists = ini_section("[playlists]", &nini);
        for (int i = 0; i < nini; i++) found_list(&f, inilists[i]);
        strings_free(inilists, nini);
        Names *names = calloc(f.nlist ? f.nlist : 1, sizeof *names);
        int web = 0, gone = 0;
        for (int p = 0; p < f.nlist; p++) playlist_read(f.list[p], &f, &names[p], &web, &gone);
        qsort(f.v, f.n, sizeof *f.v, cmp_item);
        int n = 0;
        for (int i = 0; i < f.n; i++) {   /* (one entry for each file) */
            if (n && !PATHCMP(f.v[n - 1].path, f.v[i].path)) { item_free(&f.v[i]); continue; }
            f.v[n++] = f.v[i];
        }
        f.n = n;
        int nc;
        Item *cache = force ? NULL : index_read(&nc);
        if (!cache) nc = 0;
        int reuse = 0, todo = 0;
        char *need = calloc(n ? n : 1, 1);
        for (int i = 0; i < n; i++) {
            Item key = {.path = f.v[i].path}, *hit = cache ? bsearch(&key, cache, nc, sizeof *cache, cmp_path) : NULL;
            if (hit && hit->size == f.v[i].size && hit->mtime == f.v[i].mtime) {
                Item *it = &f.v[i];
                free(it->title); free(it->artist); free(it->album);
                it->title = strdup(hit->title); it->artist = strdup(hit->artist); it->album = strdup(hit->album);
                it->duration = hit->duration;
                reuse++;
            } else need[i] = 1, todo++;
        }
        if (todo && todo < n) publish(f.v, n, f.list, names, f.nlist);   /* (play from what is known while the rest is read) */
        for (int i = 0; i < n; i++) if (need[i]) probe(&f.v[i]);
        int kept = publish(f.v, n, f.list, names, f.nlist);
        if (todo || reuse != nc) index_write(f.v, n);   /* (files gone or changed, or tracks added) */
        /* The scan's one line. Files that do not open, bad playlist entries and web addresses are left out quietly. */
        char out[300];
        int len = snprintf(out, sizeof out, "[music] %d tracks, %d playlists", kept, f.nlist);
        if (n - kept || gone || web)
            snprintf(out + len, sizeof out - len, "; left out: %d files that don't open, %d missing playlist entries, %d web addresses", n - kept, gone, web);
        fprintf(stderr, "%s (%d read, %d from the index, %d s)\n", out, todo, reuse, (int)(time(NULL) - t0));
        free(need);
        items_free(cache, nc);
        items_free(f.v, f.n);
        for (int p = 0; p < f.nlist; p++) { strings_free(names[p].path, names[p].n); free(f.list[p]); }
        free(names); free(f.list);
        strings_free(roots, nr);
        InterlockedExchange(&scan_done, 1);
        InterlockedExchange(&scan_running, 0);
    } while (scan_again && !InterlockedExchange(&scan_running, 1));
    return 0;
}

static void scan_start(int force) {
    if (force) InterlockedExchange(&scan_force, 1);
    if (InterlockedCompareExchange(&scan_running, 1, 0) == 0) CloseHandle(CreateThread(NULL, 0, scanner, NULL, 0, NULL));
    else InterlockedExchange(&scan_again, 1);
}

/* ---- the ring and the control state ------------------------------------------------------------- */
static struct {
    float ring[RING_FRAMES * 2];
    uint64_t wpos, rpos;     /* frames, ever: the player writes wpos, the mixer rpos */
    uint64_t flush_to;       /* where the player's writes went on after a command */
    uint32_t req, ack;       /* commands asked for / acted on by the player */
    int enabled, paused, volume, shuffle;
    int active;              /* the player has written something and goes on: an empty ring is an underrun */
    int idle;                /* nothing playable (musictest.c waits on it) */
    /* (the mixer's alone) */
    uint32_t gen;            /* the request it has acted on */
    float gain;
    int starving;
    uint32_t underruns;
} M = {.volume = 70, .shuffle = 1};

enum { C_START, C_STOP, C_NEXT, C_PREV, C_PICK };
static struct Track *pick;   /* (L) a track the user chose, opened by the caller, for the player to take over */
static uint8_t cmdq[16];   /* (L) */
static int ncmd;
static struct { uint64_t start; char title[256], artist[256]; double duration; int h, pos; } ent[NENT];   /* (L) tracks in the ring */
static int nent;
/* The tracks that have been played, as they began (L): the player decodes ahead, so skip and previous go by
 * what is heard (the entry of the ring's read position: its h in hist, its pos in the order), not by what the
 * player has reached. Library indexes; emptied when the library changes. */
static int hist[256], nhist;

static void post(int c) {
    EnterCriticalSection(&L);
    if (ncmd < (int)sizeof cmdq) cmdq[ncmd++] = (uint8_t)c;
    LeaveCriticalSection(&L);
    __atomic_add_fetch(&M.req, 1, __ATOMIC_SEQ_CST);
}

/* ---- one track: open, decode, resample ------------------------------------------------------------ */
typedef struct Track {
    AVFormatContext *fmt;
    AVCodecContext *dec;
    SwrContext *swr;
    AVPacket *pkt;
    AVFrame *frm;
    int stream, state;   /* 0 reading, 1 decoder draining, 2 resampler draining, 3 over */
    int errors, swr_fmt, swr_rate, swr_ch;
    float *out;
    int outcap, nout;
    uint64_t total;      /* frames produced */
    int announced;
    int idx, pos;        /* its place in the library and in the play order */
    char path[1100], title[256], artist[256];
    double duration;
} Track;

static void track_close(Track **pt) {
    Track *t = *pt;
    if (!t) return;
    if (t->swr) ff.swr_free(&t->swr);
    if (t->frm) ff.av_frame_free(&t->frm);
    if (t->pkt) ff.av_packet_free(&t->pkt);
    if (t->dec) ff.avcodec_free_context(&t->dec);
    if (t->fmt) ff.avformat_close_input(&t->fmt);
    free(t->out);
    free(t);
    *pt = NULL;
}

/* Opens a track; on failure NULL and, in *why, what is wrong (words for the user). */
static Track *track_try(const Item *it, const char **why) {
    Track *t = calloc(1, sizeof *t);
    snprintf(t->path, sizeof t->path, "%s", it->path);
    snprintf(t->title, sizeof t->title, "%s", it->title);
    snprintf(t->artist, sizeof t->artist, "%s", it->artist);
    t->duration = it->duration;
    const AVCodec *codec = NULL;
    const char *what = NULL;
    int r = ff.avformat_open_input(&t->fmt, t->path, NULL, NULL);
    if (r < 0) what = "the file doesn't open";
    else if ((r = ff.avformat_find_stream_info(t->fmt, NULL)) < 0) what = "the file can't be read";
    else if ((r = t->stream = ff.av_find_best_stream(t->fmt, AVMEDIA_TYPE_AUDIO, -1, -1, &codec, 0)) < 0) what = "no audio in it";   /* (cover art is a video stream: ignored) */
    else {
        for (unsigned i = 0; i < t->fmt->nb_streams; i++) if ((int)i != t->stream) t->fmt->streams[i]->discard = AVDISCARD_ALL;
        t->dec = ff.avcodec_alloc_context3(codec);
        t->pkt = ff.av_packet_alloc();
        t->frm = ff.av_frame_alloc();
        if (!t->dec || !t->pkt || !t->frm) { what = "out of memory"; r = AVERROR(ENOMEM); }
        else if ((r = ff.avcodec_parameters_to_context(t->dec, t->fmt->streams[t->stream]->codecpar)) < 0 ||
                 (r = ff.avcodec_open2(t->dec, codec, NULL)) < 0) what = "no decoder for it";
    }
    if (what) {
        *why = what;
        track_close(&t);
    }
    return t;
}

static Track *track_open(const Item *it) {   /* (reached by the order, not picked: skipped, with a log line) */
    const char *why = NULL;
    Track *t = track_try(it, &why);
    if (!t) fprintf(stderr, "[music] skipped while playing: %s: %s\n", it->path, why);
    else fprintf(stderr, "[music] next: %s (%s)\n", it->title, it->path);   /* (opened to decode ahead: heard up to a few seconds later) */
    return t;
}

/* Resamples one decoded frame (or, with NULL, what the resampler still holds) to 48 kHz stereo floats in
 * t->out. A resampler of its own for each rate and layout: a file may differ from the last one. */
static int convert(Track *t, const AVFrame *fr) {
    if (fr && (!t->swr || fr->format != t->swr_fmt || fr->sample_rate != t->swr_rate || fr->ch_layout.nb_channels != t->swr_ch)) {
        if (fr->sample_rate <= 0 || fr->format < 0 || fr->ch_layout.nb_channels <= 0) return 0;
        AVChannelLayout in = {0}, out = AV_CHANNEL_LAYOUT_STEREO;
        if (fr->ch_layout.order == AV_CHANNEL_ORDER_UNSPEC) ff.av_channel_layout_default(&in, fr->ch_layout.nb_channels);
        else ff.av_channel_layout_copy(&in, &fr->ch_layout);
        if (t->swr) ff.swr_free(&t->swr);
        int r = ff.swr_alloc_set_opts2(&t->swr, &out, AV_SAMPLE_FMT_FLT, RATE, &in, fr->format, fr->sample_rate, 0, NULL);
        if (r >= 0) {
            ff.av_opt_set_double(t->swr, "rematrix_maxval", 1.0, 0);   /* (a 5.1 mix down must not clip) */
            r = ff.swr_init(t->swr);
        }
        ff.av_channel_layout_uninit(&in);
        if (r < 0) { if (t->swr) ff.swr_free(&t->swr); return 0; }
        t->swr_fmt = fr->format; t->swr_rate = fr->sample_rate; t->swr_ch = fr->ch_layout.nb_channels;
    }
    if (!t->swr) return 1;
    int cap = ff.swr_get_out_samples(t->swr, fr ? fr->nb_samples : 0) + 4096;
    if (cap > t->outcap) { t->out = realloc(t->out, (size_t)cap * 2 * sizeof(float)); t->outcap = cap; }
    uint8_t *o[1] = {(uint8_t *)t->out};
    int n = ff.swr_convert(t->swr, o, t->outcap, fr ? (const uint8_t **)fr->extended_data : NULL, fr ? fr->nb_samples : 0);
    t->nout = n > 0 ? n : 0;
    t->total += t->nout;
    return n >= 0;
}

/* 1: t->out holds t->nout frames of the track; 0: it is over. FFmpeg trims the encoder delay and padding
 * (mp3's LAME tag, m4a's edit list, opus' pre-skip) from the frames it returns; nothing is trimmed here. */
static int track_step(Track *t) {
    for (;;) {
        if (t->state < 2) {
            int r = ff.avcodec_receive_frame(t->dec, t->frm);
            if (r >= 0) {
                t->errors = 0;
                int ok = convert(t, t->frm);
                ff.av_frame_unref(t->frm);
                if (!ok) t->state = 3;
                else if (t->nout) return 1;
                continue;
            }
            if (r == AVERROR_EOF || (r == AVERROR(EAGAIN) && t->state == 1)) { t->state = 2; continue; }
            if (r != AVERROR(EAGAIN)) {   /* a damaged frame: go on with the next, but not for ever */
                if (++t->errors > 100) t->state = 2;
                continue;
            }
            r = ff.av_read_frame(t->fmt, t->pkt);
            if (r < 0) { ff.avcodec_send_packet(t->dec, NULL); t->state = 1; continue; }
            if (t->pkt->stream_index == t->stream && ff.avcodec_send_packet(t->dec, t->pkt) < 0 && ++t->errors > 100) {
                ff.avcodec_send_packet(t->dec, NULL);
                t->state = 1;
            }
            ff.av_packet_unref(t->pkt);
        } else if (t->state == 2) {
            t->state = 3;
            if (t->swr && convert(t, NULL) && t->nout) { t->state = 2; return 1; }   /* (again, until it gives nothing) */
        } else return 0;
    }
}

/* ---- the player thread -------------------------------------------------------------------------------- */
static uint32_t seen;   /* the request the player has acted on */
static Track *cur, *nxt;
static int cur_idx = -1, nxt_idx = -1;
static int *ord, nord, pos = -1;
static char cur_path[1100];   /* the playing track, to keep its place when the order is rebuilt */
static int want_idx = -1;    /* the track a command chose (-1: the next in the order) */
static uint32_t ord_gen;
static uint64_t rng = 88172645463325252ull;

static uint32_t rnd(uint32_t n) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)((rng >> 11) % n); }

static void shuffle_ord(void) {
    for (int i = nord - 1; i > 0; i--) { int j = (int)rnd(i + 1), x = ord[i]; ord[i] = ord[j]; ord[j] = x; }
}

/* The play order of the source (a playlist: its own order; else the folder's tracks, or all of them), shuffled
 * when shuffle is on. `keep`, the playing track's path, stays where it is in the order (first when shuffled). */
static void rebuild(const char *keep) {
    EnterCriticalSection(&L);
    free(ord);
    int cap = lib.n;
    Playlist *pl = NULL;
    for (int p = 0; p < lib.nlist; p++) {
        if (*source && !PATHCMP(lib.list[p].path, source)) pl = &lib.list[p];
        if (lib.list[p].n > cap) cap = lib.list[p].n;
    }
    ord = malloc((cap ? cap : 1) * sizeof *ord);
    nord = 0;
    if (pl) { memcpy(ord, pl->item, pl->n * sizeof *ord); nord = pl->n; }
    else for (int i = 0; i < lib.n; i++) if (lib.item[i].folder >= 0 && (!*source || under(lib.item[i].path, source))) ord[nord++] = i;
    if (ord_gen != lib.gen) nhist = 0;   /* (the indexes of another library mean nothing) */
    ord_gen = lib.gen;
    int kept = -1;
    cur_idx = -1;
    for (int i = 0; keep && i < lib.n; i++) if (!PATHCMP(lib.item[i].path, keep)) { cur_idx = i; break; }
    for (int j = 0; cur_idx >= 0 && j < nord; j++) if (ord[j] == cur_idx) { kept = j; break; }
    LeaveCriticalSection(&L);
    pos = -1;
    if (LD(M.shuffle)) {
        shuffle_ord();
        if (kept >= 0) for (int j = 0; j < nord; j++) if (ord[j] == cur_idx) { ord[j] = ord[0]; ord[0] = cur_idx; pos = 0; break; }
    } else pos = kept;
}

/* The track after the current one in the order (pos moves only with commit). At the end the order starts
 * again: shuffled anew, and not with the track that just played. */
static int next_index(int commit) {
    if (LD(lib.gen) != ord_gen) rebuild(*cur_path ? cur_path : NULL);
    if (!nord) return -1;
    if (pos + 1 >= nord) {
        if (LD(M.shuffle)) {
            shuffle_ord();
            if (nord > 1 && ord[0] == cur_idx) { int j = 1 + (int)rnd(nord - 1); ord[0] = ord[j]; ord[j] = cur_idx; }
        }
        pos = -1;
    }
    int idx = ord[pos + 1];
    if (commit) pos++;
    return idx;
}

static int open_index(int idx, Track **out) {   /* 1: opened */
    Item copy;
    EnterCriticalSection(&L);
    int ok = idx >= 0 && idx < lib.n;
    if (ok) copy = lib.item[idx], copy.path = strdup(copy.path);
    LeaveCriticalSection(&L);
    if (!ok) return 0;
    *out = track_open(&copy);
    free(copy.path);
    return *out != NULL;
}

/* Writes frames to the ring, waiting while it is full. 0: a command came, the frames are dropped. */
static int push(const float *s, uint32_t n) {
    while (n) {
        uint64_t w = M.wpos, r = LD(M.rpos);
        uint32_t space = RING_FRAMES - (uint32_t)(w - r);
        if (!space) {
            if (LD(M.req) != seen) return 0;
            Sleep(5);
            continue;
        }
        uint32_t k = n < space ? n : space, at = (uint32_t)(w & (RING_FRAMES - 1)), first = RING_FRAMES - at;
        if (k <= first) memcpy(&M.ring[at * 2], s, k * 8);
        else { memcpy(&M.ring[at * 2], s, first * 8); memcpy(M.ring, s + first * 2, (k - first) * 8); }
        ST(M.wpos, w + k);
        s += k * 2;
        n -= k;
    }
    return 1;
}

static void announce(Track *t) {   /* the first sound of a track: remember where it starts in the ring */
    EnterCriticalSection(&L);
    uint64_t rp = LD(M.rpos);
    while (nent >= 2 && ent[1].start <= rp) { memmove(ent, ent + 1, --nent * sizeof *ent); }
    if (nent == NENT) memmove(ent, ent + 1, --nent * sizeof *ent);
    if (nhist == 256) { memmove(hist, hist + 1, --nhist * sizeof *hist); for (int i = 0; i < nent; i++) ent[i].h--; }
    hist[nhist] = t->idx;
    ent[nent].h = nhist++;
    ent[nent].pos = t->pos;
    ent[nent].start = M.wpos;
    snprintf(ent[nent].title, sizeof ent[nent].title, "%s", t->title);
    snprintf(ent[nent].artist, sizeof ent[nent].artist, "%s", t->artist);
    ent[nent++].duration = t->duration;
    LeaveCriticalSection(&L);
    t->announced = 1;
}

/* A command: whatever plays is dropped (the mixer is fading out meanwhile), the ring is flushed from here,
 * and the command is done. */
static void command(void) {
    uint8_t q[16];
    EnterCriticalSection(&L);
    uint32_t r = M.req;
    int n = ncmd;
    memcpy(q, cmdq, n);
    ncmd = 0;
    Track *picked = pick;
    pick = NULL;
    int heard = -1;   /* the track being heard: its hist entry */
    uint64_t rp = LD(M.rpos);
    for (int i = 0; i < nent; i++) if (ent[i].start <= rp && ent[i].h < nhist) heard = i;
    int heard_in = 0;
    if (heard >= 0) {
        cur_idx = hist[ent[heard].h];
        pos = ent[heard].pos;
        nhist = ent[heard].h + 1;   /* (what was decoded after it was not heard) */
        heard_in = 1;
    }
    nent = 0;
    LeaveCriticalSection(&L);
    seen = r;
    track_close(&nxt);
    track_close(&cur);
    M.flush_to = M.wpos;
    ST(M.active, 0);
    ST(M.ack, r);
    want_idx = -1;
    for (int i = 0; i < n; i++) {
        if (q[i] == C_START) { rebuild(NULL); nhist = 0; want_idx = -1; heard_in = 0; }
        else if (q[i] == C_STOP) want_idx = -1;
        else if (q[i] == C_NEXT) {
            want_idx = next_index(1);
            cur_idx = want_idx;
            heard_in = 0;
        } else if (q[i] == C_PREV) {
            EnterCriticalSection(&L);
            if (heard_in) nhist--;   /* (this one will be played again, and noted again) */
            want_idx = nhist ? hist[--nhist] : cur_idx;
            LeaveCriticalSection(&L);
            cur_idx = want_idx;
            heard_in = 0;
            for (int j = 0; j < nord; j++) if (ord[j] == want_idx) { pos = j; break; }   /* (next goes on from there) */
        } else if (q[i] == C_PICK && picked) {   /* (opened by the caller, who has seen it play) */
            cur = picked;
            picked = NULL;
            snprintf(cur_path, sizeof cur_path, "%s", cur->path);
            rebuild(cur_path);   /* (the order goes on from it) */
            cur->idx = cur_idx;
            cur->pos = pos;
            want_idx = -1;
        }
    }
    track_close(&picked);
}

static DWORD WINAPI player(void *arg) {
    (void)arg;
    rng ^= (uint64_t)time(NULL) * 2654435761u ^ (uintptr_t)&rng;
    int fails = 0, preload_tried = 0, gave_up = 0;
    uint32_t gave_gen = 0;
    for (;;) {
        if (LD(M.req) != seen) {
            command();
            fails = gave_up = 0;
            preload_tried = 0;
            continue;
        }
        if (!LD(M.enabled)) {
            track_close(&nxt);
            track_close(&cur);
            ST(M.active, 0);
            ST(M.idle, 0);
            Sleep(20);
            continue;
        }
        if (gave_up && gave_gen == LD(lib.gen)) { ST(M.idle, 1); Sleep(50); continue; }
        gave_up = 0;
        ST(M.idle, 0);
        if (!cur) {
            int idx = want_idx >= 0 ? want_idx : next_index(1);
            want_idx = -1;
            if (idx < 0 || !open_index(idx, &cur)) {   /* (a track that does not play is passed over, quietly) */
                if (idx < 0 || ++fails >= (nord > 0 ? nord : 1)) { gave_up = 1; gave_gen = LD(lib.gen); ST(M.active, 0); }
                continue;
            }
            fails = 0;
            cur_idx = idx;
            cur->idx = idx;
            cur->pos = pos;
            snprintf(cur_path, sizeof cur_path, "%s", cur->path);
            preload_tried = 0;
            ST(M.idle, 0);
        }
        if (LD(lib.gen) != ord_gen) { track_close(&nxt); preload_tried = 0; rebuild(cur_path); }
        /* preload: the next track is open before this one runs out, so a slow disk costs no gap */
        if (!nxt && !preload_tried && (cur->total > RATE || cur->state >= 1)) {
            preload_tried = 1;
            for (int tries = 0; tries < 8 && !nxt; tries++) {
                int idx = next_index(0);
                if (idx < 0) break;
                if (open_index(idx, &nxt)) { nxt_idx = idx; nxt->idx = idx; nxt->pos = pos + 1; }
                else pos++;   /* (one that does not play is passed over) */
            }
        }
        if (track_step(cur) > 0) {
            if (!cur->announced) announce(cur);
            if (push(cur->out, (uint32_t)cur->nout)) ST(M.active, 1);
        } else {
            if (!cur->total) fprintf(stderr, "[music] skipped while playing: %s: no audio decoded\n", cur->path);
            track_close(&cur);
            if (nxt) {
                cur = nxt; nxt = NULL; cur_idx = nxt_idx; pos++; preload_tried = 0;
                snprintf(cur_path, sizeof cur_path, "%s", cur->path);
            }
        }
        MUSIC_STEP_HOOK();
    }
}

/* ---- the mixer: the output thread, never blocks, takes no lock ------------------------------------------- */
void music_mix(int16_t *out, uint32_t frames, int master) {
    uint32_t req = LD(M.req);
    int on = LD(M.enabled), running = on && !LD(M.paused);
    int waiting = M.gen != req;
    if (!on && M.gain == 0 && !waiting) return;
    float vol = running ? LD(M.volume) * 0.01f * master * 0.01f : 0.0f;
    uint64_t r = M.rpos, w = LD(M.wpos), lim = w;
    const float step = 1.0f / RAMP;
    for (uint32_t i = 0; i < frames; i++) {
        if (waiting) {   /* faded out, and the player has dropped what it had: go on from where its new sound starts */
            if (M.gain == 0 && LD(M.ack) == req) {
                if (M.flush_to > r) r = M.flush_to;
                M.gen = req;
                waiting = 0;
                M.starving = 0;
                w = lim = LD(M.wpos);
            } else if (LD(M.ack) == req) lim = M.flush_to < w ? M.flush_to : w;
        }
        float target = waiting ? 0.0f : vol;
        if (M.gain < target) { M.gain += step; if (M.gain > target) M.gain = target; }
        else if (M.gain > target) { M.gain -= step; if (M.gain < target) M.gain = target; }
        if ((waiting || !running) && M.gain == 0) continue;   /* (stalled: time does not pass for the music) */
        float l = 0, rt = 0;
        if (r < lim) {
            const float *s = &M.ring[(r & (RING_FRAMES - 1)) * 2];
            l = s[0]; rt = s[1];
            r++;
            M.starving = 0;
        } else if (!waiting && !M.starving && LD(M.active)) {
            M.starving = 1;
            if (++M.underruns <= 5) fprintf(stderr, "[music] underrun %u: the player is behind\n", M.underruns);
        }
        float a = out[2 * i] + l * M.gain * 32768.0f, b = out[2 * i + 1] + rt * M.gain * 32768.0f;
        a = a > 32767.0f ? 32767.0f : a < -32768.0f ? -32768.0f : a;
        b = b > 32767.0f ? 32767.0f : b < -32768.0f ? -32768.0f : b;
        out[2 * i] = (int16_t)(a + (a < 0 ? -0.5f : 0.5f));
        out[2 * i + 1] = (int16_t)(b + (b < 0 ? -0.5f : 0.5f));
    }
    ST(M.rpos, r);
}

/* ---- the control API ---------------------------------------------------------------------------------------- */
static BOOL CALLBACK init_cb(PINIT_ONCE o, PVOID p, PVOID *c) { (void)o; (void)p; (void)c; InitializeCriticalSection(&L); return TRUE; }
static void init(void) {
    static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
    InitOnceExecuteOnce(&once, init_cb, NULL, NULL);
}

static int setting(const char *env, const char *key, char *out, int size) {
    const char *e = getenv(env);
    if (e && *e) { snprintf(out, size, "%s", e); return 1; }
    out[0] = 0;
    return settings_ini_get(display_ini_path(), NULL, key, out, size);
}
static int percent(const char *v, int def) { return v[0] && strspn(v, "0123456789") == strlen(v) ? (atoi(v) > 100 ? 100 : atoi(v)) : def; }

int music_available(void) { init(); EnterCriticalSection(&L); int ok = ff_state > 0; LeaveCriticalSection(&L); return ok; }
int music_enabled(void) { return LD(M.enabled); }

int music_set_enabled(int on) {
    init();
    on = on != 0;
    if (on == LD(M.enabled)) return on;
    if (on) {
        EnterCriticalSection(&L);
        int ok = load_ffmpeg();
        LeaveCriticalSection(&L);
        if (!ok) return 0;
        static volatile LONG started;
        ST(M.enabled, 1);
        audio_set_game_music(0);
        if (!InterlockedExchange(&started, 1)) {
            publish_cached();
            scan_start(0);
            CloseHandle(CreateThread(NULL, 0, player, NULL, 0, NULL));
        }
        post(C_START);
    } else {
        ST(M.enabled, 0);
        audio_set_game_music(1);
        post(C_STOP);
    }
    return on;
}

void music_player_start(void) {
    init();
    char v[1100];
    setting("SSBU_MUSIC_VOLUME", "music_volume", v, sizeof v);
    M.volume = percent(v, 70);
    setting("SSBU_MUSIC_SHUFFLE", "music_shuffle", v, sizeof v);
    M.shuffle = v[0] == '0' && !v[1] ? 0 : 1;
    setting("SSBU_MUSIC_SOURCE", "music_source", v, sizeof v);
    norm(v);
    strip_sep(v);
    snprintf(source, sizeof source, "%s", v);
    setting("SSBU_MUSIC", "music", v, sizeof v);
    if (v[0] == '1' && !v[1]) music_set_enabled(1);
}

int music_volume(void) { return LD(M.volume); }
void music_set_volume(int p) { ST(M.volume, p < 0 ? 0 : p > 100 ? 100 : p); }
int music_shuffle(void) { return LD(M.shuffle); }
void music_set_shuffle(int on) { ST(M.shuffle, on != 0); }
void music_get_source(char *out, int size) { init(); EnterCriticalSection(&L); snprintf(out, size, "%s", source); LeaveCriticalSection(&L); }
void music_set_source(const char *folder) {
    init();
    EnterCriticalSection(&L);
    snprintf(source, sizeof source, "%s", folder ? folder : "");
    norm(source);
    strip_sep(source);
    LeaveCriticalSection(&L);
    if (LD(M.enabled)) post(C_START);
}
void music_next(void) { init(); if (LD(M.enabled)) post(C_NEXT); }
void music_previous(void) { init(); if (LD(M.enabled)) post(C_PREV); }
void music_pause(int paused) { ST(M.paused, paused != 0); }
int music_paused(void) { return LD(M.paused); }
void music_rescan(void) { init(); if (LD(M.enabled)) scan_start(1); }
int music_scanning(void) { return (int)scan_running; }
int music_track_count(void) { init(); EnterCriticalSection(&L); int n = lib.n; LeaveCriticalSection(&L); return n; }

int music_folders(MusicFolder *out, int max) {
    init();
    EnterCriticalSection(&L);
    int n = 0;
    for (int i = 0; i < lib.nfolder; i++, n++)
        if (n < max) {
            snprintf(out[n].path, sizeof out[n].path, "%s", lib.folder[i].path);
            snprintf(out[n].name, sizeof out[n].name, "%s", base_name(lib.folder[i].path));
            out[n].tracks = lib.folder[i].count;
            out[n].playlist = 0;
        }
    for (int i = 0; i < lib.nlist; i++, n++)
        if (n < max) {
            snprintf(out[n].path, sizeof out[n].path, "%s", lib.list[i].path);
            snprintf(out[n].name, sizeof out[n].name, "%s", lib.list[i].name);
            out[n].tracks = lib.list[i].n;
            out[n].playlist = 1;
        }
    LeaveCriticalSection(&L);
    return n;
}

void music_now(MusicNow *now) {
    memset(now, 0, sizeof *now);
    init();
    EnterCriticalSection(&L);
    uint64_t rp = LD(M.rpos);
    int e = -1;
    for (int i = 0; i < nent; i++) if (ent[i].start <= rp) e = i;
    if (LD(M.enabled) && e >= 0) {
        now->state = LD(M.paused) ? 2 : 1;
        snprintf(now->title, sizeof now->title, "%s", ent[e].title);
        snprintf(now->artist, sizeof now->artist, "%s", ent[e].artist);
        now->duration = ent[e].duration > 0 ? ent[e].duration : 0;
        now->position = (rp - ent[e].start) / (double)RATE;
        if (now->duration && now->position > now->duration) now->position = now->duration;
    }
    LeaveCriticalSection(&L);
}

/* The user picked this track: open it now and say why if it cannot play, leaving what plays as it is.
 * Only this is an error to show; tracks reached any other way are skipped quietly. */
int music_play_track(const char *path, char *why, int size) {
    init();
    char shown[300];
    Item it = {0};
    if (why && size) why[0] = 0;
    EnterCriticalSection(&L);
    for (int i = 0; i < lib.n; i++)
        if (!PATHCMP(lib.item[i].path, path)) { it = item_copy(&lib.item[i]); break; }
    LeaveCriticalSection(&L);
    if (!it.path) { it.path = strdup(path); it.title = stem(path); it.artist = strdup(""); it.album = strdup(""); }
    snprintf(shown, sizeof shown, "%s", *it.title ? it.title : base_name(path));
    const char *bad = NULL;
    Track *t = NULL;
    if (!LD(M.enabled)) bad = "music is off";
    else {
        EnterCriticalSection(&L);
        int ok = ff_state > 0;
        LeaveCriticalSection(&L);
        t = ok ? track_try(&it, &bad) : NULL;
        if (!ok) bad = "FFmpeg is not available";
    }
    item_free(&it);
    if (!t) {
        if (why && size) snprintf(why, size, "Couldn't play \"%s\": %s", shown, bad ? bad : "the file doesn't play");
        return 0;
    }
    EnterCriticalSection(&L);
    Track *old = pick;
    pick = t;
    LeaveCriticalSection(&L);
    track_close(&old);
    post(C_PICK);
    return 1;
}
