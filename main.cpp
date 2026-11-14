// main.cpp — minimal streaming FLAC player with per-song skip.
//
// Long recordings (a whole minidisc in one .flac) get their song starts
// detected once, by looking for quiet gaps, and the result is stored inside
// the file as a standard FLAC CUESHEET block (plus a readable .cue next to
// it). After that, skipping is an instant seek to the next stored marker.
//
// Build:  g++ -O2 -std=c++17 main.cpp -I/usr/include/miniaudio -o player -lpthread -lm -ldl
// Usage:  ./player file1.flac|folder [...]             play (labels files on first use)
//         ./player --scan file1.flac|folder [...]      (re)label files and exit
//
// Library layout: Artist/Album/*.flac|*.mp3. An album folder may hold a cover.ppm
// (made by ./pixelate.py) which is drawn as pixel art next to the track info.
// Keys:   space/p pause   left/right arrow (or h/l) seek   s skip song   b back a song   n next file   q quit
//         (playing a folder: every file is one song, no song detection)

// Only the decoding + playback parts of miniaudio are used; leave the rest out.
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_ENGINE
#define MA_NO_NODE_GRAPH
#define MA_NO_RESOURCE_MANAGER
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <map>
#ifdef _WIN32
#  define WIN32_LEAN_AND_MEAN
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <conio.h>
#  include <io.h>
#  include <process.h>
#  include <shlobj.h>
#  include <sys/stat.h>
#elif defined(__APPLE__)
#  include <dirent.h>
#  include <sys/stat.h>
#  include <fcntl.h>
#  include <poll.h>
#  include <sys/ioctl.h>
#  include <sys/wait.h>
#  include <termios.h>
#  include <unistd.h>
#  include <mach-o/dyld.h>
#  include <thread>
#else
#  include <dirent.h>
#  include <sys/stat.h>
#  include <fcntl.h>
#  include <poll.h>
#  include <sys/ioctl.h>
#  include <sys/wait.h>
#  include <termios.h>
#  include <unistd.h>
#endif

// ---------- tunables ----------
static const double WINDOW_SEC     = 0.05;   // loudness is measured in blocks this long
static const double FLOOR_PCT      = 0.05;   // percentile used as the recording's noise floor
static const double FLOOR_MARGIN   = 14.0;   // "quiet" = within this many dB of the noise floor
static const double THRESH_MIN_DB  = -50.0;  // ...but never outside this range
static const double THRESH_MAX_DB  = -34.0;
static const double GAP_SECONDS    = 1.5;    // quiet this long = gap between songs
static const double MIN_TRACK_SEC  = 20.0;   // don't create songs shorter than this
static const double PREROLL_SEC    = 0.25;   // start slightly before the first sound
static const double BACK_GRACE_SEC = 3.0;    // "back" this soon into a song goes to the previous one
static const int    FRAME_MS       = 50;     // display refresh / key polling interval

typedef std::vector<ma_uint64> Marks;        // frame at which each song starts, ascending

// Index of the song containing `pos` (the lead-in before the first marker counts as song 0).
static size_t current_song(const Marks& marks, ma_uint64 pos) {
    const size_t after = std::upper_bound(marks.begin(), marks.end(), pos) - marks.begin();
    return after > 0 ? after - 1 : 0;
}

// ---------- player ----------
struct Player {
    ma_decoder decoder;
    ma_device  device;
    std::mutex mtx;                       // guards the decoder (audio thread vs. seeks)
    std::atomic<ma_uint64> pos{0};        // frames played so far
    std::atomic<float>     level{0.0f};   // peak of the latest audio block, 0..1 (for the display)
    std::atomic<bool> paused{false};
    std::atomic<bool> finished{false};
    std::atomic<bool> deviceReady{false};
};

// Rough peak of a block, looking at every 8th sample — plenty for a level meter.
static float block_peak(ma_format format, const void* data, ma_uint64 samples) {
    float peak = 0.0f;
    if (format == ma_format_s16) {
        const ma_int16* s = static_cast<const ma_int16*>(data);
        int m = 0;
        for (ma_uint64 i = 0; i < samples; i += 8) m = std::max(m, std::abs(static_cast<int>(s[i])));
        peak = static_cast<float>(m) / 32768.0f;
    } else if (format == ma_format_s32) {
        const ma_int32* s = static_cast<const ma_int32*>(data);
        ma_int64 m = 0;
        for (ma_uint64 i = 0; i < samples; i += 8) m = std::max<ma_int64>(m, std::llabs(static_cast<ma_int64>(s[i])));
        peak = static_cast<float>(static_cast<double>(m) / 2147483648.0);
    } else if (format == ma_format_f32) {
        const float* s = static_cast<const float*>(data);
        for (ma_uint64 i = 0; i < samples; i += 8) peak = std::max(peak, std::fabs(s[i]));
    } else {
        peak = 0.5f;                      // other formats: just keep the wave moving
    }
    return std::min(peak, 1.0f);
}

static void data_callback(ma_device* dev, void* out, const void*, ma_uint32 frameCount) {
    Player* p = static_cast<Player*>(dev->pUserData);

    const ma_uint32 channels = dev->playback.channels;
    const ma_uint32 bpf      = ma_get_bytes_per_frame(dev->playback.format, channels);

    if (p->paused.load(std::memory_order_relaxed)) {
        std::memset(out, 0, frameCount * bpf);
        return;
    }

    ma_uint64 got = 0;
    {
        std::lock_guard<std::mutex> lk(p->mtx);
        ma_decoder_read_pcm_frames(&p->decoder, out, frameCount, &got);
        p->pos.fetch_add(got, std::memory_order_relaxed);
    }
    p->level.store(block_peak(dev->playback.format, out, got * channels), std::memory_order_relaxed);

    if (got < frameCount) {
        std::memset(static_cast<unsigned char*>(out) + got * bpf, 0,
                    (frameCount - got) * bpf);
        p->finished.store(true, std::memory_order_relaxed);
    }
}

// ---------- song markers: reading them back out of the file ----------
static ma_uint64 be64(const unsigned char* b) {
    ma_uint64 v = 0;
    for (int i = 0; i < 8; ++i) v = (v << 8) | b[i];
    return v;
}

// Read song starts from the FLAC file's embedded CUESHEET metadata block.
static bool read_embedded_marks(const std::string& path, Marks& marks) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return false;

    bool found = false;
    unsigned char hdr[4];
    if (std::fread(hdr, 1, 4, f) == 4 && std::memcmp(hdr, "fLaC", 4) == 0) {
        while (std::fread(hdr, 1, 4, f) == 4) {
            const bool   last = (hdr[0] & 0x80) != 0;
            const int    type = hdr[0] & 0x7F;
            const size_t len  = (size_t(hdr[1]) << 16) | (size_t(hdr[2]) << 8) | hdr[3];

            if (type != 5) {                       // 5 = CUESHEET
                if (last || std::fseek(f, static_cast<long>(len), SEEK_CUR) != 0) break;
                continue;
            }

            std::vector<unsigned char> b(len);
            if (std::fread(b.data(), 1, len, f) != len || len < 396) break;

            // 128 catalog + 8 lead-in + 259 flags/reserved, then the track count
            const unsigned nTracks = b[395];
            size_t p = 396;
            marks.clear();
            for (unsigned t = 0; t < nTracks && p + 36 <= len; ++t) {
                const ma_uint64 trackOff = be64(&b[p]);
                const unsigned  number   = b[p + 8];
                const unsigned  nIdx     = b[p + 35];
                p += 36;

                ma_uint64 idxOff = 0;              // prefer INDEX 01, else the first index
                for (unsigned i = 0; i < nIdx && p + 12 <= len; ++i, p += 12)
                    if (i == 0 || b[p + 8] == 1) idxOff = be64(&b[p]);

                if (number != 170 && number != 255) // those are the lead-out
                    marks.push_back(trackOff + idxOff);
            }
            std::sort(marks.begin(), marks.end());
            found = !marks.empty();
            break;
        }
    }
    std::fclose(f);
    return found;
}

// ---------- song markers: detecting them ----------
// Decode the whole file once, measure loudness in short blocks, and call the
// end of every sustained quiet stretch the start of a new song.
static bool scan_marks(const std::string& path, Marks& marks, ma_uint32& sampleRate) {
    // 16-bit integers: no float conversion for CD-quality files, integer maths below.
    ma_decoder_config cfg = ma_decoder_config_init(ma_format_s16, 0, 0);
    ma_decoder d;
    if (ma_decoder_init_file(path.c_str(), &cfg, &d) != MA_SUCCESS) {
        std::fprintf(stderr, "Failed to open: %s\n", path.c_str());
        return false;
    }

    sampleRate = d.outputSampleRate;
    const ma_uint32 channels = d.outputChannels;
    const ma_uint64 win      = std::max<ma_uint64>(1, static_cast<ma_uint64>(sampleRate * WINDOW_SEC));
    const ma_uint64 chunk    = win * 64;           // decode many blocks per call

    std::vector<ma_int16> buf(chunk * channels);
    std::vector<float>    db;                      // loudness of each block, in dBFS

    ma_uint64 total = 0;
    if (ma_decoder_get_length_in_pcm_frames(&d, &total) == MA_SUCCESS)
        db.reserve(static_cast<size_t>(total / win) + 1);

    while (true) {
        ma_uint64 filled = 0;
        while (filled < chunk) {
            ma_uint64 got = 0;
            ma_decoder_read_pcm_frames(&d, buf.data() + filled * channels, chunk - filled, &got);
            if (got == 0) break;
            filled += got;
        }

        for (ma_uint64 at = 0; at < filled; at += win) {
            const ma_uint64 n   = std::min(win, filled - at) * channels;
            const ma_int16* s   = buf.data() + at * channels;
            ma_uint64       sum = 0;
            for (ma_uint64 i = 0; i < n; ++i)
                sum += static_cast<ma_uint64>(static_cast<ma_int32>(s[i]) * s[i]);
            const double rms = std::sqrt(static_cast<double>(sum) / static_cast<double>(n)) / 32768.0;
            db.push_back(static_cast<float>(20.0 * std::log10(rms + 1e-9)));
        }

        if (filled < chunk) break;
    }
    ma_decoder_uninit(&d);

    // Noise floor of this particular recording (tape hiss etc.), ignoring
    // pure digital silence, so the threshold adapts to each disc.
    std::vector<float> audible;
    for (float v : db) if (v > -90.0f) audible.push_back(v);
    marks.clear();
    if (audible.empty()) return true;              // file is entirely silent

    const size_t k = static_cast<size_t>(audible.size() * FLOOR_PCT);
    std::nth_element(audible.begin(), audible.begin() + k, audible.end());
    const double thresh = std::min(THRESH_MAX_DB,
                          std::max(THRESH_MIN_DB, audible[k] + FLOOR_MARGIN));

    const size_t    gapBlocks = static_cast<size_t>(GAP_SECONDS / WINDOW_SEC);
    const ma_uint64 preRoll   = static_cast<ma_uint64>(sampleRate * PREROLL_SEC);
    const ma_uint64 minTrack  = static_cast<ma_uint64>(sampleRate * MIN_TRACK_SEC);
    const ma_uint64 cueFrame  = std::max<ma_uint32>(1, sampleRate / 75);  // .cue resolution

    size_t quietRun = gapBlocks;                   // start of file counts as a gap
    for (size_t i = 0; i < db.size(); ++i) {
        if (db[i] < thresh) { ++quietRun; continue; }

        if (quietRun >= gapBlocks) {
            ma_uint64 start = i * win;
            start = start > preRoll ? start - preRoll : 0;
            start -= start % cueFrame;
            if (marks.empty() || start >= marks.back() + minTrack)
                marks.push_back(start);
        }
        quietRun = 0;
    }
    return true;
}

// ---------- song markers: writing them into the file ----------
#ifdef _WIN32
// Windows passes one command line, so arguments with spaces need quoting.
static std::string win_quote(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) return a;
    std::string q = "\"";
    for (char c : a) { if (c == '"') q += '\\'; q += c; }
    return q + "\"";
}

// Runs a program and returns its exit code; `quiet` hides its error messages.
static int run_program(const std::vector<std::string>& args, bool quiet) {
    std::vector<std::string> quoted;
    for (const std::string& a : args) quoted.push_back(win_quote(a));
    std::vector<const char*> argv;
    for (const std::string& a : quoted) argv.push_back(a.c_str());
    argv.push_back(nullptr);
    int saved = -1;
    if (quiet) {
        std::fflush(stderr);
        saved = _dup(2);
        FILE* nul = std::fopen("NUL", "w");
        if (nul) { _dup2(_fileno(nul), 2); std::fclose(nul); }
    }
    const intptr_t rc = _spawnvp(_P_WAIT, argv[0], argv.data());
    if (saved >= 0) { _dup2(saved, 2); _close(saved); }
    return rc < 0 ? -1 : static_cast<int>(rc);
}
static int run_tool(const std::vector<std::string>& args) { return run_program(args, true); }
#else
static int run_program(const std::vector<std::string>& args, bool quiet) {
    std::vector<char*> argv;
    for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);

    const pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        const int devnull = open("/dev/null", O_WRONLY);
        if (quiet && devnull >= 0) dup2(devnull, STDERR_FILENO);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}
// Runs a program quietly (its own messages are hidden) and returns its exit code.
static int run_tool(const std::vector<std::string>& args) { return run_program(args, true); }
#endif

// Write "<file>.cue" and embed it in the FLAC as a CUESHEET block.
static bool save_marks(const std::string& path, const Marks& marks, ma_uint32 sampleRate) {
    const std::string cuePath = path.substr(0, path.find_last_of('.')) + ".cue";
    const size_t      slash   = path.find_last_of("/\\");
    const std::string base    = slash == std::string::npos ? path : path.substr(slash + 1);

    FILE* f = std::fopen(cuePath.c_str(), "w");
    if (!f) { std::fprintf(stderr, "Cannot write %s\n", cuePath.c_str()); return false; }

    std::fprintf(f, "FILE \"%s\" WAVE\n", base.c_str());
    const size_t n = std::min<size_t>(marks.size(), 99);          // cue sheets allow 99 tracks
    for (size_t i = 0; i < n; ++i) {
        const ma_uint64 cf = marks[i] * 75 / sampleRate;          // 1/75 s cue frames
        std::fprintf(f, "  TRACK %02zu AUDIO\n", i + 1);
        if (i == 0 && cf > 0) std::fprintf(f, "    INDEX 00 00:00:00\n");
        std::fprintf(f, "    INDEX 01 %02llu:%02llu:%02llu\n",
                     static_cast<unsigned long long>(cf / 75 / 60),
                     static_cast<unsigned long long>(cf / 75 % 60),
                     static_cast<unsigned long long>(cf % 75));
    }
    std::fclose(f);

    // FLAC files also get the markers embedded; other formats keep just the .cue.
    std::string ext = path.substr(path.find_last_of('.') + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (ext != "flac") return true;

    run_tool({"metaflac", "--remove", "--block-type=CUESHEET", path});
    if (run_tool({"metaflac", "--import-cuesheet-from=" + cuePath, path}) != 0) {
        std::fprintf(stderr, "Could not embed markers in %s (is metaflac installed?); "
                             "they are only in %s\n", path.c_str(), cuePath.c_str());
        return false;
    }
    return true;
}

// Song starts from the "<file>.cue" next to it (INDEX 01 of every track).
static bool read_cue_marks(const std::string& path, Marks& marks) {
    const std::string cuePath = path.substr(0, path.find_last_of('.')) + ".cue";
    FILE* f = std::fopen(cuePath.c_str(), "r");
    if (!f) return false;

    ma_uint32 rate = 0;                            // cue times are in 1/75 s; need the sample rate
    ma_decoder d;
    if (ma_decoder_init_file(path.c_str(), nullptr, &d) == MA_SUCCESS) {
        rate = d.outputSampleRate;
        ma_decoder_uninit(&d);
    }
    marks.clear();
    char line[512];
    unsigned mm, ss, ff;
    while (rate && std::fgets(line, sizeof line, f))
        if (std::sscanf(line, " INDEX 01 %u:%u:%u", &mm, &ss, &ff) == 3)
            marks.push_back((static_cast<ma_uint64>(mm) * 60 * 75 + ss * 75 + ff) * rate / 75);
    std::fclose(f);
    std::sort(marks.begin(), marks.end());
    return !marks.empty();
}

// Markers stored for a file: embedded in a FLAC, else the .cue next to it.
static bool read_stored_marks(const std::string& path, Marks& marks) {
    return read_embedded_marks(path, marks) || read_cue_marks(path, marks);
}

// Markers for a file: the stored ones, or (first time / forced) a fresh scan.
// A file always has at least one song, starting at 0, even if nothing was found.
static Marks get_marks(const std::string& path, bool forceScan) {
    Marks marks;
    if (forceScan || !read_stored_marks(path, marks)) {
        ma_uint32 sampleRate = 0;
        if (scan_marks(path, marks, sampleRate) && !marks.empty())
            save_marks(path, marks, sampleRate);
    }
    if (marks.empty()) marks.push_back(0);
    return marks;
}

static std::string mmss(ma_uint64 frames, ma_uint32 rate) {
    const unsigned long long s = rate ? frames / rate : 0;
    char b[32];
    std::snprintf(b, sizeof b, "%llu:%02llu", s / 60, s % 60);
    return b;
}

static std::string display_name(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    std::string  name  = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot   = name.find_last_of('.');
    return dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
}

// ---------- library: folders, tags, covers ----------
static bool is_dir(const std::string& p) {
#ifdef _WIN32
    std::wstring w;
    if (!p.empty()) {
        const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p.data(), static_cast<int>(p.size()), nullptr, 0);
        if (n > 0) {
            w.resize(static_cast<size_t>(n));
            MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, p.data(), static_cast<int>(p.size()), &w[0], n);
        }
    }
    return !w.empty() && (GetFileAttributesW(w.c_str()) & FILE_ATTRIBUTE_DIRECTORY) != 0;
#else
    struct stat st {};
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

// Formats the player handles (miniaudio decodes both).
static bool is_audio(const std::string& name) {
    const size_t dot = name.find_last_of('.');
    if (dot == std::string::npos) return false;
    std::string ext = name.substr(dot + 1);
    for (char& c : ext) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return ext == "flac" || ext == "mp3";
}

// Every .flac / .mp3 under `dir` (recursing into album folders), sorted by path.
static void collect_audio(const std::string& dir, std::vector<std::string>& out) {
#ifdef _WIN32
    std::wstring wdir;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, dir.data(), static_cast<int>(dir.size()), nullptr, 0);
    if (n <= 0) return;
    wdir.resize(static_cast<size_t>(n));
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, dir.data(), static_cast<int>(dir.size()), &wdir[0], n);
    WIN32_FIND_DATAW data{};
    HANDLE h = FindFirstFileW((wdir + L"\\*").c_str(), &data);
    if (h == INVALID_HANDLE_VALUE) return;
    std::vector<std::string> found;
    do {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, data.cFileName, -1, nullptr, 0, nullptr, nullptr);
        if (bytes <= 1) continue;
        std::string name(static_cast<size_t>(bytes - 1), '\0');
        WideCharToMultiByte(CP_UTF8, 0, data.cFileName, -1, &name[0], bytes, nullptr, nullptr);
        const std::string path = dir + (dir.back() == '/' || dir.back() == '\\' ? "" : "\\") + name;
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) collect_audio(path, found);
        else if (is_audio(name)) found.push_back(path);
    } while (FindNextFileW(h, &data));
    FindClose(h);
    std::sort(found.begin(), found.end());
    out.insert(out.end(), found.begin(), found.end());
#else
    DIR* d = opendir(dir.c_str());
    if (!d) return;
    std::vector<std::string> found;
    while (dirent* e = readdir(d)) {
        const std::string n = e->d_name;
        if (n.empty() || n[0] == '.') continue;
        const std::string p = dir + (dir.back() == '/' || dir.back() == '\\' ? "" : "/") + n;
        if (is_dir(p)) collect_audio(p, found);
        else if (is_audio(n)) found.push_back(p);
    }
    closedir(d);
    std::sort(found.begin(), found.end());
    out.insert(out.end(), found.begin(), found.end());
#endif
}

static ma_uint32 le32(const unsigned char* b) {
    return b[0] | (b[1] << 8) | (b[2] << 16) | (static_cast<ma_uint32>(b[3]) << 24);
}

typedef std::map<std::string, std::string> Tags;     // upper-case key -> value

static std::string trim(const std::string& s);

// ---- MP3: ID3v2 (start of file) and ID3v1 (last 128 bytes) ----
static void put_utf8(std::string& out, unsigned cp) {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
}

// An ID3 text frame body (encoding byte + text) as UTF-8; stops at the first NUL.
static std::string id3_text(const unsigned char* b, size_t n) {
    std::string out;
    if (n < 1) return out;
    const int enc = b[0];
    ++b; --n;
    if (enc == 0) {                                // Latin-1
        for (size_t i = 0; i < n && b[i]; ++i) put_utf8(out, b[i]);
    } else if (enc == 3) {                         // UTF-8
        for (size_t i = 0; i < n && b[i]; ++i) out += static_cast<char>(b[i]);
    } else {                                       // UTF-16, with BOM (1) or big-endian (2)
        bool be = enc == 2;
        size_t i = 0;
        if (enc == 1 && n >= 2) {
            if (b[0] == 0xFE && b[1] == 0xFF) { be = true;  i = 2; }
            else if (b[0] == 0xFF && b[1] == 0xFE) { be = false; i = 2; }
        }
        for (; i + 1 < n; i += 2) {
            unsigned u = be ? (b[i] << 8 | b[i + 1]) : (b[i + 1] << 8 | b[i]);
            if (!u) break;
            if (u >= 0xD800 && u < 0xDC00 && i + 3 < n) {          // surrogate pair
                const unsigned lo = be ? (b[i + 2] << 8 | b[i + 3]) : (b[i + 3] << 8 | b[i + 2]);
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
            put_utf8(out, u);
        }
    }
    return trim(out);
}

static ma_uint32 syncsafe(const unsigned char* b) {
    return (b[0] & 0x7F) << 21 | (b[1] & 0x7F) << 14 | (b[2] & 0x7F) << 7 | (b[3] & 0x7F);
}

static Tags read_id3_tags(FILE* f) {
    Tags tags;
    // Frame ids for v2.3/2.4 and v2.2 -> our tag names.
    static const char* const MAP[][3] = {
        {"TIT2", "TT2", "TITLE"}, {"TPE1", "TP1", "ARTIST"}, {"TPE2", "TP2", "ALBUMARTIST"},
        {"TALB", "TAL", "ALBUM"}, {"TDRC", "TYE", "DATE"},   {"TYER", "TYE", "DATE"},
        {"TRCK", "TRK", "TRACKNUMBER"},
    };
    unsigned char h[10];
    std::fseek(f, 0, SEEK_SET);
    if (std::fread(h, 1, 10, f) == 10 && std::memcmp(h, "ID3", 3) == 0 && h[3] >= 2 && h[3] <= 4) {
        const int ver = h[3];
        std::vector<unsigned char> b(syncsafe(&h[6]));
        b.resize(std::fread(b.data(), 1, b.size(), f));
        size_t p = 0;
        if (ver >= 3 && (h[5] & 0x40) && b.size() >= 4)             // skip extended header
            p = ver == 4 ? syncsafe(&b[0]) : 4 + (b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]);
        const size_t idLen = ver == 2 ? 3 : 4, hdrLen = ver == 2 ? 6 : 10;
        while (p + hdrLen <= b.size() && b[p]) {
            const std::string id(reinterpret_cast<const char*>(&b[p]), idLen);
            size_t len = ver == 2 ? (b[p + 3] << 16 | b[p + 4] << 8 | b[p + 5])
                       : ver == 4 ? syncsafe(&b[p + 4])
                       : (static_cast<size_t>(b[p + 4]) << 24 | b[p + 5] << 16 | b[p + 6] << 8 | b[p + 7]);
            p += hdrLen;
            if (p + len > b.size()) break;
            for (const auto& m : MAP)
                if (id == m[ver == 2 ? 1 : 0] && !tags.count(m[2])) {
                    const std::string v = id3_text(&b[p], len);
                    if (!v.empty()) tags[m[2]] = v;
                }
            p += len;
        }
    }
    // ID3v1 fallback for anything still missing.
    if (std::fseek(f, -128, SEEK_END) == 0 && std::fread(h, 1, 3, f) == 3 && std::memcmp(h, "TAG", 3) == 0) {
        unsigned char v1[125];
        if (std::fread(v1, 1, 125, f) == 125) {
            auto field = [&](size_t off, size_t n) {
                std::string out;
                for (size_t i = 0; i < n && v1[off + i]; ++i) put_utf8(out, v1[off + i]);
                return trim(out);
            };
            const std::pair<const char*, std::string> v[] = {
                {"TITLE", field(0, 30)}, {"ARTIST", field(30, 30)}, {"ALBUM", field(60, 30)}, {"DATE", field(90, 4)}};
            for (const auto& kv : v)
                if (!kv.second.empty() && !tags.count(kv.first)) tags[kv.first] = kv.second;
        }
    }
    return tags;
}

static Tags read_flac_tags(FILE* f);

// TITLE, ARTIST, ALBUM, DATE ... from a FLAC or MP3 file.
static Tags read_tags(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return Tags();
    unsigned char magic[4] = {0};
    const bool flac = std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "fLaC", 4) == 0;
    const Tags tags = flac ? read_flac_tags(f) : read_id3_tags(f);
    std::fclose(f);
    return tags;
}

// Vorbis comments (TITLE, ARTIST, ALBUM, ...) from the FLAC metadata.
// `f` is positioned just after the "fLaC" marker.
static Tags read_flac_tags(FILE* f) {
    Tags tags;
    unsigned char hdr[4];
    {
        while (std::fread(hdr, 1, 4, f) == 4) {
            const bool   last = (hdr[0] & 0x80) != 0;
            const int    type = hdr[0] & 0x7F;
            const size_t len  = (size_t(hdr[1]) << 16) | (size_t(hdr[2]) << 8) | hdr[3];
            if (type != 4) {                       // 4 = VORBIS_COMMENT
                if (last || std::fseek(f, static_cast<long>(len), SEEK_CUR) != 0) break;
                continue;
            }
            std::vector<unsigned char> b(len);
            if (std::fread(b.data(), 1, len, f) != len || len < 8) break;
            size_t p = 4 + le32(&b[0]);            // skip vendor string
            if (p + 4 > len) break;
            const ma_uint32 count = le32(&b[p]);
            p += 4;
            for (ma_uint32 i = 0; i < count && p + 4 <= len; ++i) {
                const ma_uint32 n = le32(&b[p]);
                p += 4;
                if (p + n > len) break;
                const std::string kv(reinterpret_cast<const char*>(&b[p]), n);
                p += n;
                const size_t eq = kv.find('=');
                if (eq == std::string::npos) continue;
                std::string key = kv.substr(0, eq);
                for (char& c : key) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                if (!tags.count(key)) tags[key] = kv.substr(eq + 1);
            }
            break;
        }
    }
    return tags;
}

// ---------- user config (key = value file) ----------
struct Config {
    std::string accent    = "\x1b[38;5;80m";       // escape built from `accent`
    unsigned char accentRGB[3] = {95, 215, 215};   // same colour as plain RGB (window version)
    int         waveWidth = 20;
    int         listRows  = 5;
    int         barMax    = 44;
    bool        cover     = true;
    bool        coverRight = false;
    bool        showKeys  = true;
    double      seekSec   = 5.0;                  // ← / → jump this many seconds
    std::string coverFile = "cover.ppm";
    std::vector<std::string> waveChars = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    std::string noteIcon = "", playIcon = ">", pauseIcon = "=";
    std::string barDone = "━", barLeft = "─", barHead = "●", sep = "·";
};
static Config g_cfg;

static const char* const DEFAULT_CONFIG =
    "# player config. Lines are  key = value ; '#' starts a comment.\n"
    "# Found via --config FILE, $PLAYER_CONFIG, ./player.conf,\n"
    "# player.conf next to the program, or ~/.config/player/config.\n"
    "\n"
    "# Colour: a 256-colour number (0-255) or #rrggbb\n"
    "accent      = 80\n"
    "\n"
    "# Cover art\n"
    "cover       = on          # on / off\n"
    "cover_side  = left        # left / right\n"
    "cover_file  = cover.ppm   # file looked for in each album folder\n"
    "\n"
    "# Layout\n"
    "list_rows   = 5           # songs shown around the current one\n"
    "wave_width  = 20\n"
    "bar_width   = 44          # max width of the progress bar\n"
    "show_keys   = on          # key help line at the bottom\n"
    "seek_seconds = 5          # left/right arrow jump\n"
    "\n"
    "# Symbols (any text)\n"
    "wave_chars  = ▁▂▃▄▅▆▇█    # low to high, any number of characters\n"
    "note_icon   =             # empty = no icon before the title\n"
    "play_icon   = >\n"
    "pause_icon  = =\n"
    "bar_done    = ━\n"
    "bar_left    = ─\n"
    "bar_head    = ●\n"
    "separator   = ·\n";

static std::string trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return "";
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

static std::vector<std::string> utf8_chars(const std::string& s) {
    std::vector<std::string> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = s[i];
        const size_t n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        out.push_back(s.substr(i, n));
        i += n;
    }
    return out;
}

static bool load_config(const std::string& path) {
    FILE* f = std::fopen(path.c_str(), "r");
    if (!f) return false;
    char buf[1024];
    int lineNo = 0;
    while (std::fgets(buf, sizeof buf, f)) {
        ++lineNo;
        std::string line = buf;
        // Comment: '#' at the start of a line, or " # " later on (so #rrggbb colours survive).
        if (!line.empty() && trim(line)[0] == '#') continue;
        for (size_t h = line.find('#'); h != std::string::npos; h = line.find('#', h + 1))
            if (h > 0 && std::isspace(static_cast<unsigned char>(line[h - 1])) &&
                (h + 1 >= line.size() || std::isspace(static_cast<unsigned char>(line[h + 1])))) {
                line.resize(h);
                break;
            }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        const bool on = v == "on" || v == "yes" || v == "true" || v == "1";
        auto num = [&](int lo, int hi, int def) { int n = std::atoi(v.c_str()); return n >= lo && n <= hi ? n : def; };

        if (k == "accent") {
            unsigned r, g, b;
            if (v.size() == 7 && v[0] == '#' && std::sscanf(v.c_str() + 1, "%2x%2x%2x", &r, &g, &b) == 3) {
                g_cfg.accent = "\x1b[38;2;" + std::to_string(r) + ";" + std::to_string(g) + ";" + std::to_string(b) + "m";
            } else {
                const int n = num(0, 255, 80);
                g_cfg.accent = "\x1b[38;5;" + std::to_string(n) + "m";
                // xterm 256-colour palette -> RGB
                static const unsigned char base[16][3] = {{0,0,0},{205,0,0},{0,205,0},{205,205,0},{0,0,238},{205,0,205},{0,205,205},{229,229,229},
                                                          {127,127,127},{255,0,0},{0,255,0},{255,255,0},{92,92,255},{255,0,255},{0,255,255},{255,255,255}};
                static const int steps[6] = {0, 95, 135, 175, 215, 255};
                if (n < 16)       { r = base[n][0]; g = base[n][1]; b = base[n][2]; }
                else if (n < 232) { r = steps[(n - 16) / 36]; g = steps[(n - 16) / 6 % 6]; b = steps[(n - 16) % 6]; }
                else              { r = g = b = 8 + (n - 232) * 10; }
            }
            g_cfg.accentRGB[0] = static_cast<unsigned char>(r);
            g_cfg.accentRGB[1] = static_cast<unsigned char>(g);
            g_cfg.accentRGB[2] = static_cast<unsigned char>(b);
        }
        else if (k == "cover")       g_cfg.cover      = on;
        else if (k == "cover_side")  g_cfg.coverRight = v == "right";
        else if (k == "cover_file")  { if (!v.empty()) g_cfg.coverFile = v; }
        else if (k == "list_rows")   g_cfg.listRows   = num(1, 30, 5);
        else if (k == "wave_width")  g_cfg.waveWidth  = num(0, 80, 20);
        else if (k == "bar_width")   g_cfg.barMax     = num(10, 300, 44);
        else if (k == "show_keys")   g_cfg.showKeys   = on;
        else if (k == "seek_seconds") { const double d = std::atof(v.c_str()); if (d > 0 && d <= 600) g_cfg.seekSec = d; }
        else if (k == "wave_chars")  { if (!v.empty()) g_cfg.waveChars = utf8_chars(v); }
        else if (k == "note_icon")   g_cfg.noteIcon   = v;
        else if (k == "play_icon")   g_cfg.playIcon   = v;
        else if (k == "pause_icon")  g_cfg.pauseIcon  = v;
        else if (k == "bar_done")    g_cfg.barDone    = v;
        else if (k == "bar_left")    g_cfg.barLeft    = v;
        else if (k == "bar_head")    g_cfg.barHead    = v;
        else if (k == "separator")   g_cfg.sep        = v;
        else std::fprintf(stderr, "%s:%d: unknown setting '%s' (ignored)\n", path.c_str(), lineNo, k.c_str());
    }
    std::fclose(f);
    return true;
}

// A pixel-art cover: cover.ppm (binary P6) in the file's folder.
struct Cover {
    int w = 0, h = 0;
    std::vector<unsigned char> rgb;              // w*h*3
    bool ok() const { return w > 0 && h > 0; }
};

static Cover load_cover(const std::string& trackPath) {
    Cover c;
    const size_t slash = trackPath.find_last_of("/\\");
    const std::string dir = slash == std::string::npos ? "." : trackPath.substr(0, slash);
    FILE* f = std::fopen((dir + "/" + g_cfg.coverFile).c_str(), "rb");
    if (!f) return c;
    int w = 0, h = 0, maxv = 0;
    if (std::fscanf(f, "P6 %d %d %d", &w, &h, &maxv) == 3 && maxv == 255 &&
        w > 0 && h > 0 && w <= 64 && h <= 64 && std::fgetc(f) != EOF) {
        c.rgb.resize(static_cast<size_t>(w) * h * 3);
        if (std::fread(c.rgb.data(), 1, c.rgb.size(), f) == c.rgb.size()) { c.w = w; c.h = h; }
        else c.rgb.clear();
    }
    std::fclose(f);
    return c;
}

// Two pixel rows per text row using "▀" (top = foreground, bottom = background).
static std::vector<std::string> render_cover(const Cover& c) {
    std::vector<std::string> rows;
    char buf[64];
    for (int y = 0; y < c.h; y += 2) {
        std::string s;
        for (int x = 0; x < c.w; ++x) {
            const unsigned char* t = &c.rgb[(static_cast<size_t>(y) * c.w + x) * 3];
            const unsigned char* b = y + 1 < c.h ? &c.rgb[(static_cast<size_t>(y + 1) * c.w + x) * 3] : t;
            std::snprintf(buf, sizeof buf, "\x1b[38;2;%d;%d;%dm\x1b[48;2;%d;%d;%dm\xe2\x96\x80",
                          t[0], t[1], t[2], b[0], b[1], b[2]);
            s += buf;
        }
        rows.push_back(s + "\x1b[0m");
    }
    return rows;
}

// --scan: label every file (several at once) and print what was found.
static int scan_files(const std::vector<std::string>& files) {
    std::vector<Marks>  results(files.size());
    std::atomic<size_t> nextFile{0};

    const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
    std::vector<std::thread> workers;
    for (size_t t = 0; t < std::min<size_t>(hw, files.size()); ++t)
        workers.emplace_back([&] {
            for (size_t i; (i = nextFile.fetch_add(1)) < files.size(); )
                results[i] = get_marks(files[i], true);
        });
    for (std::thread& w : workers) w.join();

    for (size_t i = 0; i < files.size(); ++i) {
        Marks shown;                               // read back what is really in the file
        const bool stored = read_stored_marks(files[i], shown);
        if (!stored) shown = results[i];

        ma_uint32  rate = 44100;
        ma_decoder d;
        if (ma_decoder_init_file(files[i].c_str(), nullptr, &d) == MA_SUCCESS) {
            rate = d.outputSampleRate;
            ma_decoder_uninit(&d);
        }

        std::printf("%s: %zu song(s)%s\n", files[i].c_str(), shown.size(),
                    stored ? "" : "  (markers NOT stored in the file)");
        for (size_t k = 0; k < shown.size(); ++k)
            std::printf("  %2zu  %6s\n", k + 1, mmss(shown[k], rate).c_str());
    }
    return 0;
}

// ---------- playback ----------
static bool open_and_play(Player& p, const std::string& path) {
    if (ma_decoder_init_file(path.c_str(), nullptr, &p.decoder) != MA_SUCCESS) {
        std::fprintf(stderr, "Failed to open: %s\n", path.c_str());
        return false;
    }

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format   = p.decoder.outputFormat;
    cfg.playback.channels = p.decoder.outputChannels;
    cfg.sampleRate        = p.decoder.outputSampleRate;
    cfg.dataCallback      = data_callback;
    // Bigger buffer than the low-latency default: latency doesn't matter for a
    // music player, and it rides out hiccups (disk, CPU spikes) without dropouts.
    cfg.performanceProfile = ma_performance_profile_conservative;
    cfg.periodSizeInMilliseconds = 50;
    cfg.pUserData         = &p;

    if (ma_device_init(nullptr, &cfg, &p.device) != MA_SUCCESS) {
        std::fprintf(stderr, "Failed to init device\n");
        ma_decoder_uninit(&p.decoder);
        return false;
    }

    p.pos.store(0);
    p.level.store(0.0f);
    p.finished.store(false);
    p.paused.store(false);

    // Mark the device live before starting it: some backends can invoke the
    // callback immediately from ma_device_start(). A very short track could
    // otherwise finish before deviceReady is set, and close_player() would
    // skip uninitializing the device before its Player storage is freed.
    p.deviceReady.store(true);
    if (ma_device_start(&p.device) != MA_SUCCESS) {
        p.deviceReady.store(false);
        std::fprintf(stderr, "Failed to start device\n");
        ma_device_uninit(&p.device);
        ma_decoder_uninit(&p.decoder);
        return false;
    }
    return true;
}

static void close_player(Player& p) {
    if (p.deviceReady.exchange(false)) {
        ma_device_uninit(&p.device);
        ma_decoder_uninit(&p.decoder);
    }
}

static void seek_to(Player& p, ma_uint64 frame) {
    std::lock_guard<std::mutex> lk(p.mtx);
    if (ma_decoder_seek_to_pcm_frame(&p.decoder, frame) == MA_SUCCESS)
        p.pos.store(frame);
}

// ---------- terminal ----------
static std::atomic<bool> g_quit{false};
static bool              g_fancy    = false;      // stdout is a terminal: redraw in place

#ifndef _WIN32
static void on_signal(int) { g_quit.store(true); }
#endif

static void sleep_ms(int ms) {
#ifdef _WIN32
    Sleep(static_cast<DWORD>(ms));
#else
    usleep(static_cast<useconds_t>(ms) * 1000);
#endif
}

static int terminal_columns() {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &info))
        return info.srWindow.Right - info.srWindow.Left + 1;
#else
    struct winsize ws {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) return ws.ws_col;
#endif
    return 80;
}

#ifdef _WIN32
static HANDLE g_in = nullptr, g_out = nullptr;
static DWORD  g_savedIn = 0, g_savedOut = 0;
static UINT   g_savedCP = 0;
static bool   g_consoleIn = false;
static bool   g_restore   = false;

static void restore_terminal() {
    if (g_fancy) { std::fputs("\x1b[?7h\x1b[?25h\x1b[0m\x1b[23;0t", stdout); std::fflush(stdout); g_fancy = false; }
    if (g_restore) {
        SetConsoleMode(g_out, g_savedOut);
        if (g_consoleIn) SetConsoleMode(g_in, g_savedIn);
        SetConsoleOutputCP(g_savedCP);
        g_restore = false;
    }
}

static BOOL WINAPI on_ctrl(DWORD) { g_quit.store(true); return TRUE; }

static void setup_terminal() {
    g_out = GetStdHandle(STD_OUTPUT_HANDLE);
    g_in  = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    // A real console: turn on UTF-8 and escape-code (colour / cursor) support.
    if (GetConsoleMode(g_out, &mode)) {
        g_savedOut = mode;
        g_savedCP  = GetConsoleOutputCP();
        SetConsoleOutputCP(CP_UTF8);
        g_fancy = SetConsoleMode(g_out, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING |
                                       ENABLE_PROCESSED_OUTPUT) != 0;
        g_restore = true;
    }
    g_consoleIn = GetConsoleMode(g_in, &g_savedIn) != 0;
    SetConsoleCtrlHandler(on_ctrl, TRUE);
    std::atexit(restore_terminal);
    if (g_fancy) std::fputs("\x1b[22;0t\x1b[?25l", stdout);
}

// Wait up to one animation frame for a key. Returns 0 if none.
static char poll_key() {
    if (!g_consoleIn) { sleep_ms(FRAME_MS); return 0; }
    for (int waited = 0; waited < FRAME_MS; waited += 10) {
        if (_kbhit()) {
            const int c = _getch();
            if (c == 0 || c == 0xE0) {             // arrow and other special keys come in two parts
                const int k = _getch();
                return k == 77 ? '>' : k == 75 ? '<' : 0;
            }
            return static_cast<char>(c);
        }
        sleep_ms(10);
    }
    return 0;
}
#else
static struct termios g_savedTerm;
static bool           g_rawInput = false;

static void restore_terminal() {
    if (g_rawInput) { tcsetattr(STDIN_FILENO, TCSANOW, &g_savedTerm); g_rawInput = false; }
    if (g_fancy)    { std::fputs("\x1b[?7h\x1b[?25h\x1b[0m\x1b[23;0t", stdout); std::fflush(stdout); g_fancy = false; }
}

static void setup_terminal() {
    g_fancy = isatty(STDOUT_FILENO);
    std::signal(SIGINT,  on_signal);
    std::signal(SIGTERM, on_signal);
    std::atexit(restore_terminal);

    // Single key presses, no Enter and no echo, so typing never adds lines.
    if (isatty(STDIN_FILENO) && tcgetattr(STDIN_FILENO, &g_savedTerm) == 0) {
        struct termios raw = g_savedTerm;
        raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
        raw.c_cc[VMIN]  = 0;
        raw.c_cc[VTIME] = 0;
        g_rawInput = tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0;
    }
    if (g_fancy) std::fputs("\x1b[22;0t\x1b[?25l", stdout);  // save the tab title; hide the cursor
}

// Wait up to one animation frame for a key. Returns 0 if none.
static char poll_key() {
    static bool stdinOpen = true;
    if (!stdinOpen) { sleep_ms(FRAME_MS); return 0; }

    struct pollfd pfd { STDIN_FILENO, POLLIN, 0 };
    if (poll(&pfd, 1, FRAME_MS) <= 0) return 0;

    char c = 0;
    if (read(STDIN_FILENO, &c, 1) == 1) {
        if (c != '\x1b') return c;
        // Arrow keys arrive as ESC [ C (right) / ESC [ D (left).
        char seq[2] = {0, 0};
        struct pollfd more { STDIN_FILENO, POLLIN, 0 };
        if (poll(&more, 1, 10) > 0 && read(STDIN_FILENO, &seq[0], 1) == 1 && seq[0] == '[' &&
            poll(&more, 1, 10) > 0 && read(STDIN_FILENO, &seq[1], 1) == 1) {
            if (seq[1] == 'C') return '>';
            if (seq[1] == 'D') return '<';
        }
        return 0;
    }
    stdinOpen = false;                             // EOF (piped input ran out): keep playing
    return 0;
}
#endif

// Send UTF-8 text to the terminal. On Windows the C runtime re-encodes bytes
// with the console's code page (mangling "▀" etc.), so write UTF-16 directly.
static void term_write(const std::string& text) {
#ifdef _WIN32
    DWORD mode;
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    if (GetConsoleMode(out, &mode)) {
        std::fflush(stdout);
        const int n = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), &w[0], n);
        DWORD written = 0;
        WriteConsoleW(out, w.data(), static_cast<DWORD>(w.size()), &written, nullptr);
        return;
    }
#endif
    std::fwrite(text.data(), 1, text.size(), stdout);
    std::fflush(stdout);
}

// ---------- display ----------
#define C_RESET  "\x1b[0m"
#define C_DIM    "\x1b[2m"
#define C_BOLD   "\x1b[1m"
#define C_ACCENT g_cfg.accent

static const int MIN_LINES  = 14;                  // the display occupies at least this many rows

struct View {
    int    drawn = 0;                              // rows of the block already on screen
    double phase = 0.0;                            // wave animation position
    float  level = 0.0f;                           // smoothed loudness driving the wave
    std::vector<std::string> shown;                // what each on-screen row currently holds
};

// Draw the block over the previous one in a single write, rewriting only the
// rows that changed (the cover is sent once, not 20 times a second). The
// terminal is asked to show the frame atomically, so nothing tears or flickers.
static void paint(View& v, const std::vector<std::string>& lines) {
    const int rows = std::max({MIN_LINES, static_cast<int>(lines.size()), v.drawn});
    std::string out = "\x1b[?2026h\x1b[?7l";     // begin synchronized frame; clip, don't wrap
    if (v.drawn) out += "\x1b[" + std::to_string(v.drawn) + "A";
    v.shown.resize(static_cast<size_t>(rows));
    for (int i = 0; i < rows; ++i) {
        const std::string& want = i < static_cast<int>(lines.size()) ? lines[i] : std::string();
        if (i >= v.drawn || v.shown[i] != want) {
            out += "\r" + want + C_RESET "\x1b[K";   // write, then clear whatever is left on the row
            v.shown[i] = want;
        }
        out += "\n";
    }
    out += "\x1b[?7h\x1b[?2026l";
    term_write(out);
    v.drawn = rows;
}

// What we know about the file being played.
struct TrackInfo {
    std::string title, artist, album, date;
    std::vector<std::string> cover;                // rendered rows, empty = no cover
    int coverW = 0;                                // in columns
};

static TrackInfo track_info(const std::string& path) {
    TrackInfo t;
    const Tags tags = read_tags(path);
    auto get = [&](const char* k) { Tags::const_iterator it = tags.find(k); return it == tags.end() ? std::string() : it->second; };
    t.title  = get("TITLE");
    t.artist = get("ARTIST");
    if (t.artist.empty()) t.artist = get("ALBUMARTIST");
    t.album  = get("ALBUM");
    t.date   = get("DATE");
    if (t.title.empty()) t.title = display_name(path);
    const Cover c = load_cover(path);
    if (g_cfg.cover && c.ok()) { t.cover = render_cover(c); t.coverW = c.w; }
    return t;
}

// Visible width of a line: skips escape codes, counts each UTF-8 character as one column.
static int visible_width(const std::string& s) {
    int w = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '\x1b') { while (i < s.size() && !std::isalpha(static_cast<unsigned char>(s[i]))) ++i; continue; }
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) ++w;
    }
    return w;
}

// Header: big title, then artist · album · year · file n/N.
static void header_lines(std::vector<std::string>& lines, const TrackInfo& t,
                         size_t fileIdx, size_t fileCount) {
    const std::string sep = std::string(C_DIM) + "  " + g_cfg.sep + "  ";
    lines.push_back("");
    const std::string icon = g_cfg.noteIcon.empty() ? "" : C_ACCENT + g_cfg.noteIcon + C_RESET "  ";
    lines.push_back("  " + icon + C_BOLD + t.title + C_RESET);
    std::string sub = "     " + C_ACCENT + (t.artist.empty() ? std::string("Unknown artist") : t.artist) + C_RESET;
    if (!t.album.empty()) sub += sep + C_RESET + t.album;
    if (!t.date.empty())  sub += sep + t.date + C_RESET;
    sub += sep + "file " + std::to_string(fileIdx + 1) + "/" + std::to_string(fileCount);
    lines.push_back(sub);
    lines.push_back("");
}

// Put the cover beside the text block (left or right, per config).
static std::vector<std::string> with_cover(const TrackInfo& t, const std::vector<std::string>& text) {
    if (t.cover.empty()) return text;
    std::vector<std::string> out;
    const size_t top = 1;                          // one blank row above the cover
    const size_t n = std::max(text.size(), t.cover.size() + top + 1);
    const std::string blank(static_cast<size_t>(t.coverW), ' ');

    int textW = 0;                                 // right side: line the cover up after the widest row
    if (g_cfg.coverRight)
        for (const std::string& s : text) textW = std::max(textW, visible_width(s));

    for (size_t i = 0; i < n; ++i) {
        const std::string& art = (i >= top && i - top < t.cover.size()) ? t.cover[i - top] : blank;
        const std::string  txt = i < text.size() ? text[i] : "";
        std::string row;
        if (g_cfg.coverRight)
            row = txt + C_RESET + std::string(static_cast<size_t>(textW - visible_width(txt) + 3), ' ') + art;
        else
            row = "  " + art + C_RESET " " + txt;
        out.push_back(row);
    }
    return out;
}

// A travelling wave whose height follows how loud the music is right now.
static std::string wave(const View& v, bool paused) {
    const std::vector<std::string>& chars = g_cfg.waveChars;
    const int levels = static_cast<int>(chars.size());
    const double amp = paused ? 0.0 : 0.25 + 0.75 * std::sqrt(static_cast<double>(v.level));

    std::string s;
    for (int x = 0; x < g_cfg.waveWidth; ++x) {
        const double shape = 0.5 + 0.34 * std::sin(x * 0.62 - v.phase)
                                 + 0.16 * std::sin(x * 1.70 + v.phase * 1.3);
        s += chars[std::min(levels - 1, std::max(0, static_cast<int>(shape * amp * levels)))];
    }
    return s;
}

struct NowPlaying {
    const TrackInfo* info;
    size_t fileIdx, fileCount;
    const Marks* marks;
    ma_uint64 pos, total;
    ma_uint32 rate;
    bool paused;
    const std::vector<std::string>* titles;        // folder mode: one song per file; else null
};

static void draw_player(View& v, const NowPlaying& np) {
    const Marks&    marks = *np.marks;
    const ma_uint64 total = std::max(np.total, np.pos);

    std::vector<std::string> lines;
    header_lines(lines, *np.info, np.fileIdx, np.fileCount);

    // Folder mode lists the files; otherwise the songs found inside this file.
    const size_t n   = np.titles ? np.titles->size() : marks.size();
    const size_t cur = np.titles ? np.fileIdx : current_song(marks, np.pos);

    const size_t rows  = static_cast<size_t>(g_cfg.listRows);
    const size_t above = (rows - 1) / 2;
    const size_t first = n <= rows ? 0 : std::min(cur > above ? cur - above : 0, n - rows);
    for (size_t row = 0; row < rows; ++row) {
        const size_t i = first + row;
        if (i >= n) { lines.push_back(""); continue; }

        if (np.titles) {
            char num[32];
            std::snprintf(num, sizeof num, "%3zu   ", i + 1);
            const std::string& t = (*np.titles)[i];
            if (i == cur)
                lines.push_back("  " + C_ACCENT + C_BOLD + (np.paused ? g_cfg.pauseIcon : g_cfg.playIcon) + " " +
                                num + t + "   " + C_RESET + C_ACCENT + wave(v, np.paused) + C_RESET "   " +
                                mmss(np.pos, np.rate) + C_DIM " / " + mmss(total, np.rate));
            else
                lines.push_back(std::string("    " C_DIM) + num + t);
            continue;
        }

        const ma_uint64 start = marks[i];
        const ma_uint64 end   = i + 1 < n ? marks[i + 1] : total;
        const ma_uint64 len   = end > start ? end - start : 0;

        char label[64];
        std::snprintf(label, sizeof label, "%3zu   %6s   ", i + 1, mmss(start, np.rate).c_str());

        if (i == cur) {
            const ma_uint64 into = np.pos > start ? np.pos - start : 0;
            lines.push_back("  " + C_ACCENT + C_BOLD + (np.paused ? g_cfg.pauseIcon : g_cfg.playIcon) + " " +
                            label + C_RESET + C_ACCENT + wave(v, np.paused) + C_RESET "   " +
                            mmss(into, np.rate) + C_DIM " / " + mmss(len, np.rate));
        } else {
            lines.push_back(std::string("    " C_DIM) + label + mmss(len, np.rate));
        }
    }
    lines.push_back("");

    // Progress through the whole file.
    const int cols = terminal_columns();
    const int barW = std::max(10, std::min(g_cfg.barMax, cols - 22 - (np.info->coverW ? np.info->coverW + 3 : 0)));
    const int head = total ? static_cast<int>(np.pos * static_cast<ma_uint64>(barW - 1) / total) : 0;

    std::string bar = "  " + C_ACCENT;
    for (int x = 0; x < barW; ++x) {
        if (x == head)     bar += g_cfg.barHead + C_RESET C_DIM;
        else if (x < head) bar += g_cfg.barDone;
        else               bar += g_cfg.barLeft;
    }
    bar += C_RESET "  " + mmss(np.pos, np.rate) + C_DIM " / " + mmss(total, np.rate);
    lines.push_back(bar);
    if (g_cfg.showKeys) {
        lines.push_back("");
        lines.push_back("  " C_DIM "space pause   s skip   b back   n next file   q quit");
    }

    paint(v, with_cover(*np.info, lines));
}

static void draw_message(View& v, const TrackInfo& info, size_t fileIdx, size_t fileCount,
                         const std::string& message) {
    std::vector<std::string> lines;
    header_lines(lines, info, fileIdx, fileCount);
    lines.push_back("    " C_DIM + message);
    paint(v, with_cover(info, lines));
}

#ifdef _WIN32
// A small "Browse For Folder" pop-up (folder tree + OK / Cancel). "" if cancelled.
static std::string pick_folder(const wchar_t* title) {
    std::string result;
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    BROWSEINFOW bi {};
    bi.lpszTitle = title;
    bi.ulFlags   = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_NONEWFOLDERBUTTON;
    if (PIDLIST_ABSOLUTE pidl = SHBrowseForFolderW(&bi)) {
        wchar_t wide[MAX_PATH];
        if (SHGetPathFromIDListW(pidl, wide)) {
            // All Windows filesystem paths are carried as UTF-8 internally.
            const int n = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
            if (n > 1) {
                result.resize(static_cast<size_t>(n - 1));
                WideCharToMultiByte(CP_UTF8, 0, wide, -1, &result[0], n, nullptr, nullptr);
            }
        }
        CoTaskMemFree(pidl);
    }
    if (SUCCEEDED(init)) CoUninitialize();
    return result;
}

// Started with nothing to play (e.g. double-clicked): ask for a music folder
// until one with .flac / .mp3 files is chosen. False if the user cancels.
static bool ask_for_folder(std::vector<std::string>& paths) {
    while (true) {
        const std::string dir = pick_folder(L"Choose a folder with your music (.mp3 / .flac)");
        if (dir.empty()) return false;
        std::vector<std::string> found;
        collect_audio(dir, found);
        if (!found.empty()) { paths.push_back(dir); return true; }
        MessageBoxW(nullptr, L"That folder has no .mp3 or .flac files (subfolders are searched too).\n"
                    L"Please choose another folder.", L"player", MB_OK | MB_ICONINFORMATION);
    }
}
#endif

// ---------- main ----------
// (The Windows window version, gui_win32.cpp, includes this file with
//  PLAYER_GUI defined and brings its own WinMain instead.)
#ifndef PLAYER_GUI
int main(int argc, char** argv) {
    // Folder this program lives in (for player.conf / pixelate.py next to it).
    std::string exeDir = ".";
    {
        char buf[4096];
#ifdef _WIN32
        const DWORD n = GetModuleFileNameA(nullptr, buf, sizeof buf - 1);
#else
        const ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
#endif
        if (n > 0) { buf[n] = 0; exeDir = buf; exeDir = exeDir.substr(0, exeDir.find_last_of("/\\")); }
    }

    const char* usage =
        "Usage: %s [options] file.flac|file.mp3|folder ...\n"
        "\n"
        "  --scan PATH...            (re)find songs in files/folders and exit\n"
        "  --config FILE             use this config file (any name you like)\n"
        "  --write-config FILE       write a config with all settings + defaults, then exit\n"
        "  --cover [IMAGE] [ALBUM] [pixelate options]\n"
        "                            make an album cover (runs pixelate.py; see --cover --help)\n"
        "  -h, --help                this help\n";

    bool scanOnly = false;
    std::string configPath;
    std::vector<std::string> paths;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "-h" || a == "--help") { std::printf(usage, argv[0]); return 0; }
        else if (a == "--scan") scanOnly = true;
        else if (a == "--config" || a == "--write-config") {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a file name\n", a.c_str()); return 1; }
            const std::string file = argv[++i];
            if (a == "--config") { configPath = file; continue; }
            if (FILE* existing = std::fopen(file.c_str(), "rb")) {
                std::fclose(existing);
                std::fprintf(stderr, "%s already exists, not overwriting it\n", file.c_str());
                return 1;
            }
            FILE* f = std::fopen(file.c_str(), "w");
            if (!f || std::fputs(DEFAULT_CONFIG, f) < 0) { std::fprintf(stderr, "Cannot write %s\n", file.c_str()); return 1; }
            std::fclose(f);
            std::printf("Wrote %s — edit it, then run with --config %s\n", file.c_str(), file.c_str());
            return 0;
        }
        else if (a == "--cover") {                 // hand everything after it to pixelate.py
#ifdef _WIN32
            const char* python = "python";
#else
            const char* python = "python3";
#endif
            std::vector<std::string> cmd = {python, exeDir + "/pixelate.py"};
            cmd.insert(cmd.end(), argv + i + 1, argv + argc);
            const int rc = run_program(cmd, false);
            if (rc < 0) std::fprintf(stderr, "Could not run %s (is Python installed?)\n", python);
            return rc < 0 ? 1 : rc;
        }
        else if (a.size() > 1 && a[0] == '-') { std::fprintf(stderr, "Unknown option %s\n", a.c_str()); return 1; }
        else paths.push_back(a);
    }

    // Config: explicit file, else the first of the usual places that exists.
    if (!configPath.empty()) {
        if (!load_config(configPath)) { std::fprintf(stderr, "Cannot read config %s\n", configPath.c_str()); return 1; }
    } else {
        std::vector<std::string> candidates;
        if (const char* env = std::getenv("PLAYER_CONFIG")) candidates.push_back(env);
        candidates.push_back("player.conf");
        candidates.push_back(exeDir + "/player.conf");
        if (const char* home = std::getenv("HOME")) candidates.push_back(std::string(home) + "/.config/player/config");
        for (const std::string& c : candidates) if (load_config(c)) break;
    }

#ifdef _WIN32
    if (paths.empty() && !scanOnly && !ask_for_folder(paths)) return 0;   // cancelled: just close
#endif
    if (paths.empty()) { std::fprintf(stderr, usage, argv[0]); return 1; }

    std::vector<std::string> files;
    bool folderMode = false;                       // a folder was given: every file is one song
    for (const std::string& p : paths) {
        if (is_dir(p)) { collect_audio(p, files); folderMode = true; }
        else           files.push_back(p);
    }
    if (files.empty()) { std::fprintf(stderr, "No .flac or .mp3 files found\n"); return 1; }
    if (scanOnly) return scan_files(files);

    std::vector<std::string> titles;
    if (folderMode)
        for (const std::string& f : files) {
            const Tags t = read_tags(f);
            Tags::const_iterator it = t.find("TITLE");
            titles.push_back(it != t.end() && !it->second.empty() ? it->second : display_name(f));
        }

    setup_terminal();

    View   view;
    std::string tabTitle;                          // last title sent to the terminal tab
    size_t idx      = 0;
    size_t failures = 0;                           // consecutive files that would not play

    while (!g_quit.load() && failures < files.size()) {
        const TrackInfo info = track_info(files[idx]);

        // Song markers are loaded (or detected and stored) before playback opens the file.
        // In folder mode each file is just one song: no markers, no scanning.
        Marks marks;
        if (folderMode) marks.push_back(0);
        else if (!read_stored_marks(files[idx], marks)) {
            if (g_fancy) draw_message(view, info, idx, files.size(), "finding songs…");
            else         std::printf("Finding songs in %s ...\n", files[idx].c_str());
            marks = get_marks(files[idx], false);
        }

        Player p;
        if (!open_and_play(p, files[idx])) {
            view.drawn = 0;                        // an error was printed; start a fresh block
            ++failures;
            idx = (idx + 1) % files.size();
            continue;
        }
        failures = 0;

        const ma_uint32 rate  = p.decoder.outputSampleRate;
        ma_uint64       total = 0;
        ma_decoder_get_length_in_pcm_frames(&p.decoder, &total);

        if (!g_fancy) std::printf("Playing: %s — %s (%s)\n", info.artist.c_str(),
                                  info.title.c_str(), info.album.c_str());

        size_t announced = static_cast<size_t>(-1);       // plain output: last song printed
        bool   advance   = false;
        bool   goBack    = false;                         // folder mode: go to the previous file
        const ma_uint64 seekStep = static_cast<ma_uint64>(rate * g_cfg.seekSec);
        while (!advance && !g_quit.load()) {
            if (p.finished.load()) break;

            const ma_uint64 pos    = p.pos.load(std::memory_order_relaxed);
            const bool      paused = p.paused.load();

            // Terminal tab title: "player - Song - Artist" (only sent when it changes).
            if (g_fancy) {
                std::string tab = "player - ";
                if (paused) tab += "(paused) ";
                tab += info.title;
                if (!folderMode && marks.size() > 1)
                    tab += " #" + std::to_string(current_song(marks, pos) + 1);
                if (!info.artist.empty()) tab += " - " + info.artist;
                if (tab != tabTitle) {
                    tabTitle = tab;
                    term_write("\x1b]0;" + tab + "\x07");
                }
            }

            if (g_fancy) {
                if (!paused) view.phase += 0.45;
                view.level = std::max(p.level.load(std::memory_order_relaxed), view.level * 0.80f);
                const NowPlaying np{&info, idx, files.size(), &marks, pos, total, rate, paused,
                                    folderMode ? &titles : nullptr};
                draw_player(view, np);
            } else {
                const size_t cur = current_song(marks, pos);
                if (cur != announced) {
                    std::printf("Song %zu/%zu  [%s]\n", cur + 1, marks.size(),
                                mmss(marks[cur], rate).c_str());
                    std::fflush(stdout);
                    announced = cur;
                }
            }

            switch (poll_key()) {
            case 'q':
                g_quit.store(true);
                break;
            case 'p': case ' ':
                p.paused.store(!paused);
                break;
            case 'n':
                advance = true;
                break;
            case '>': case 'l': case '.':                  // jump forward a few seconds
                if (total && pos + seekStep >= total) advance = true;
                else seek_to(p, pos + seekStep);
                break;
            case '<': case 'h': case ',':                  // jump back a few seconds
                seek_to(p, pos > seekStep ? pos - seekStep : 0);
                break;
            case 's':
                if (folderMode) { advance = true; break; }
                {
                Marks::const_iterator next = std::upper_bound(marks.begin(), marks.end(), pos);
                if (next == marks.end()) advance = true;          // last song: on to the next file
                else                     seek_to(p, *next);
                break;
            }
            case 'b': {
                if (folderMode) {                          // restart, or previous file if just started
                    if (pos < static_cast<ma_uint64>(rate * BACK_GRACE_SEC)) { goBack = true; advance = true; }
                    else seek_to(p, 0);
                    break;
                }
                const size_t cur = current_song(marks, pos);
                // Just started this song? Go to the previous one; otherwise restart it.
                const bool justStarted = pos < marks[cur] + static_cast<ma_uint64>(rate * BACK_GRACE_SEC);
                seek_to(p, marks[justStarted && cur > 0 ? cur - 1 : cur]);
                announced = static_cast<size_t>(-1);
                break;
            }
            default:
                break;
            }
        }

        close_player(p);
        idx = goBack ? (idx + files.size() - 1) % files.size() : (idx + 1) % files.size();
    }

    restore_terminal();
    return 0;
}
#endif  // PLAYER_GUI
