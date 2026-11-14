// downloader_win32.h - the built-in Internet Archive downloader window.
// Included from gui_win32.cpp inside its anonymous namespace (uses app, widen,
// narrow_utf8, colours, font() and build_library from there).
//
// Pure Win32 + WinINet, so it works on every Windows version and under Wine
// (no PowerShell needed). Network work runs on a background thread that
// reports back with posted window messages.

// (headers are included at the top of gui_win32.cpp)

enum : UINT {
    WM_DL_STATUS = WM_APP + 1,  // lParam: std::wstring* (error if wParam)
    WM_DL_PROGRESS,             // wParam: 0..1000
    WM_DL_LOADED,               // lParam: DlItem*
    WM_DL_FINISHED,             // lParam: std::wstring*; wParam: 1 = success, 2 = error
    WM_DL_ASK_FFMPEG,           // sent; returns IDYES / IDNO
};
enum {
    ID_URL = 100, ID_FIND, ID_LIST, ID_SELECT, ID_ARTIST, ID_ALBUM, ID_CONVERT, ID_KEEP,
    ID_PROGRESS, ID_STATUS, ID_DOWNLOAD, ID_OPENDIR, ID_CLOSE,
};

struct DlEntry { std::string name; double size = 0; };
struct DlItem {
    std::string id, artist, album;
    std::vector<DlEntry> audio;
    std::vector<std::string> allNames;
};
struct Cancelled {};

struct Downloader {
    HWND hwnd = nullptr;
    HWND url = nullptr, find = nullptr, list = nullptr, select = nullptr, artist = nullptr, album = nullptr,
         convert = nullptr, keep = nullptr, progress = nullptr, status = nullptr, download = nullptr,
         opendir = nullptr, close = nullptr;
    HBRUSH bgBrush = nullptr, fieldBrush = nullptr;
    HANDLE thread = nullptr;
    std::atomic<bool> cancel{false};
    bool busy = false, closeWhenDone = false, statusError = false, filling = false;
    int lastUrlLen = 0;
    DlItem item;
    std::wstring lastFolder;
} dl;

// ---------------------------------------------------------------- small helpers
std::wstring dl_text(HWND h) {
    const int n = GetWindowTextLengthW(h);
    std::wstring s(static_cast<size_t>(n) + 1, L'\0');
    GetWindowTextW(h, &s[0], n + 1);
    s.resize(static_cast<size_t>(n));
    return s;
}
std::string trim(std::string s) {
    const auto a = s.find_first_not_of(" \t\r\n");
    if (a == std::string::npos) return {};
    return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}
std::string format_size(double b) {
    char buf[32];
    if (b >= 1024.0 * 1024 * 1024) std::snprintf(buf, sizeof buf, "%.1f GB", b / (1024.0 * 1024 * 1024));
    else if (b >= 1024.0 * 1024) std::snprintf(buf, sizeof buf, "%.1f MB", b / (1024.0 * 1024));
    else if (b > 0) std::snprintf(buf, sizeof buf, "%.0f KB", b / 1024.0);
    else buf[0] = 0;
    return buf;
}
std::string safe_name(std::string v) {
    for (char& c : v) if (std::strchr("<>:\"/\\|?*", c) || static_cast<unsigned char>(c) < 32) c = '-';
    v = trim(v);
    while (!v.empty() && (v.back() == '.' || v.back() == ' ')) v.pop_back();
    return v.empty() ? "Unknown" : v;
}
std::string base_name(const std::string& path) {
    const size_t s = path.find_last_of('/');
    return s == std::string::npos ? path : path.substr(s + 1);
}
std::string lower(std::string s) { for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c))); return s; }
bool ends_with(const std::string& s, const std::string& suffix) {
    return s.size() >= suffix.size() && lower(s.substr(s.size() - suffix.size())) == suffix;
}
std::string url_escape(const std::string& s, bool keepSlash) {
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || (keepSlash && c == '/')) out += static_cast<char>(c);
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
    }
    return out;
}
std::string url_unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit(static_cast<unsigned char>(s[i + 1])) && std::isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            out += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16)); i += 2;
        } else out += s[i];
    }
    return out;
}
// "https://archive.org/details/ID/..." or a bare identifier -> ID ("" if not recognised).
std::string item_id(std::string v) {
    v = trim(v);
    for (const char* key : {"archive.org/details/", "archive.org/download/", "archive.org/metadata/", "archive.org/embed/"}) {
        const size_t at = v.find(key);
        if (at == std::string::npos) continue;
        std::string rest = v.substr(at + std::strlen(key));
        const size_t end = rest.find_first_of("/?# \t");
        if (end != std::string::npos) rest.resize(end);
        return url_unescape(rest);
    }
    if (v.empty()) return {};
    for (char c : v) if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.')) return {};
    return v;
}
std::string archive_url(const std::string& id, const std::string& name) {
    return "https://archive.org/download/" + url_escape(id, false) + "/" + url_escape(name, true);
}
bool file_exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }
std::wstring exe_dir() {
    wchar_t exe[MAX_PATH]; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring d = exe; return d.substr(0, d.find_last_of(L"/\\"));
}
void make_dirs(const std::wstring& path) {
    for (size_t i = 3; i <= path.size(); ++i)
        if (i == path.size() || path[i] == L'\\' || path[i] == L'/') CreateDirectoryW(path.substr(0, i).c_str(), nullptr);
}

// Posting from the worker thread.
void post_status(const std::string& s, bool error = false) {
    PostMessageW(dl.hwnd, WM_DL_STATUS, error ? 1 : 0, reinterpret_cast<LPARAM>(new std::wstring(widen(s))));
}
void post_progress(double fraction) {
    fraction = std::max(0.0, std::min(1.0, fraction));
    PostMessageW(dl.hwnd, WM_DL_PROGRESS, static_cast<WPARAM>(fraction * 1000), 0);
}

// ---------------------------------------------------------------- HTTP (WinINet)
struct Net {
    HINTERNET h = nullptr;
    Net() {
        h = InternetOpenW(L"GruvboxMusicPlayer/1.2", INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
        if (!h) throw std::runtime_error("Could not start the Internet connection.");
        DWORD timeout = 60000;
        InternetSetOptionW(h, INTERNET_OPTION_CONNECT_TIMEOUT, &timeout, sizeof timeout);
        InternetSetOptionW(h, INTERNET_OPTION_RECEIVE_TIMEOUT, &timeout, sizeof timeout);
    }
    ~Net() { if (h) InternetCloseHandle(h); }
};
struct UrlHandle {
    HINTERNET h = nullptr;
    ~UrlHandle() { if (h) InternetCloseHandle(h); }
};

void open_url(Net& net, UrlHandle& u, const std::string& url, unsigned long long from, DWORD& status, long long& length) {
    std::wstring headers;
    if (from) headers = L"Range: bytes=" + std::to_wstring(from) + L"-\r\n";
    const std::wstring wurl = widen(url);
    u.h = InternetOpenUrlW(net.h, wurl.c_str(), headers.empty() ? nullptr : headers.c_str(),
                           headers.empty() ? 0 : static_cast<DWORD>(-1),
                           INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_UI |
                           INTERNET_FLAG_KEEP_CONNECTION, 0);
    if (!u.h) throw std::runtime_error("Could not reach archive.org. Check your Internet connection and try again.");
    status = 0; DWORD size = sizeof status;
    HttpQueryInfoW(u.h, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &status, &size, nullptr);
    wchar_t len[64]{}; size = sizeof len; length = -1;
    if (HttpQueryInfoW(u.h, HTTP_QUERY_CONTENT_LENGTH, len, &size, nullptr)) length = _wcstoi64(len, nullptr, 10);
}

std::string http_get_text(Net& net, const std::string& url) {
    UrlHandle u; DWORD status; long long length;
    open_url(net, u, url, 0, status, length);
    if (status >= 400) throw std::runtime_error("archive.org answered with an error (" + std::to_string(status) + "). Try again later.");
    std::string out; char buf[65536]; DWORD got = 0;
    while (InternetReadFile(u.h, buf, sizeof buf, &got) && got) {
        if (dl.cancel) throw Cancelled{};
        out.append(buf, got);
    }
    return out;
}

// Streams a URL to `dest`, resuming "<dest>.part". onProgress(have, total).
void http_download(Net& net, const std::string& url, const std::wstring& dest, const std::string& label,
                   const std::function<void(double)>& onFraction) {
    const std::wstring part = dest + L".part";
    unsigned long long have = 0;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(part.c_str(), GetFileExInfoStandard, &fa))
        have = (static_cast<unsigned long long>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;

    UrlHandle u; DWORD status; long long length;
    open_url(net, u, url, have, status, length);
    if (status == 416 && have) { MoveFileExW(part.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING); return; }
    if (status == 401 || status == 403) throw std::runtime_error(label + " is restricted on the Internet Archive, so it can't be downloaded.");
    if (status == 404) throw std::runtime_error(label + " was not found on the Internet Archive.");
    if (status >= 400) throw std::runtime_error("Could not download " + label + " (error " + std::to_string(status) + ").");
    const bool resumed = have && status == 206;
    if (!resumed) have = 0;
    const long long total = length > 0 ? length + static_cast<long long>(have) : -1;

    HANDLE f = CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, resumed ? OPEN_ALWAYS : CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) throw std::runtime_error("Could not save " + label + ". Is the music folder writable?");
    if (resumed) SetFilePointer(f, 0, nullptr, FILE_END);

    std::vector<char> buf(256 * 1024);
    DWORD got = 0, lastTick = 0, startTick = GetTickCount();
    const unsigned long long startHave = have;
    bool ok = true;
    while (true) {
        if (dl.cancel) { CloseHandle(f); throw Cancelled{}; }
        if (!InternetReadFile(u.h, buf.data(), static_cast<DWORD>(buf.size()), &got)) { ok = false; break; }
        if (!got) break;
        DWORD wrote = 0;
        if (!WriteFile(f, buf.data(), got, &wrote, nullptr) || wrote != got) {
            CloseHandle(f); throw std::runtime_error("Could not save " + label + ". Is the disk full?");
        }
        have += got;
        const DWORD now = GetTickCount();
        if (now - lastTick >= 150) {
            lastTick = now;
            const double secs = std::max(0.001, (now - startTick) / 1000.0);
            const std::string speed = format_size((have - startHave) / secs) + "/s";
            if (total > 0) {
                onFraction(static_cast<double>(have) / total);
                post_status(label + "    " + format_size(have) + " of " + format_size(total) + "    (" + speed + ")");
            } else post_status(label + "    " + format_size(have) + "    (" + speed + ")");
        }
    }
    CloseHandle(f);
    if (!ok || (total > 0 && static_cast<long long>(have) < total))
        throw std::runtime_error("The download of " + label + " stopped early. Press Download again to continue it.");
    onFraction(1.0);
    if (!MoveFileExW(part.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING))
        throw std::runtime_error("Could not save " + label + ".");
}

// ---------------------------------------------------------------- FFmpeg (FLAC -> MP3)
std::wstring tools_dir() { return exe_dir() + L"\\tools"; }

std::wstring find_ffmpeg() {
    const std::wstring local = tools_dir() + L"\\ffmpeg.exe";
    if (file_exists(local)) return local;
    wchar_t found[MAX_PATH]{};
    const DWORD n = SearchPathW(nullptr, L"ffmpeg.exe", nullptr, MAX_PATH, found, nullptr);
    if (n && n < MAX_PATH) return found;
    return {};
}

std::wstring install_ffmpeg(Net& net) {
    static const char* urls[] = {
        "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip",
        "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-win64-gpl.zip",
    };
    const std::wstring dir = tools_dir();
    make_dirs(dir);
    const std::wstring zipPath = dir + L"\\ffmpeg-download.zip";
    const std::wstring exe = dir + L"\\ffmpeg.exe";
    std::string lastError;
    for (const char* url : urls) {
        try {
            http_download(net, url, zipPath, "FFmpeg converter (one-time setup)", [](double f) { post_progress(f); });
            post_status("Unpacking FFmpeg...");
            std::vector<uint8_t> zip;
            {
                HANDLE f = CreateFileW(zipPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
                if (f == INVALID_HANDLE_VALUE) throw std::runtime_error("Could not open the FFmpeg download.");
                LARGE_INTEGER sz; GetFileSizeEx(f, &sz);
                zip.resize(static_cast<size_t>(sz.QuadPart));
                DWORD got = 0; size_t at = 0;
                while (at < zip.size() && ReadFile(f, zip.data() + at, static_cast<DWORD>(std::min<size_t>(zip.size() - at, 1 << 24)), &got, nullptr) && got) at += got;
                CloseHandle(f);
                if (at != zip.size()) throw std::runtime_error("Could not read the FFmpeg download.");
            }
            std::vector<uint8_t> bin = dlutil::zip_extract(zip, "bin/ffmpeg.exe");
            const std::wstring tmp = exe + L".tmp";
            HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            DWORD wrote = 0;
            const bool saved = f != INVALID_HANDLE_VALUE && WriteFile(f, bin.data(), static_cast<DWORD>(bin.size()), &wrote, nullptr) && wrote == bin.size();
            if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
            if (!saved || !MoveFileExW(tmp.c_str(), exe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
                DeleteFileW(tmp.c_str());
                throw std::runtime_error("Could not save ffmpeg.exe in the tools folder.");
            }
            DeleteFileW(zipPath.c_str());
            return exe;
        } catch (const Cancelled&) { throw; }
        catch (const std::exception& e) {
            lastError = e.what();
            DeleteFileW(zipPath.c_str());
            DeleteFileW((zipPath + L".part").c_str());
        }
    }
    throw std::runtime_error("Could not set up the FFmpeg converter: " + lastError);
}

int run_process(const std::wstring& exe, std::wstring args) {
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    STARTUPINFOW si{}; si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi))
        throw std::runtime_error("Could not start the FFmpeg converter.");
    CloseHandle(pi.hThread);
    while (WaitForSingleObject(pi.hProcess, 100) == WAIT_TIMEOUT) {
        if (dl.cancel) { TerminateProcess(pi.hProcess, 1); WaitForSingleObject(pi.hProcess, 5000); CloseHandle(pi.hProcess); throw Cancelled{}; }
    }
    DWORD code = 1; GetExitCodeProcess(pi.hProcess, &code); CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

void convert_to_mp3(const std::wstring& ffmpeg, const std::wstring& src, const std::wstring& dest, const std::string& label) {
    const std::wstring tmp = dest.substr(0, dest.size() - 4) + L".converting.mp3";
    const std::wstring head = L"-hide_banner -nostdin -loglevel error -y -i \"" + src + L"\" ";
    const std::wstring tail = L" -map_metadata 0 -c:a libmp3lame -q:a 0 -id3v2_version 3 \"" + tmp + L"\"";
    // Keep embedded album art if there is any; fall back to audio only.
    int code = run_process(ffmpeg, head + L"-map 0:a:0 -map 0:v? -c:v copy -disposition:v attached_pic" + tail);
    if (code != 0) code = run_process(ffmpeg, head + L"-map 0:a:0 -vn" + tail);
    if (code != 0 || !file_exists(tmp)) {
        DeleteFileW(tmp.c_str());
        throw std::runtime_error("Could not convert " + label + " to MP3.");
    }
    if (!MoveFileExW(tmp.c_str(), dest.c_str(), MOVEFILE_REPLACE_EXISTING))
        throw std::runtime_error("Could not save the MP3 of " + label + ".");
}

// ---------------------------------------------------------------- worker jobs
struct Job {
    bool load = false;
    std::string id;
    // download
    DlItem item;
    std::vector<DlEntry> selected;
    std::string artist, album;
    bool convert = true, keepFlac = false;
};

void job_load(Job& job) {
    Net net;
    post_status("Looking up the Archive item...");
    const std::string body = http_get_text(net, "https://archive.org/metadata/" + url_escape(job.id, false));
    const dlutil::JVal root = dlutil::parse_json(body);
    const dlutil::JVal* files = root.get("files");
    if (!files || files->t != dlutil::JVal::Arr || files->a.empty())
        throw std::runtime_error("Nothing was found for \"" + job.id + "\". Check the link and try again.");
    auto* item = new DlItem;
    item->id = job.id;
    std::vector<DlEntry> all, originals;
    for (const auto& f : files->a) {
        const std::string name = f.field("name");
        item->allNames.push_back(name);
        bool audio = false;
        for (const char* ext : {".flac", ".mp3", ".ogg", ".wav", ".m4a", ".opus", ".aac", ".wma"}) if (ends_with(name, ext)) audio = true;
        if (!audio) continue;
        DlEntry e; e.name = name; e.size = std::atof(f.field("size").c_str());
        all.push_back(e);
        if (f.field("source") == "original") originals.push_back(e);
    }
    item->audio = originals.empty() ? all : originals;
    std::sort(item->audio.begin(), item->audio.end(), [](const DlEntry& a, const DlEntry& b) { return a.name < b.name; });
    if (item->audio.empty()) { delete item; throw std::runtime_error("This Archive item has no audio files to download."); }
    if (const dlutil::JVal* md = root.get("metadata")) { item->artist = md->field("creator"); item->album = md->field("title"); }
    PostMessageW(dl.hwnd, WM_DL_LOADED, 0, reinterpret_cast<LPARAM>(item));
}

std::string job_download(Job& job) {
    Net net;
    const std::wstring folder = widen(app.libraryRoot) + L"\\" + widen(safe_name(job.artist)) + L"\\" + widen(safe_name(job.album));
    std::wstring folderFixed = folder; for (auto& c : folderFixed) if (c == L'/') c = L'\\';
    make_dirs(folderFixed);
    dl.lastFolder = folderFixed;

    size_t flacs = 0;
    for (const auto& e : job.selected) if (ends_with(e.name, ".flac")) ++flacs;
    std::wstring ffmpeg;
    bool convert = job.convert && flacs;
    if (convert) {
        ffmpeg = find_ffmpeg();
        if (ffmpeg.empty()) {
            if (SendMessageW(dl.hwnd, WM_DL_ASK_FFMPEG, 0, 0) == IDYES) ffmpeg = install_ffmpeg(net);
            else convert = false;
        }
    }

    const double steps = static_cast<double>(job.selected.size() + (convert ? flacs : 0));
    double step = 0;
    size_t done = 0, converted = 0, skipped = 0;
    for (const auto& e : job.selected) {
        const std::string file = safe_name(base_name(e.name));
        const std::wstring dest = folderFixed + L"\\" + widen(file);
        const bool isFlac = ends_with(file, ".flac");
        const std::wstring mp3 = dest.substr(0, dest.size() - 5) + L".mp3";
        const std::string label = "Track " + std::to_string(done + 1) + " of " + std::to_string(job.selected.size()) + ": " + file;
        const bool doConvert = isFlac && convert;

        if (doConvert && file_exists(mp3) && (!job.keepFlac || file_exists(dest))) {
            ++skipped; ++done; step += 2; post_progress(step / steps); continue;
        }
        if (!file_exists(dest)) {
            const double base = step;
            http_download(net, archive_url(job.item.id, e.name), dest, label,
                          [&](double f) { post_progress((base + f) / steps); });
        } else ++skipped;
        step += 1; post_progress(step / steps);

        if (doConvert) {
            if (!file_exists(mp3)) {
                post_status("Converting to MP3 (" + std::to_string(done + 1) + " of " + std::to_string(job.selected.size()) + "): " + file);
                convert_to_mp3(ffmpeg, dest, mp3, file);
                ++converted;
            }
            if (!job.keepFlac) DeleteFileW(dest.c_str());
            step += 1; post_progress(step / steps);
        }
        ++done;
    }

    // Album cover (best effort).
    bool haveCover = false;
    for (const wchar_t* n : {L"cover.jpg", L"cover.jpeg", L"cover.png", L"cover.ppm"}) if (file_exists(folderFixed + L"\\" + n)) haveCover = true;
    if (!haveCover) {
        std::string coverName;
        for (const char* want : {"cover.jpg", "cover.jpeg", "cover.png", "folder.jpg", "front.jpg", "__ia_thumb.jpg", "_itemimage.jpg"}) {
            for (const auto& n : job.item.allNames) if (lower(base_name(n)) == want) { coverName = n; break; }
            if (!coverName.empty()) break;
        }
        try {
            if (!coverName.empty()) {
                const std::string ext = ends_with(coverName, ".png") ? ".png" : ".jpg";
                http_download(net, archive_url(job.item.id, coverName), folderFixed + L"\\cover" + widen(ext), "Album cover", [](double) {});
            } else {
                http_download(net, "https://archive.org/services/img/" + url_escape(job.item.id, false), folderFixed + L"\\cover.jpg", "Album cover", [](double) {});
            }
        } catch (const Cancelled&) { throw; } catch (...) { /* a missing cover is fine */ }
    }

    post_progress(1.0);
    std::string msg = "Done! " + std::to_string(done) + (done == 1 ? " track is" : " tracks are") +
                      " in your library under " + safe_name(job.artist) + " / " + safe_name(job.album) + ".";
    if (converted) msg += " Converted " + std::to_string(converted) + " FLAC to MP3.";
    if (skipped) msg += " (" + std::to_string(skipped) + " already downloaded.)";
    if (flacs && !convert) msg += " FLAC files were kept as FLAC.";
    return msg;
}

DWORD WINAPI worker(LPVOID param) {
    std::unique_ptr<Job> job(static_cast<Job*>(param));
    std::wstring* msg = nullptr; WPARAM kind = 0;
    try {
        if (job->load) job_load(*job);
        else { msg = new std::wstring(widen(job_download(*job))); kind = 1; }
    } catch (const Cancelled&) {
        msg = new std::wstring(job->load ? L"" : L"Download stopped. Press Download again to continue where it left off.");
    } catch (const std::exception& e) {
        msg = new std::wstring(widen(e.what())); kind = 2;
    } catch (...) {
        msg = new std::wstring(L"Something went wrong. Please try again."); kind = 2;
    }
    PostMessageW(dl.hwnd, WM_DL_FINISHED, kind, reinterpret_cast<LPARAM>(msg));
    return 0;
}

// ---------------------------------------------------------------- UI
void dl_set_status(const std::wstring& s, bool error) {
    dl.statusError = error;
    SetWindowTextW(dl.status, s.c_str());
    InvalidateRect(dl.status, nullptr, TRUE);
}
int dl_checked_count() {
    int n = 0;
    for (int i = 0, c = ListView_GetItemCount(dl.list); i < c; ++i) if (ListView_GetCheckState(dl.list, i)) ++n;
    return n;
}
void dl_update_buttons() {
    const int total = ListView_GetItemCount(dl.list);
    SetWindowTextW(dl.select, total && dl_checked_count() == total ? L"Select none" : L"Select all");
    SetWindowTextW(dl.download, dl.busy ? L"Cancel" : L"Download");
    EnableWindow(dl.download, dl.busy || !dl.item.audio.empty());
    EnableWindow(dl.opendir, !dl.lastFolder.empty());
    for (HWND h : {dl.url, dl.find, dl.list, dl.select, dl.artist, dl.album, dl.convert}) EnableWindow(h, !dl.busy);
    EnableWindow(dl.keep, !dl.busy && Button_GetCheck(dl.convert) == BST_CHECKED);
    InvalidateRect(dl.hwnd, nullptr, FALSE);
}
void dl_start(Job* job) {
    dl.cancel = false; dl.busy = true;
    dl_update_buttons();
    if (dl.thread) { CloseHandle(dl.thread); dl.thread = nullptr; }
    dl.thread = CreateThread(nullptr, 0, worker, job, 0, nullptr);
    if (!dl.thread) { delete job; dl.busy = false; dl_update_buttons(); dl_set_status(L"Could not start the download.", true); }
}
void dl_find() {
    if (dl.busy) return;
    const std::string id = item_id(narrow_utf8(dl_text(dl.url).c_str()));
    if (id.empty()) {
        dl_set_status(L"That doesn't look like an Internet Archive link. Paste a link like https://archive.org/details/SOME-ITEM", true);
        SetFocus(dl.url); return;
    }
    auto* job = new Job; job->load = true; job->id = id;
    SendMessageW(dl.progress, PBM_SETPOS, 0, 0);
    dl_start(job);
}
void dl_download() {
    if (dl.busy) { dl.cancel = true; dl_set_status(L"Stopping...", false); return; }
    if (dl.item.audio.empty()) { dl_find(); return; }
    auto* job = new Job;
    job->item = dl.item;
    for (int i = 0, c = ListView_GetItemCount(dl.list); i < c; ++i)
        if (ListView_GetCheckState(dl.list, i)) job->selected.push_back(dl.item.audio[static_cast<size_t>(i)]);
    if (job->selected.empty()) { delete job; dl_set_status(L"Tick at least one track to download.", true); return; }
    job->artist = narrow_utf8(dl_text(dl.artist).c_str());
    job->album = narrow_utf8(dl_text(dl.album).c_str());
    job->convert = Button_GetCheck(dl.convert) == BST_CHECKED;
    job->keepFlac = Button_GetCheck(dl.keep) == BST_CHECKED;
    SendMessageW(dl.progress, PBM_SETPOS, 0, 0);
    dl_set_status(L"Starting download...", false);
    dl_start(job);
}
void dl_show_item(DlItem* item) {
    dl.item = *item; delete item;
    dl.filling = true;
    ListView_DeleteAllItems(dl.list);
    size_t flacs = 0;
    for (size_t i = 0; i < dl.item.audio.size(); ++i) {
        const auto& e = dl.item.audio[i];
        if (ends_with(e.name, ".flac")) ++flacs;
        std::wstring name = widen(base_name(e.name)), size = widen(format_size(e.size));
        LVITEMW it{}; it.mask = LVIF_TEXT; it.iItem = static_cast<int>(i); it.pszText = &name[0];
        SendMessageW(dl.list, LVM_INSERTITEMW, 0, reinterpret_cast<LPARAM>(&it));
        LVITEMW sub{}; sub.iSubItem = 1; sub.pszText = &size[0];
        SendMessageW(dl.list, LVM_SETITEMTEXTW, i, reinterpret_cast<LPARAM>(&sub));
        ListView_SetCheckState(dl.list, static_cast<int>(i), TRUE);
    }
    dl.filling = false;
    SetWindowTextW(dl.artist, widen(dl.item.artist).c_str());
    SetWindowTextW(dl.album, widen(dl.item.album).c_str());
    std::wstring msg = L"Found " + std::to_wstring(dl.item.audio.size()) + L" tracks. They're all ticked - press Download.";
    if (flacs) msg += L" (" + std::to_wstring(flacs) + L" FLAC will be converted to MP3.)";
    dl_set_status(msg, false);
}
void dl_open_folder() {
    const std::wstring target = !dl.lastFolder.empty() && file_exists(dl.lastFolder) ? dl.lastFolder : widen(app.libraryRoot);
    ShellExecuteW(dl.hwnd, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void dl_draw_button(const DRAWITEMSTRUCT* d) {
    const bool primary = d->CtlID == ID_DOWNLOAD || d->CtlID == ID_FIND;
    const bool disabled = d->itemState & ODS_DISABLED, pressed = d->itemState & ODS_SELECTED;
    HBRUSH fillBrush = CreateSolidBrush(pressed ? PANEL : RGB(80, 73, 69));
    FillRect(d->hDC, &d->rcItem, fillBrush); DeleteObject(fillBrush);
    HBRUSH edge = CreateSolidBrush(primary && !disabled ? ACCENT : RGB(80, 73, 69));
    FrameRect(d->hDC, &d->rcItem, edge); DeleteObject(edge);
    if (d->itemState & ODS_FOCUS) { RECT f = d->rcItem; InflateRect(&f, -3, -3); DrawFocusRect(d->hDC, &f); }
    wchar_t text[64]{}; GetWindowTextW(d->hwndItem, text, 64);
    SetBkMode(d->hDC, TRANSPARENT);
    SetTextColor(d->hDC, disabled ? DIM : TEXT);
    SelectObject(d->hDC, app.fButton);
    RECT r = d->rcItem;
    DrawTextW(d->hDC, text, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

LRESULT CALLBACK dl_wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(w);
        const HWND c = reinterpret_cast<HWND>(l);
        SetBkColor(dc, BG);
        SetTextColor(dc, c == dl.status ? (dl.statusError ? ACCENT : DIM) : (IsWindowEnabled(c) ? TEXT : DIM));
        return reinterpret_cast<LRESULT>(dl.bgBrush);
    }
    case WM_CTLCOLOREDIT: {
        HDC dc = reinterpret_cast<HDC>(w);
        SetBkColor(dc, PANEL); SetTextColor(dc, TEXT);
        return reinterpret_cast<LRESULT>(dl.fieldBrush);
    }
    case WM_ERASEBKGND: {
        RECT r; GetClientRect(h, &r); FillRect(reinterpret_cast<HDC>(w), &r, dl.bgBrush);
        return 1;
    }
    case WM_DRAWITEM:
        dl_draw_button(reinterpret_cast<const DRAWITEMSTRUCT*>(l));
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(w)) {
        case IDOK:
            if (GetFocus() == dl.url || dl.item.audio.empty()) dl_find(); else if (!dl.busy) dl_download();
            return 0;
        case IDCANCEL: case ID_CLOSE: SendMessageW(h, WM_CLOSE, 0, 0); return 0;
        case ID_FIND: dl_find(); return 0;
        case ID_DOWNLOAD: dl_download(); return 0;
        case ID_OPENDIR: dl_open_folder(); return 0;
        case ID_CONVERT: dl_update_buttons(); return 0;
        case ID_SELECT: {
            const bool check = dl_checked_count() != ListView_GetItemCount(dl.list);
            for (int i = 0, c = ListView_GetItemCount(dl.list); i < c; ++i) ListView_SetCheckState(dl.list, i, check);
            dl_update_buttons(); return 0;
        }
        case ID_URL:
            if (HIWORD(w) == EN_CHANGE) {
                // Pasting a whole link finds the tracks straight away (typing doesn't).
                const std::wstring t = dl_text(dl.url);
                const int jump = static_cast<int>(t.size()) - dl.lastUrlLen;
                dl.lastUrlLen = static_cast<int>(t.size());
                if (jump > 8 && !dl.busy && t.find(L"archive.org/") != std::wstring::npos) dl_find();
            }
            return 0;
        }
        break;
    case WM_NOTIFY: {
        const NMHDR* n = reinterpret_cast<const NMHDR*>(l);
        if (n->hwndFrom == dl.list && n->code == NM_CLICK) {
            // Clicking anywhere on a row toggles it (not just the tiny checkbox).
            const NMITEMACTIVATE* a = reinterpret_cast<const NMITEMACTIVATE*>(l);
            LVHITTESTINFO hit{}; hit.pt = a->ptAction;
            if (ListView_SubItemHitTest(dl.list, &hit) >= 0 && !(hit.flags & LVHT_ONITEMSTATEICON))
                ListView_SetCheckState(dl.list, hit.iItem, !ListView_GetCheckState(dl.list, hit.iItem));
        } else if (n->hwndFrom == dl.list && n->code == LVN_ITEMCHANGED && !dl.filling) {
            dl_update_buttons();
        }
        return 0;
    }
    case WM_DL_STATUS: {
        std::unique_ptr<std::wstring> s(reinterpret_cast<std::wstring*>(l));
        if (!dl.cancel) dl_set_status(*s, w != 0);
        return 0;
    }
    case WM_DL_PROGRESS:
        SendMessageW(dl.progress, PBM_SETPOS, w, 0);
        return 0;
    case WM_DL_LOADED:
        dl_show_item(reinterpret_cast<DlItem*>(l));
        return 0;
    case WM_DL_ASK_FFMPEG:
        return MessageBoxW(h,
            L"To turn FLAC files into MP3, the downloader needs FFmpeg, a free audio converter.\n\n"
            L"It is downloaded once (about 100 MB) into the \"tools\" folder next to player.exe.\n\n"
            L"Download FFmpeg now?\n\n(Choose No to keep the tracks as FLAC - the player can still play them.)",
            L"Convert FLAC to MP3", MB_YESNO | MB_ICONQUESTION);
    case WM_DL_FINISHED: {
        std::unique_ptr<std::wstring> s(reinterpret_cast<std::wstring*>(l));
        if (dl.thread) { WaitForSingleObject(dl.thread, 5000); CloseHandle(dl.thread); dl.thread = nullptr; }
        dl.busy = false;
        if (s && !s->empty()) dl_set_status(*s, w == 2);
        else if (s) dl_set_status(L"Paste a link above to start.", false);
        if (w == 1) {
            MessageBeep(MB_ICONASTERISK);
            SendMessageW(dl.progress, PBM_SETPOS, 1000, 0);
            app.libraryDirty = true;
            if (app.libraryMode && !app.libraryRoot.empty()) build_library(app.libraryRoot);
        }
        if (!dl.item.audio.empty()) SetFocus(dl.download);
        dl_update_buttons();
        if (dl.closeWhenDone) DestroyWindow(h);
        return 0;
    }
    case WM_CLOSE:
        if (dl.busy) {
            if (MessageBoxW(h, L"A download is still running. Stop it and close?", L"Download music", MB_YESNO | MB_ICONQUESTION) == IDYES) {
                dl.cancel = true; dl.closeWhenDone = true;
            }
            return 0;
        }
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        dl.hwnd = nullptr;
        SetForegroundWindow(app.hwnd);
        return 0;
    }
    return DefWindowProcW(h, m, w, l);
}

HWND dl_control(const wchar_t* cls, const wchar_t* text, DWORD style, int x, int y, int w, int h, int id, DWORD ex = 0) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, x, y, w, h, dl.hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(app.fText), TRUE);
    return c;
}

void open_downloader() {
    if (dl.hwnd) { ShowWindow(dl.hwnd, SW_RESTORE); SetForegroundWindow(dl.hwnd); return; }
    if (app.libraryRoot.empty()) {
        MessageBoxW(app.hwnd, L"The music folder beside player.exe is unavailable. Move the app to a writable folder and restart it.",
                    L"Music folder unavailable", MB_OK | MB_ICONERROR);
        return;
    }
    static bool registered = false;
    if (!registered) {
        INITCOMMONCONTROLSEX icc{sizeof icc, ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&icc);
        WNDCLASSW wc{};
        wc.lpfnWndProc = dl_wndproc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
        wc.lpszClassName = L"player_downloader";
        RegisterClassW(&wc);
        registered = true;
    }
    if (!dl.bgBrush) dl.bgBrush = CreateSolidBrush(BG);
    if (!dl.fieldBrush) dl.fieldBrush = CreateSolidBrush(PANEL);
    dl.item = DlItem(); dl.lastFolder.clear(); dl.busy = false; dl.closeWhenDone = false; dl.lastUrlLen = 0;

    const int CW = 680, CH = 600;
    const DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    RECT r{0, 0, CW, CH}; AdjustWindowRect(&r, style, FALSE);
    RECT owner; GetWindowRect(app.hwnd, &owner);
    const int ww = r.right - r.left, wh = r.bottom - r.top;
    int x = (owner.left + owner.right - ww) / 2, y = (owner.top + owner.bottom - wh) / 2;
    x = std::max(0, std::min(x, GetSystemMetrics(SM_CXSCREEN) - ww));
    y = std::max(0, std::min(y, GetSystemMetrics(SM_CYSCREEN) - wh));
    dl.hwnd = CreateWindowExW(WS_EX_APPWINDOW, L"player_downloader", L"Download music - Internet Archive", style,
                              x, y, ww, wh, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!dl.hwnd) { MessageBoxW(app.hwnd, L"Could not open the downloader window.", L"Download music", MB_OK | MB_ICONERROR); return; }

    const DWORD btn = BS_OWNERDRAW | WS_TABSTOP;
    dl_control(L"STATIC", L"1.  Paste an Internet Archive link (for example https://archive.org/details/...)", 0, 18, 12, 644, 22, -1);
    dl.url = dl_control(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP | WS_BORDER, 18, 38, 504, 28, ID_URL);
    dl.find = dl_control(L"BUTTON", L"Find tracks", btn, 532, 36, 130, 32, ID_FIND);
    dl_control(L"STATIC", L"2.  Tick the tracks you want (click a row to tick or untick it)", 0, 18, 80, 500, 22, -1);
    dl.select = dl_control(L"BUTTON", L"Select all", btn, 532, 74, 130, 30, ID_SELECT);
    dl.list = dl_control(WC_LISTVIEWW, L"", LVS_REPORT | LVS_NOCOLUMNHEADER | LVS_SINGLESEL | LVS_SHOWSELALWAYS | WS_TABSTOP | WS_BORDER,
                         18, 110, 644, 232, ID_LIST);
    ListView_SetExtendedListViewStyle(dl.list, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    ListView_SetBkColor(dl.list, RGB(50, 48, 47));
    ListView_SetTextBkColor(dl.list, RGB(50, 48, 47));
    ListView_SetTextColor(dl.list, TEXT);
    LVCOLUMNW col{}; col.mask = LVCF_WIDTH | LVCF_FMT;
    col.cx = 520; col.fmt = LVCFMT_LEFT; SendMessageW(dl.list, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&col));
    col.cx = 96;  col.fmt = LVCFMT_RIGHT; SendMessageW(dl.list, LVM_INSERTCOLUMNW, 1, reinterpret_cast<LPARAM>(&col));

    dl_control(L"STATIC", L"3.  Check the artist and album names (they name the folder)", 0, 18, 352, 644, 22, -1);
    dl_control(L"STATIC", L"Artist", 0, 18, 378, 300, 20, -1);
    dl.artist = dl_control(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP | WS_BORDER, 18, 400, 312, 28, ID_ARTIST);
    dl_control(L"STATIC", L"Album", 0, 350, 378, 300, 20, -1);
    dl.album = dl_control(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP | WS_BORDER, 350, 400, 312, 28, ID_ALBUM);
    dl.convert = dl_control(L"BUTTON", L"Convert FLAC to MP3 so it plays everywhere on Windows (recommended)",
                            BS_AUTOCHECKBOX | WS_TABSTOP, 18, 438, 644, 24, ID_CONVERT);
    dl.keep = dl_control(L"BUTTON", L"Also keep the original FLAC files", BS_AUTOCHECKBOX | WS_TABSTOP, 40, 462, 500, 24, ID_KEEP);
    Button_SetCheck(dl.convert, BST_CHECKED);

    dl.progress = dl_control(PROGRESS_CLASSW, L"", PBS_SMOOTH, 18, 496, 644, 16, ID_PROGRESS);
    SendMessageW(dl.progress, PBM_SETRANGE32, 0, 1000);
    SendMessageW(dl.progress, PBM_SETBARCOLOR, 0, static_cast<LPARAM>(ACCENT));
    SendMessageW(dl.progress, PBM_SETBKCOLOR, 0, static_cast<LPARAM>(PANEL));
    dl.status = dl_control(L"STATIC", L"Paste a link above and press Enter.", SS_LEFT, 18, 518, 644, 34, ID_STATUS);
    dl.download = dl_control(L"BUTTON", L"Download", btn, 18, 558, 160, 34, ID_DOWNLOAD);
    dl.opendir = dl_control(L"BUTTON", L"Open folder", btn, 188, 558, 130, 34, ID_OPENDIR);
    dl.close = dl_control(L"BUTTON", L"Close", btn, 532, 558, 130, 34, ID_CLOSE);
    dl_update_buttons();

    // If an archive.org link is already copied, use it.
    if (OpenClipboard(dl.hwnd)) {
        if (HANDLE data = GetClipboardData(CF_UNICODETEXT)) {
            if (const wchar_t* text = static_cast<const wchar_t*>(GlobalLock(data))) {
                std::wstring clip = text;
                GlobalUnlock(data);
                if (clip.size() < 2000 && clip.find(L"archive.org/") != std::wstring::npos) {
                    CloseClipboard();
                    while (!clip.empty() && iswspace(clip.back())) clip.pop_back();
                    while (!clip.empty() && iswspace(clip.front())) clip.erase(clip.begin());
                    dl.lastUrlLen = static_cast<int>(clip.size());
                    SetWindowTextW(dl.url, clip.c_str());
                    dl_set_status(L"Using the archive.org link you copied - press Enter or Find tracks.", false);
                    goto shown;
                }
            }
        }
        CloseClipboard();
    }
shown:
    ShowWindow(dl.hwnd, SW_SHOWNORMAL);
    UpdateWindow(dl.hwnd);
    SetForegroundWindow(dl.hwnd);
    SetFocus(dl.url);
    SendMessageW(dl.url, EM_SETSEL, 0, -1);
}

void downloader_shutdown() {
    dl.cancel = true;
    if (dl.thread) { WaitForSingleObject(dl.thread, 3000); CloseHandle(dl.thread); dl.thread = nullptr; }
    if (dl.hwnd) { dl.closeWhenDone = false; DestroyWindow(dl.hwnd); }
}

bool downloader_dialog_message(MSG& msg) {
    return dl.hwnd && IsDialogMessageW(dl.hwnd, &msg);
}
