// dl_util.h - small portable helpers for the built-in downloader:
// a minimal JSON reader (for archive.org metadata) and a raw DEFLATE
// decoder + ZIP entry extractor (for unpacking the FFmpeg download).
// No platform headers, so it can be unit-tested on Linux.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace dlutil {

// ---------------------------------------------------------------- JSON
struct JVal {
    enum Type { Null, Bool, Num, Str, Arr, Obj } t = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<JVal> a;
    std::vector<std::pair<std::string, JVal>> o;

    const JVal* get(const std::string& key) const {
        if (t != Obj) return nullptr;
        for (const auto& kv : o) if (kv.first == key) return &kv.second;
        return nullptr;
    }
    // Text value; arrays give their first element, numbers are formatted.
    std::string text() const {
        if (t == Str) return s;
        if (t == Arr) return a.empty() ? std::string() : a[0].text();
        if (t == Num) { char buf[64]; std::snprintf(buf, sizeof buf, "%.0f", n); return buf; }
        if (t == Bool) return b ? "true" : "false";
        return {};
    }
    std::string field(const std::string& key) const { const JVal* v = get(key); return v ? v->text() : std::string(); }
};

class JsonParser {
public:
    explicit JsonParser(const std::string& src) : s_(src) {}
    JVal parse() { JVal v = value(); ws(); if (i_ != s_.size()) fail(); return v; }

private:
    const std::string& s_;
    size_t i_ = 0;
    int depth_ = 0;

    [[noreturn]] void fail() { throw std::runtime_error("The Internet Archive sent a reply that could not be read."); }
    void ws() { while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) ++i_; }
    char peek() { ws(); if (i_ >= s_.size()) fail(); return s_[i_]; }
    void expect(char c) { if (peek() != c) fail(); ++i_; }
    bool lit(const char* w) { size_t n = std::strlen(w); if (s_.compare(i_, n, w) == 0) { i_ += n; return true; } return false; }

    static void utf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) out += static_cast<char>(cp);
        else if (cp < 0x800) { out += static_cast<char>(0xC0 | (cp >> 6)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { out += static_cast<char>(0xE0 | (cp >> 12)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
        else { out += static_cast<char>(0xF0 | (cp >> 18)); out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); out += static_cast<char>(0x80 | (cp & 0x3F)); }
    }
    uint32_t hex4() {
        if (i_ + 4 > s_.size()) fail();
        uint32_t v = 0;
        for (int k = 0; k < 4; ++k) {
            char c = s_[i_++]; v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else fail();
        }
        return v;
    }
    std::string str() {
        expect('"');
        std::string out;
        while (true) {
            if (i_ >= s_.size()) fail();
            char c = s_[i_++];
            if (c == '"') break;
            if (c != '\\') { out += c; continue; }
            if (i_ >= s_.size()) fail();
            char e = s_[i_++];
            switch (e) {
            case '"': out += '"'; break;   case '\\': out += '\\'; break; case '/': out += '/'; break;
            case 'b': out += '\b'; break;  case 'f': out += '\f'; break;  case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;  case 't': out += '\t'; break;
            case 'u': {
                uint32_t cp = hex4();
                if (cp >= 0xD800 && cp < 0xDC00 && i_ + 6 <= s_.size() && s_[i_] == '\\' && s_[i_ + 1] == 'u') {
                    i_ += 2; uint32_t lo = hex4();
                    if (lo >= 0xDC00 && lo < 0xE000) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    else { utf8(out, 0xFFFD); cp = lo; }
                }
                utf8(out, cp); break;
            }
            default: fail();
            }
        }
        return out;
    }
    JVal value() {
        if (++depth_ > 200) fail();
        JVal v;
        char c = peek();
        if (c == '{') {
            ++i_; v.t = JVal::Obj;
            if (peek() == '}') ++i_;
            else while (true) {
                std::string k = str(); expect(':');
                v.o.emplace_back(std::move(k), value());
                char d = peek(); ++i_;
                if (d == '}') break;
                if (d != ',') fail();
            }
        } else if (c == '[') {
            ++i_; v.t = JVal::Arr;
            if (peek() == ']') ++i_;
            else while (true) {
                v.a.push_back(value());
                char d = peek(); ++i_;
                if (d == ']') break;
                if (d != ',') fail();
            }
        } else if (c == '"') { v.t = JVal::Str; v.s = str(); }
        else if (lit("true")) { v.t = JVal::Bool; v.b = true; }
        else if (lit("false")) { v.t = JVal::Bool; }
        else if (lit("null")) { v.t = JVal::Null; }
        else {
            size_t start = i_;
            while (i_ < s_.size() && std::strchr("+-0123456789.eE", s_[i_])) ++i_;
            if (start == i_) fail();
            v.t = JVal::Num; v.n = std::strtod(s_.substr(start, i_ - start).c_str(), nullptr);
        }
        --depth_;
        return v;
    }
};

inline JVal parse_json(const std::string& text) { return JsonParser(text).parse(); }

// ---------------------------------------------------------------- DEFLATE (RFC 1951)
class Inflater {
public:
    Inflater(const uint8_t* src, size_t len, std::vector<uint8_t>& out) : src_(src), len_(len), out_(out) {}

    void run() {
        int last;
        do {
            last = bits(1);
            int type = bits(2);
            if (type == 0) stored();
            else if (type == 1) fixed();
            else if (type == 2) dynamic();
            else bad();
        } while (!last);
    }

private:
    struct Huff { short count[16]; short symbol[288]; };
    const uint8_t* src_;
    size_t len_, pos_ = 0;
    uint64_t buf_ = 0;
    int cnt_ = 0;
    std::vector<uint8_t>& out_;

    [[noreturn]] static void bad() { throw std::runtime_error("The downloaded archive is damaged."); }
    int bits(int need) {
        while (cnt_ < need) {
            if (pos_ >= len_) bad();
            buf_ |= static_cast<uint64_t>(src_[pos_++]) << cnt_;
            cnt_ += 8;
        }
        int v = static_cast<int>(buf_ & ((1ull << need) - 1));
        buf_ >>= need; cnt_ -= need;
        return v;
    }
    void stored() {
        buf_ = 0; cnt_ = 0;
        if (pos_ + 4 > len_) bad();
        unsigned n = src_[pos_] | (src_[pos_ + 1] << 8);
        unsigned nn = src_[pos_ + 2] | (src_[pos_ + 3] << 8);
        pos_ += 4;
        if (n != (~nn & 0xFFFF) || pos_ + n > len_) bad();
        out_.insert(out_.end(), src_ + pos_, src_ + pos_ + n);
        pos_ += n;
    }
    static void build(Huff& h, const short* lengths, int n) {
        std::memset(h.count, 0, sizeof h.count);
        for (int s = 0; s < n; ++s) h.count[lengths[s]]++;
        short offs[16]; offs[1] = 0;
        for (int l = 1; l < 15; ++l) offs[l + 1] = offs[l] + h.count[l];
        for (int s = 0; s < n; ++s) if (lengths[s]) h.symbol[offs[lengths[s]]++] = static_cast<short>(s);
        h.count[0] = 0;
    }
    int decode(const Huff& h) {
        int code = 0, first = 0, index = 0;
        for (int l = 1; l < 16; ++l) {
            code |= bits(1);
            int count = h.count[l];
            if (code - count < first) return h.symbol[index + (code - first)];
            index += count; first += count; first <<= 1; code <<= 1;
        }
        bad();
    }
    void codes(const Huff& lens, const Huff& dists) {
        static const short lbase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
        static const short lext[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
        static const short dbase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
        static const short dext[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
        while (true) {
            int sym = decode(lens);
            if (sym < 256) { out_.push_back(static_cast<uint8_t>(sym)); continue; }
            if (sym == 256) return;
            sym -= 257;
            if (sym >= 29) bad();
            size_t len = lbase[sym] + bits(lext[sym]);
            int ds = decode(dists);
            if (ds >= 30) bad();
            size_t dist = dbase[ds] + bits(dext[ds]);
            if (dist > out_.size()) bad();
            size_t from = out_.size() - dist;
            for (size_t k = 0; k < len; ++k) out_.push_back(out_[from + k]);
        }
    }
    void fixed() {
        static Huff lens, dists;
        static bool ready = false;
        if (!ready) {
            short l[288];
            int s = 0;
            for (; s < 144; ++s) l[s] = 8;
            for (; s < 256; ++s) l[s] = 9;
            for (; s < 280; ++s) l[s] = 7;
            for (; s < 288; ++s) l[s] = 8;
            build(lens, l, 288);
            for (s = 0; s < 30; ++s) l[s] = 5;
            build(dists, l, 30);
            ready = true;
        }
        codes(lens, dists);
    }
    void dynamic() {
        static const short order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
        int nlen = bits(5) + 257, ndist = bits(5) + 1, ncode = bits(4) + 4;
        if (nlen > 286 || ndist > 30) bad();
        short lengths[320] = {0};
        for (int k = 0; k < ncode; ++k) lengths[order[k]] = static_cast<short>(bits(3));
        Huff lencode, distcode;
        build(lencode, lengths, 19);
        int idx = 0;
        while (idx < nlen + ndist) {
            int sym = decode(lencode);
            if (sym < 16) { lengths[idx++] = static_cast<short>(sym); continue; }
            short val = 0; int rep;
            if (sym == 16) { if (idx == 0) bad(); val = lengths[idx - 1]; rep = 3 + bits(2); }
            else if (sym == 17) rep = 3 + bits(3);
            else rep = 11 + bits(7);
            if (idx + rep > nlen + ndist) bad();
            while (rep--) lengths[idx++] = val;
        }
        if (lengths[256] == 0) bad();
        build(lencode, lengths, nlen);
        build(distcode, lengths + nlen, ndist);
        codes(lencode, distcode);
    }
};

// ---------------------------------------------------------------- ZIP
inline uint32_t rd16(const uint8_t* p) { return p[0] | (p[1] << 8); }
inline uint32_t rd32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }

// Finds the first entry whose name ends with `suffix` (e.g. "bin/ffmpeg.exe")
// in a whole ZIP file held in memory and returns its uncompressed bytes.
inline std::vector<uint8_t> zip_extract(const std::vector<uint8_t>& zip, const std::string& suffix) {
    auto bad = [] { throw std::runtime_error("The downloaded archive is damaged."); };
    if (zip.size() < 22) bad();
    size_t eocd = std::string::npos;
    size_t lowest = zip.size() > 66000 ? zip.size() - 66000 : 0;
    for (size_t p = zip.size() - 22 + 1; p-- > lowest;)
        if (rd32(&zip[p]) == 0x06054b50) { eocd = p; break; }
    if (eocd == std::string::npos) bad();
    uint32_t entries = rd16(&zip[eocd + 10]);
    size_t cd = rd32(&zip[eocd + 16]);
    for (uint32_t e = 0; e < entries; ++e) {
        if (cd + 46 > zip.size() || rd32(&zip[cd]) != 0x02014b50) bad();
        uint32_t method = rd16(&zip[cd + 10]);
        uint32_t csize = rd32(&zip[cd + 20]), usize = rd32(&zip[cd + 24]);
        uint32_t nlen = rd16(&zip[cd + 28]), xlen = rd16(&zip[cd + 30]), clen = rd16(&zip[cd + 32]);
        size_t local = rd32(&zip[cd + 42]);
        if (cd + 46 + nlen > zip.size()) bad();
        std::string name(reinterpret_cast<const char*>(&zip[cd + 46]), nlen);
        cd += 46 + nlen + xlen + clen;
        if (name.size() < suffix.size() || name.compare(name.size() - suffix.size(), suffix.size(), suffix) != 0) continue;
        if (local + 30 > zip.size() || rd32(&zip[local]) != 0x04034b50) bad();
        size_t data = local + 30 + rd16(&zip[local + 26]) + rd16(&zip[local + 28]);
        if (data + csize > zip.size()) bad();
        std::vector<uint8_t> out;
        if (method == 0) out.assign(zip.begin() + data, zip.begin() + data + csize);
        else if (method == 8) { out.reserve(usize); Inflater(&zip[data], csize, out).run(); }
        else throw std::runtime_error("The downloaded archive uses an unsupported compression method.");
        if (out.size() != usize) bad();
        return out;
    }
    throw std::runtime_error(suffix + " was not found in the downloaded archive.");
}

}  // namespace dlutil
