// gui_win32.cpp — the player as a Windows pop-up window (no console).
//
// All the audio / tags / song-finding / cover code comes from main.cpp; this
// file only adds the window: cover, title + artist, song list with a live
// wave, a clickable progress bar and buttons.
//
// Build with MinGW-w64 (from Linux):
//   x86_64-w64-mingw32-g++ -O2 -std=c++17 gui_win32.cpp -I/usr/include/miniaudio \
//       -o player.exe -static -mwindows -lole32 -lwinmm -luuid -lshell32 -lgdi32 -lwindowscodecs -lwininet -lcomctl32
// Usage: double-click (asks for a folder), or player.exe file|folder ...
// Keys:  space pause   left/right seek   s skip   b back   n next file   o open folder   esc quit

#define PLAYER_GUI
#include "../main.cpp"

#include <memory>
#include <fstream>
#include <map>
#include <sstream>
#include <shellapi.h>
#include <windowsx.h>
#include <wincodec.h>
#include <commdlg.h>
#include <commctrl.h>
#include <wininet.h>
#include <atomic>
#include <functional>
#include <algorithm>
#include <cctype>
#include <cwctype>
#include "dl_util.h"

namespace {

// ---------- look ----------
const int W = 700;                  // client size; height is computed from the layout
const int PAD = 18;
const int COVER = 210;              // cover box (square)
const int ROW_H = 24;
COLORREF BG = RGB(40, 40, 40), PANEL = RGB(60, 56, 54), TEXT = RGB(235, 219, 178),
         DIM = RGB(168, 153, 132), ACCENT = RGB(250, 189, 47);
COLORREF accent() { return ACCENT; }

bool parse_color(const std::string& value, COLORREF& out) {
    unsigned r, g, b;
    if (value.size() != 7 || value[0] != '#' ||
        std::sscanf(value.c_str() + 1, "%2x%2x%2x", &r, &g, &b) != 3) return false;
    out = RGB(r, g, b); return true;
}
void load_colors(const std::string& path) {
    std::ifstream in(path); std::string line;
    while (std::getline(in, line)) {
        const size_t first = line.find_first_not_of(" \t\r");
        if (first == std::string::npos || line[first] == '#') continue;
        const auto eq = line.find('='); if (eq == std::string::npos) continue;
        auto key = line.substr(0, eq), value = line.substr(eq + 1);
        auto trim = [](std::string& s) { const auto a=s.find_first_not_of(" \t\r"); if(a==std::string::npos){s.clear();return;} s=s.substr(a,s.find_last_not_of(" \t\r")-a+1); };
        trim(key); trim(value);
        const auto end = value.find_first_of(" \t"); if (end != std::string::npos) value.resize(end);
        if (key == "background") parse_color(value, BG);
        else if (key == "panel") parse_color(value, PANEL);
        else if (key == "text") parse_color(value, TEXT);
        else if (key == "dim") parse_color(value, DIM);
        else if (key == "accent") parse_color(value, ACCENT);
    }
}

std::wstring widen(const std::string& s, UINT cp = CP_UTF8) {
    if (s.empty()) return L"";
    const int n = MultiByteToWideChar(cp, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(cp, 0, s.data(), static_cast<int>(s.size()), &w[0], n);
    return w;
}
std::string narrow_utf8(const wchar_t* w) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string s(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    s.resize(static_cast<size_t>(n - 1)); // drop the converted NUL terminator
    return s;
}
// Tag text is UTF-8; names from file paths are in the ANSI code page.
std::wstring widen_any(const std::string& s) {
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0)
         ? widen(s) : widen(s, CP_ACP);
}

Cover load_windows_cover(const std::string& trackPath) {
    Cover cover;
    const size_t slash = trackPath.find_last_of("/\\");
    const std::string dir = slash == std::string::npos ? "." : trackPath.substr(0, slash);
    std::string filename;
    for (const char* name : {"cover.jpg", "cover.jpeg", "cover.png"}) {
        const std::string candidate = dir + "/" + name;
        const std::wstring wide = widen(candidate);
        if (GetFileAttributesW(wide.c_str()) != INVALID_FILE_ATTRIBUTES) { filename = candidate; break; }
    }
    if (filename.empty()) return cover;
    IWICImagingFactory* factory = nullptr;
    IWICBitmapDecoder* decoder = nullptr;
    IWICBitmapFrameDecode* frame = nullptr;
    IWICFormatConverter* converter = nullptr;
    do {
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                    IID_PPV_ARGS(&factory)))) break;
        const std::wstring wide = widen(filename);
        if (FAILED(factory->CreateDecoderFromFilename(wide.c_str(), nullptr, GENERIC_READ,
                                                       WICDecodeMetadataCacheOnLoad, &decoder))) break;
        if (FAILED(decoder->GetFrame(0, &frame))) break;
        UINT width = 0, height = 0;
        if (FAILED(frame->GetSize(&width, &height)) || !width || !height || width > 4096 || height > 4096) break;
        if (FAILED(factory->CreateFormatConverter(&converter))) break;
        if (FAILED(converter->Initialize(frame, GUID_WICPixelFormat24bppRGB,
                                         WICBitmapDitherTypeNone, nullptr, 0.0,
                                         WICBitmapPaletteTypeCustom))) break;
        cover.rgb.resize(static_cast<size_t>(width) * height * 3);
        if (FAILED(converter->CopyPixels(nullptr, width * 3,
                                         static_cast<UINT>(cover.rgb.size()), cover.rgb.data()))) {
            cover.rgb.clear(); break;
        }
        cover.w = static_cast<int>(width); cover.h = static_cast<int>(height);
    } while (false);
    if (converter) converter->Release();
    if (frame) frame->Release();
    if (decoder) decoder->Release();
    if (factory) factory->Release();
    return cover;
}

// ---------- state ----------
struct App {
    HWND hwnd = nullptr;
    HFONT fTitle = nullptr, fText = nullptr, fSmall = nullptr, fButton = nullptr;

    std::vector<std::string> files, titles;
    struct Album { std::string name; std::vector<std::string> files, titles; };
    std::vector<Album> albums;
    std::string libraryRoot;
    bool libraryDirty = false;
    bool   folderMode = false;
    bool   libraryMode = true;
    size_t albumSelection = 0;
    size_t idx = 0;

    std::unique_ptr<Player> p;
    Marks       marks;
    std::string title, artist, album, date;
    Cover       cover;
    ma_uint64   total = 0;
    ma_uint32   rate  = 44100;
    std::string status;                 // shown instead of the list (e.g. "finding songs…")
    std::wstring winTitle;

    double phase = 0.0;
    float  level = 0.0f;
    int    height = 0;

    // Layout rectangles, filled in by layout().
    RECT coverBox{}, listBox{}, bar{}, buttons[7]{};
} app;

enum { BTN_BACK, BTN_PLAY, BTN_SKIP, BTN_REW, BTN_FWD, BTN_OPEN, BTN_DOWNLOAD };

ma_uint64 pos() { return app.p ? app.p->pos.load() : 0; }
bool paused()   { return app.p && app.p->paused.load(); }

void layout() {
    const bool hasCover = g_cfg.cover && app.cover.ok();
    const int  textX = hasCover && !g_cfg.coverRight ? PAD + COVER + PAD : PAD;
    const int  textR = hasCover && g_cfg.coverRight ? W - PAD - COVER - PAD : W - PAD;
    if (hasCover) {
        const int x = g_cfg.coverRight ? W - PAD - COVER : PAD;
        app.coverBox = {x, PAD, x + COVER, PAD + COVER};
    } else {
        app.coverBox = {0, 0, 0, 0};
    }
    const int rows = std::min(g_cfg.listRows, 8);
    app.listBox = {textX, 88, textR, 88 + rows * ROW_H};
    const int barY = app.listBox.bottom + 18;
    app.bar = {textX, barY, textR, barY + 6};
    const int by = barY + 34, bw = 46, bh = 32, gap = 8;
    for (int i = 0; i < 5; ++i) {
        const int x = textX + i * (bw + gap);
        app.buttons[i] = {x, by, x + bw, by + bh};
    }
    const int actionWidth = 120, actionGap = 8;
    const int actionX = textR - actionWidth * 2 - actionGap;
    app.buttons[BTN_OPEN] = {actionX, by + bh + 8, actionX + actionWidth, by + bh * 2 + 8};
    app.buttons[BTN_DOWNLOAD] = {actionX + actionWidth + actionGap, by + bh + 8,
                                  actionX + actionWidth * 2 + actionGap, by + bh * 2 + 8};
    app.height = std::max<int>(hasCover ? app.coverBox.bottom : 0, by + bh * 2 + 8) + PAD;
}

void resize_window() {
    RECT r{0, 0, W, app.height};
    AdjustWindowRect(&r, GetWindowLong(app.hwnd, GWL_STYLE), FALSE);
    SetWindowPos(app.hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top, SWP_NOMOVE | SWP_NOZORDER);
}

// ---------- playback ----------
void stop() {
    if (app.p) { close_player(*app.p); app.p.reset(); }
}

void load(size_t i) {
    stop();
    if (app.files.empty()) {
        app.title.clear(); app.artist.clear(); app.album.clear(); app.date.clear();
        app.marks.clear(); app.total = 0; app.status = "no playable music files";
        InvalidateRect(app.hwnd, nullptr, FALSE);
        return;
    }
    for (size_t tries = 0; tries < app.files.size(); ++tries, i = (i + 1) % app.files.size()) {
        app.idx = i;
        const std::string& f = app.files[i];
        const Tags tags = read_tags(f);
        auto get = [&](const char* k) { auto it = tags.find(k); return it == tags.end() ? std::string() : it->second; };
        app.title  = get("TITLE").empty() ? display_name(f) : get("TITLE");
        app.artist = get("ARTIST").empty() ? get("ALBUMARTIST") : get("ARTIST");
        const std::string taggedAlbum = get("ALBUM");
        if (!taggedAlbum.empty()) app.album = taggedAlbum;
        app.date   = get("DATE");
        app.cover  = load_cover(f);
        if (!app.cover.ok()) app.cover = load_windows_cover(f);
        layout();
        resize_window();

        app.marks.clear();
        if (app.folderMode) app.marks.push_back(0);
        else if (!read_stored_marks(f, app.marks)) {
            app.status = "finding songs\xE2\x80\xA6";          // paint the message before the slow scan
            InvalidateRect(app.hwnd, nullptr, FALSE);
            UpdateWindow(app.hwnd);
            app.marks = get_marks(f, false);
        }
        app.status.clear();

        app.p.reset(new Player);
        if (open_and_play(*app.p, f)) {
            app.rate  = app.p->decoder.outputSampleRate;
            app.total = 0;
            ma_decoder_get_length_in_pcm_frames(&app.p->decoder, &app.total);
            return;
        }
        app.p.reset();
    }
    app.total = 0;
    app.title.clear(); app.artist.clear(); app.album.clear(); app.date.clear();
    app.marks.clear();
    app.status = "none of these files would play";
}

void next_file() { if (!app.files.empty()) load((app.idx + 1) % app.files.size()); }
void prev_file() { if (!app.files.empty()) load((app.idx + app.files.size() - 1) % app.files.size()); }

void seek(ma_uint64 frame) {
    if (!app.p) return;
    if (app.total && frame >= app.total) { next_file(); return; }
    seek_to(*app.p, frame);
}

void skip() {
    if (!app.p || app.files.empty()) return;
    if (app.folderMode) { next_file(); return; }
    auto next = std::upper_bound(app.marks.begin(), app.marks.end(), pos());
    if (next == app.marks.end()) next_file();
    else seek(*next);
}

void back() {
    if (!app.p || app.files.empty()) return;
    const ma_uint64 grace = static_cast<ma_uint64>(app.rate * BACK_GRACE_SEC);
    if (app.folderMode) {
        if (pos() < grace) prev_file(); else seek(0);
        return;
    }
    const size_t cur = current_song(app.marks, pos());
    seek(app.marks[pos() < app.marks[cur] + grace && cur > 0 ? cur - 1 : cur]);
}

void seek_by(double seconds) {
    if (!app.p) return;
    const double target = static_cast<double>(pos()) + seconds * app.rate;
    seek(target <= 0 ? 0 : static_cast<ma_uint64>(target));
}

void toggle_pause() { if (app.p) app.p->paused.store(!app.p->paused.load()); }

void set_files(const std::vector<std::string>& paths) {
    app.files.clear();
    app.titles.clear();
    app.albums.clear(); app.libraryMode = false;
    app.folderMode = false;
    for (const std::string& p : paths) {
        if (is_dir(p)) { collect_audio(p, app.files); app.folderMode = true; }
        else           app.files.push_back(p);
    }
    for (const std::string& f : app.files) {
        const Tags t = read_tags(f);
        auto it = t.find("TITLE");
        app.titles.push_back(it != t.end() && !it->second.empty() ? it->second : display_name(f));
    }
}

void build_library(const std::string& root) {
    app.libraryRoot = root;
    app.libraryDirty = false;
    std::vector<std::string> all; collect_audio(root, all);
    std::map<std::string, App::Album> grouped;
    const size_t start = root.size() + (root.empty() || (root.back() != '\\' && root.back() != '/') ? 1 : 0);
    for (const auto& file : all) {
        std::string relative = file.size() > start ? file.substr(start) : file;
        const size_t sep = relative.find_first_of("/\\");
        std::string anchor = file.substr(0, file.find_last_of("/\\"));
        std::string rootPrefix = root; if (!rootPrefix.empty() && rootPrefix.back() != '\\' && rootPrefix.back() != '/') rootPrefix += "\\";
        auto hasCover = [](const std::string& dir) {
            for (const char* name : {"cover.ppm", "cover.jpg", "cover.jpeg", "cover.png"})
                if (std::ifstream(dir + "\\" + name, std::ios::binary).good()) return true;
            return false;
        };
        while (anchor.size() >= rootPrefix.size() && anchor.compare(0, rootPrefix.size(), rootPrefix) == 0 && !hasCover(anchor)) {
            const size_t parent = anchor.find_last_of("/\\");
            if (parent == std::string::npos || parent < root.size()) break;
            anchor.resize(parent);
        }
        std::string albumName;
        if (anchor.size() >= rootPrefix.size() && anchor.compare(0, rootPrefix.size(), rootPrefix) == 0) {
            albumName = anchor.substr(rootPrefix.size());
            const size_t last = albumName.find_last_of("/\\"); if (last != std::string::npos) albumName = albumName.substr(last + 1);
        } else albumName = sep == std::string::npos ? "Music" : relative.substr(0, sep);
        if (albumName.empty()) albumName = "Music";
        auto& album = grouped[anchor]; album.name = albumName; album.files.push_back(file);
        const Tags tags = read_tags(file); auto it = tags.find("TITLE");
        album.titles.push_back(it != tags.end() && !it->second.empty() ? it->second : display_name(file));
    }
    app.albums.clear();
    for (auto& entry : grouped) app.albums.push_back(std::move(entry.second));
    app.files.clear(); app.titles.clear(); app.folderMode = false; app.libraryMode = true;
    app.albumSelection = 0; app.idx = 0; app.album.clear(); app.title.clear(); app.artist.clear();
    app.status = app.albums.empty() ? "No music yet — use Download to add tracks" : "Choose an album to play";
    stop(); app.cover = Cover(); app.total = 0; layout(); resize_window();
    InvalidateRect(app.hwnd, nullptr, FALSE);
}

void open_album(size_t i) {
    if (i >= app.albums.size()) return;
    stop(); app.albumSelection = i; app.album = app.albums[i].name;
    app.files = app.albums[i].files; app.titles = app.albums[i].titles;
    app.folderMode = true; app.libraryMode = false; app.idx = 0; app.status.clear();
    load(0);
}

void show_library() {
    if (app.libraryDirty && !app.libraryRoot.empty()) { build_library(app.libraryRoot); return; }
    stop(); app.files.clear(); app.titles.clear(); app.folderMode = false; app.libraryMode = true;
    app.title.clear(); app.artist.clear(); app.album.clear(); app.total = 0; app.cover = Cover();
    app.status = app.albums.empty() ? "No music yet — use Download to add tracks" : "Choose an album to play";
    layout(); resize_window(); InvalidateRect(app.hwnd, nullptr, FALSE);
}

void open_folder() {
    if (!app.libraryRoot.empty()) build_library(app.libraryRoot);
}

void open_downloader();   // built-in downloader, see downloader_win32.h
void downloader_shutdown();
bool downloader_dialog_message(MSG& msg);

// ---------- drawing ----------
void fill(HDC dc, const RECT& r, COLORREF c) {
    HBRUSH b = CreateSolidBrush(c);
    FillRect(dc, &r, b);
    DeleteObject(b);
}

void round_rect(HDC dc, const RECT& r, COLORREF c, int radius) {
    HBRUSH b = CreateSolidBrush(c);
    HPEN   p = CreatePen(PS_SOLID, 1, c);
    HGDIOBJ ob = SelectObject(dc, b), op = SelectObject(dc, p);
    RoundRect(dc, r.left, r.top, r.right, r.bottom, radius, radius);
    SelectObject(dc, ob); SelectObject(dc, op);
    DeleteObject(b); DeleteObject(p);
}

void text(HDC dc, const std::wstring& s, RECT r, HFONT f, COLORREF c, UINT flags = DT_LEFT) {
    SelectObject(dc, f);
    SetTextColor(dc, c);
    DrawTextW(dc, s.c_str(), static_cast<int>(s.size()), &r, flags | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
}

int text_width(HDC dc, const std::wstring& s, HFONT f) {
    SelectObject(dc, f);
    SIZE sz{};
    GetTextExtentPoint32W(dc, s.c_str(), static_cast<int>(s.size()), &sz);
    return sz.cx;
}

void draw_cover(HDC dc) {
    const RECT& b = app.coverBox;
    if (b.right == 0) return;
    const Cover& c = app.cover;
    // Fit inside the box, keeping its shape; nearest-neighbour keeps the pixel-art look.
    const double s = std::min(static_cast<double>(COVER) / c.w, static_cast<double>(COVER) / c.h);
    const int dw = static_cast<int>(c.w * s), dh = static_cast<int>(c.h * s);
    const int dx = b.left + (COVER - dw) / 2, dy = b.top + (COVER - dh) / 2;

    std::vector<unsigned char> bgra(static_cast<size_t>(c.w) * c.h * 4);
    for (int i = 0; i < c.w * c.h; ++i) {
        bgra[i * 4 + 0] = c.rgb[i * 3 + 2];
        bgra[i * 4 + 1] = c.rgb[i * 3 + 1];
        bgra[i * 4 + 2] = c.rgb[i * 3 + 0];
    }
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = c.w;
    bi.bmiHeader.biHeight = -c.h;                  // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    SetStretchBltMode(dc, c.w <= 64 && c.h <= 64 ? COLORONCOLOR : HALFTONE);
    if (c.w > 64 || c.h > 64) SetBrushOrgEx(dc, 0, 0, nullptr);
    StretchDIBits(dc, dx, dy, dw, dh, 0, 0, c.w, c.h, bgra.data(), &bi, DIB_RGB_COLORS, SRCCOPY);
}

void draw_wave(HDC dc, int x, int yMid, int width) {
    const int bars = width / 6;
    const double amp = paused() ? 0.08 : 0.25 + 0.75 * std::sqrt(static_cast<double>(app.level));
    for (int i = 0; i < bars; ++i) {
        const double shape = 0.5 + 0.34 * std::sin(i * 0.62 - app.phase) + 0.16 * std::sin(i * 1.70 + app.phase * 1.3);
        const int h = std::max(2, static_cast<int>(shape * amp * (ROW_H - 6)));
        const RECT r{x + i * 6, yMid - h / 2, x + i * 6 + 4, yMid - h / 2 + h};
        fill(dc, r, accent());
    }
}

void draw_button(HDC dc, int which, const wchar_t* label, bool active) {
    const RECT& r = app.buttons[which];
    round_rect(dc, r, active ? accent() : PANEL, 10);
    text(dc, label, r, app.fButton, active ? BG : TEXT, DT_CENTER);
}

void paint(HDC dc) {
    RECT all{0, 0, W, app.height};
    fill(dc, all, BG);
    SetBkMode(dc, TRANSPARENT);
    draw_cover(dc);

    const RECT& L = app.listBox;
    const ma_uint64 p = pos();
    const ma_uint64 total = std::max(app.total, p);

    if (app.libraryMode) {
        text(dc, L"Your music", {L.left, PAD, L.right, PAD + 34}, app.fTitle, TEXT);
        text(dc, std::to_wstring(app.albums.size()) + L" albums  ·  choose one to play",
             {L.left, PAD + 36, L.right, PAD + 58}, app.fText, accent());
        if (app.albums.empty()) {
            text(dc, widen(app.status), {L.left, L.top, L.right, L.top + ROW_H}, app.fText, DIM);
        } else {
            const size_t rows = static_cast<size_t>((L.bottom - L.top) / ROW_H);
            const size_t above = (rows - 1) / 2;
            const size_t first = app.albums.size() <= rows ? 0 : std::min(app.albumSelection > above ? app.albumSelection - above : 0, app.albums.size() - rows);
            for (size_t r = 0; r < rows && first + r < app.albums.size(); ++r) {
                const size_t i = first + r; const int y = L.top + static_cast<int>(r) * ROW_H;
                RECT row{L.left, y, L.right, y + ROW_H};
                if (i == app.albumSelection) round_rect(dc, {row.left - 6, row.top + 1, row.right, row.bottom - 1}, PANEL, 8);
                text(dc, std::to_wstring(i + 1), {row.left, y, row.left + 28, y + ROW_H}, app.fSmall, i == app.albumSelection ? accent() : DIM);
                text(dc, widen_any(app.albums[i].name), {row.left + 32, y, row.right - 90, y + ROW_H}, app.fText, TEXT);
                text(dc, std::to_wstring(app.albums[i].files.size()) + L" tracks", {row.right - 86, y, row.right, y + ROW_H}, app.fSmall, DIM, DT_RIGHT);
            }
        }
    } else {
    // Title + artist line.
    text(dc, L"‹ Albums", {L.left, 0, L.left + 100, 16}, app.fSmall, accent());
    text(dc, widen_any(app.title), {L.left, PAD, L.right, PAD + 34}, app.fTitle, TEXT);
    std::wstring sub = widen_any(app.artist.empty() ? std::string("Unknown artist") : app.artist);
    const int artistW = text_width(dc, sub, app.fText);
    text(dc, sub, {L.left, PAD + 36, L.right, PAD + 58}, app.fText, accent());
    std::wstring rest;
    if (!app.album.empty()) rest += L"  ·  " + widen_any(app.album);
    if (!app.date.empty())  rest += L"  ·  " + widen_any(app.date);
    rest += L"  ·  " + std::to_wstring(app.idx + 1) + L"/" + std::to_wstring(app.files.size());
    text(dc, rest, {L.left + artistW, PAD + 36, L.right, PAD + 58}, app.fText, DIM);

    // Song list: files of the folder, or songs inside this file.
    if (!app.status.empty()) {
        text(dc, widen(app.status), {L.left, L.top, L.right, L.top + ROW_H}, app.fText, DIM);
    } else {
        const size_t n   = app.folderMode ? app.files.size() : app.marks.size();
        const size_t cur = app.folderMode ? app.idx : current_song(app.marks, p);
        const size_t rows  = static_cast<size_t>((L.bottom - L.top) / ROW_H);
        const size_t above = (rows - 1) / 2;
        const size_t first = n <= rows ? 0 : std::min(cur > above ? cur - above : 0, n - rows);
        for (size_t r = 0; r < rows && first + r < n; ++r) {
            const size_t i = first + r;
            const int y = L.top + static_cast<int>(r) * ROW_H;
            RECT row{L.left, y, L.right, y + ROW_H};
            const bool now = i == cur;
            if (now) round_rect(dc, {row.left - 6, row.top + 1, row.right, row.bottom - 1}, PANEL, 8);

            std::wstring name, len;
            if (app.folderMode) {
                name = widen_any(app.titles[i]);
                if (now) len = widen(mmss(p, app.rate)) + L" / " + widen(mmss(total, app.rate));
            } else {
                const ma_uint64 s = app.marks[i], e = i + 1 < n ? app.marks[i + 1] : total;
                name = L"Song " + std::to_wstring(i + 1) + L"   " + widen(mmss(s, app.rate));
                len = now ? widen(mmss(p > s ? p - s : 0, app.rate)) + L" / " + widen(mmss(e > s ? e - s : 0, app.rate))
                          : widen(mmss(e > s ? e - s : 0, app.rate));
            }
            const std::wstring num = std::to_wstring(i + 1);
            text(dc, app.folderMode ? num : L"", {row.left, y, row.left + 26, y + ROW_H}, app.fSmall, now ? accent() : DIM);
            const int lenW = text_width(dc, len, app.fSmall);
            const int waveW = now ? 96 : 0;
            const int nameL = row.left + (app.folderMode ? 28 : 0);
            text(dc, name, {nameL, y, row.right - lenW - waveW - 16, y + ROW_H}, app.fText, now ? TEXT : DIM);
            if (now) draw_wave(dc, row.right - lenW - waveW - 8, y + ROW_H / 2, waveW);
            text(dc, len, {row.right - lenW - 4, y, row.right - 4, y + ROW_H}, app.fSmall, now ? TEXT : DIM);
        }
    }
    }

    // Progress through the whole file.
    const RECT& B = app.bar;
    round_rect(dc, B, PANEL, 6);
    if (total) {
        const int head = B.left + static_cast<int>((B.right - B.left) * static_cast<double>(p) / total);
        if (head > B.left + 2) round_rect(dc, {B.left, B.top, head, B.bottom}, accent(), 6);
        round_rect(dc, {head - 6, B.top - 4, head + 6, B.bottom + 4}, TEXT, 12);
    }
    text(dc, widen(mmss(p, app.rate)), {B.left, B.bottom + 4, B.left + 80, B.bottom + 20}, app.fSmall, DIM);
    text(dc, widen(mmss(total, app.rate)), {B.right - 80, B.bottom + 4, B.right, B.bottom + 20}, app.fSmall, DIM, DT_RIGHT);

    // Buttons (plain text, no emoji).
    draw_button(dc, BTN_BACK, L"|<", false);
    draw_button(dc, BTN_PLAY, paused() ? L"▶" : L"||", true);
    draw_button(dc, BTN_SKIP, L">|", false);
    draw_button(dc, BTN_REW,  (L"-" + std::to_wstring(static_cast<int>(g_cfg.seekSec)) + L"s").c_str(), false);
    draw_button(dc, BTN_FWD,  (L"+" + std::to_wstring(static_cast<int>(g_cfg.seekSec)) + L"s").c_str(), false);
    draw_button(dc, BTN_OPEN, L"Refresh", false);
    draw_button(dc, BTN_DOWNLOAD, L"Download", false);
}

void update_title() {
    std::wstring t = L"player - ";
    if (paused()) t += L"(paused) ";
    t += widen_any(app.title);
    if (!app.folderMode && app.marks.size() > 1)
        t += L" #" + std::to_wstring(current_song(app.marks, pos()) + 1);
    if (!app.artist.empty()) t += L" - " + widen_any(app.artist);
    if (t != app.winTitle) { app.winTitle = t; SetWindowTextW(app.hwnd, t.c_str()); }
}

bool inside(const RECT& r, int x, int y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }

void click(int x, int y) {
    if (!app.libraryMode && y < 20 && x < app.listBox.left + 110) { show_library(); return; }
    for (int i = 0; i < 7; ++i)
        if (inside(app.buttons[i], x, y)) {
            switch (i) {
            case BTN_BACK: back(); break;
            case BTN_PLAY: toggle_pause(); break;
            case BTN_SKIP: skip(); break;
            case BTN_REW:  seek_by(-g_cfg.seekSec); break;
            case BTN_FWD:  seek_by(g_cfg.seekSec); break;
            case BTN_OPEN: open_folder(); break;
            case BTN_DOWNLOAD: open_downloader(); break;
            }
            return;
        }
    // Progress bar (generous click area): jump there.
    RECT hit = app.bar;
    InflateRect(&hit, 0, 8);
    if (inside(hit, x, y) && app.total) {
        const double f = static_cast<double>(x - app.bar.left) / (app.bar.right - app.bar.left);
        seek(static_cast<ma_uint64>(std::max(0.0, std::min(0.999, f)) * app.total));
        return;
    }
    // Song list: play that song.
    if (inside(app.listBox, x, y) && (app.libraryMode || app.status.empty())) {
        if (app.libraryMode) {
            const size_t rows = static_cast<size_t>((app.listBox.bottom - app.listBox.top) / ROW_H);
            const size_t above = (rows - 1) / 2;
            const size_t first = app.albums.size() <= rows ? 0 : std::min(app.albumSelection > above ? app.albumSelection - above : 0, app.albums.size() - rows);
            const size_t i = first + static_cast<size_t>((y - app.listBox.top) / ROW_H);
            if (i < app.albums.size()) open_album(i);
            return;
        }
        const size_t n   = app.folderMode ? app.files.size() : app.marks.size();
        const size_t cur = app.folderMode ? app.idx : current_song(app.marks, pos());
        const size_t rows  = static_cast<size_t>((app.listBox.bottom - app.listBox.top) / ROW_H);
        const size_t above = (rows - 1) / 2;
        const size_t first = n <= rows ? 0 : std::min(cur > above ? cur - above : 0, n - rows);
        const size_t i = first + static_cast<size_t>((y - app.listBox.top) / ROW_H);
        if (i < n) { if (app.folderMode) load(i); else seek(app.marks[i]); }
    }
}

LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_TIMER:
        // Consume EOF once, on the UI thread. Repeated timer messages must not
        // race to tear down/recreate the decoder while the audio callback is
        // still reporting the same end of stream.
        if (app.p && app.p->finished.exchange(false)) next_file();
        if (!paused()) app.phase += 0.30;
        app.level = std::max(app.p ? app.p->level.load() : 0.0f, app.level * 0.85f);
        update_title();
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;                                   // everything is painted in WM_PAINT (no flicker)
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, W, app.height);
        HGDIOBJ old = SelectObject(mem, bmp);
        paint(mem);
        BitBlt(dc, 0, 0, W, app.height, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_LBUTTONDOWN:
        click(GET_X_LPARAM(l), GET_Y_LPARAM(l));
        return 0;
    case WM_KEYDOWN:
        switch (w) {
        case VK_SPACE: case 'P': toggle_pause(); break;
        case VK_LEFT:  case 'H': seek_by(-g_cfg.seekSec); break;
        case VK_RIGHT: case 'L': seek_by(g_cfg.seekSec); break;
        case 'S': skip(); break;
        case 'B': back(); break;
        case 'N': next_file(); break;
        case 'O': open_folder(); break;
        case VK_ESCAPE: if (!app.libraryMode) show_library(); else DestroyWindow(h); break;
        case 'Q': DestroyWindow(h); break;
        }
        return 0;
    case WM_DESTROY:
        KillTimer(h, 1);
        downloader_shutdown();
        stop();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

HFONT font(int px, int weight) {
    return CreateFontW(-px, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                       CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
}

#include "downloader_win32.h"

}  // namespace

int WINAPI WinMain(HINSTANCE inst, HINSTANCE, LPSTR, int show) {
    const HRESULT comInit = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    // Arguments (files / folders / --config FILE), converted for the C runtime.
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(narrow_utf8(wargv[i]));
    LocalFree(wargv);

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::string exeDir = narrow_utf8(exe);
    exeDir = exeDir.substr(0, exeDir.find_last_of("/\\"));

    std::string configPath, colorsPath;
    std::vector<std::string> paths;
    for (size_t i = 0; i < args.size(); ++i) {
        if (args[i] == "--config" && i + 1 < args.size()) configPath = args[++i];
        else if (args[i] == "--colors" && i + 1 < args.size()) colorsPath = args[++i];
        else if (!args[i].empty() && args[i][0] != '-') paths.push_back(args[i]);
    }
    if (!configPath.empty()) load_config(configPath);
    else {
        std::vector<std::string> candidates;
        if (const char* env = std::getenv("PLAYER_CONFIG")) candidates.push_back(env);
        candidates.push_back("player.conf");
        candidates.push_back(exeDir + "/player.conf");
        if (const char* home = std::getenv("USERPROFILE")) candidates.push_back(std::string(home) + "/.config/player/config");
        for (const std::string& c : candidates) if (load_config(c)) break;
    }
    load_colors(colorsPath.empty() ? exeDir + "/colors.conf" : colorsPath);

    // Keep the library beside the executable so a portable folder works
    // without asking users to browse for or type a music path.
    app.libraryRoot = exeDir + "/music";
    const std::wstring musicRootWide = widen(app.libraryRoot);
    const BOOL musicRootCreated = CreateDirectoryW(musicRootWide.c_str(), nullptr);
    const DWORD createError = musicRootCreated ? ERROR_SUCCESS : GetLastError();
    const DWORD rootAttributes = GetFileAttributesW(musicRootWide.c_str());
    if (!musicRootCreated && !(createError == ERROR_ALREADY_EXISTS &&
          rootAttributes != INVALID_FILE_ATTRIBUTES && (rootAttributes & FILE_ATTRIBUTE_DIRECTORY))) {
        MessageBoxW(nullptr, L"The app could not create its music folder. Move player.exe to a writable folder and try again.",
                    L"Music folder unavailable", MB_OK | MB_ICONERROR);
        if (SUCCEEDED(comInit)) CoUninitialize();
        return 1;
    }

    WNDCLASSW wc{};
    wc.lpfnWndProc   = wndproc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon         = LoadIcon(nullptr, IDI_APPLICATION);
    wc.lpszClassName = L"player";
    RegisterClassW(&wc);

    app.fTitle  = font(24, FW_SEMIBOLD);
    app.fText   = font(15, FW_NORMAL);
    app.fSmall  = font(13, FW_NORMAL);
    app.fButton = font(15, FW_SEMIBOLD);

    // Fixed-size pop-up: title bar with close / minimise, no resizing.
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    app.cover = Cover();
    layout();
    RECT r{0, 0, W, app.height};
    AdjustWindowRect(&r, style, FALSE);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    app.hwnd = CreateWindowW(L"player", L"player", style,
                             (GetSystemMetrics(SM_CXSCREEN) - ww) / 2, (GetSystemMetrics(SM_CYSCREEN) - wh) / 2,
                             ww, wh, nullptr, nullptr, inst, nullptr);
    app.status = "Loading music library…";
    ShowWindow(app.hwnd, show);
    UpdateWindow(app.hwnd);
    if (!paths.empty() && paths.size() == 1 && is_dir(paths[0])) build_library(paths[0]);
    else if (!paths.empty()) { set_files(paths); if (app.files.empty()) app.status = "No playable music files found"; else load(0); }
    else build_library(app.libraryRoot);
    SetTimer(app.hwnd, 1, 33, nullptr);             // ~30 fps animation

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (downloader_dialog_message(msg)) continue;   // Tab / Enter / Esc in the downloader
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (SUCCEEDED(comInit)) CoUninitialize();
    return 0;
}
