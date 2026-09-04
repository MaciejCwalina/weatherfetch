// weatherfetch - fastfetch-style weather report for your terminal (C++23 port).
// Build:  g++ -O2 -std=c++23 -o weatherfetch weatherfetch.cpp -lcurl
// No API keys. Weather via Open-Meteo, geocoding via Open-Meteo Geocoding,
// IP geolocation via ipapi.co falling back to ip-api.com.
//
// Memory design: no node-based maps anywhere (JSON objects are a flat
// vector of pairs - lookups are linear over a handful of keys), all static
// tables (art, WMO codes, diacritics) are constexpr with string_view access
// and zero heap, the JSON parser works over a string_view of the HTTP body,
// fold() streams UTF-8 without temp buffers, and errors flow through
// std::expected instead of exceptions.

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <curl/curl.h>
#include <expected>
#include <format>
#include <functional>
#include <print>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace wf {

using namespace std::literals;

constexpr int kRequestTimeout = 8;
constexpr std::string_view kUserAgent = "weatherfetch/1.0";

template <typename T>
using Result = std::expected<T, std::string>;

// ---------------------------------------------------------------- colors

inline bool g_color = true;

struct Palette {
    inline static std::string reset = "\x1b[0m";
    inline static std::string bold = "\x1b[1m";
    inline static std::string dim = "\x1b[2m";
    inline static std::string accent, label, value, muted;
    [[nodiscard]] static std::string foreground(int r, int g, int b) {
        return g_color ? std::format("\x1b[38;2;{};{};{}m", r, g, b) : std::string{};
    }
};

inline void disable_color() {
    g_color = false;
    Palette::reset.clear();
    Palette::bold.clear();
    Palette::dim.clear();
}

// visible width: strip ANSI CSI ... m, then count UTF-8 code points. No allocation.
[[nodiscard]] constexpr std::size_t visible_width(std::string_view s) noexcept {
    std::size_t n = 0;
    for (std::size_t i = 0; i < s.size();) {
        if (s[i] == '\x1b' && i + 1 < s.size() && s[i + 1] == '[') {
            std::size_t j = i + 2;
            while (j < s.size() && ((s[j] >= '0' && s[j] <= '9') || s[j] == ';')) ++j;
            if (j < s.size() && s[j] == 'm') {
                i = j + 1;
                continue;
            }
        }
        auto c = static_cast<unsigned char>(s[i]);
        i += (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
        ++n;
    }
    return n;
}

// ---------------------------------------------------------------- JSON
// Minimal DOM: objects are a flat vector of pairs (no per-key node overhead).
// Numbers remember int-ness so printing matches Python str(int)/str(float).

struct Json {
    enum class Type { Null, Bool, Num, Str, Arr, Obj } type = Type::Null;
    bool boolean = false;
    double number = 0.0;
    bool is_int = false; // valid only for Num
    std::string str;     // valid only for Str
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;

    [[nodiscard]] Json* find(std::string_view key) noexcept {
        if (type != Type::Obj) return nullptr;
        auto it = std::ranges::find_if(obj, [&](const auto& kv) { return kv.first == key; });
        return it == obj.end() ? nullptr : &it->second;
    }
    [[nodiscard]] const Json* find(std::string_view key) const noexcept {
        if (type != Type::Obj) return nullptr;
        auto it = std::ranges::find_if(obj, [&](const auto& kv) { return kv.first == key; });
        return it == obj.end() ? nullptr : &it->second;
    }
    [[nodiscard]] std::string get_str(std::string_view key, std::string_view dflt = "") const {
        const Json* v = find(key);
        if (!v || v->type != Type::Str) return std::string{dflt};
        return v->str;
    }
    [[nodiscard]] double get_num(std::string_view key, double dflt = 0.0) const noexcept {
        const Json* v = find(key);
        if (!v) return dflt;
        if (v->type == Type::Num) return v->number;
        if (v->type == Type::Bool) return v->boolean ? 1.0 : 0.0;
        return dflt;
    }
    [[nodiscard]] long long get_id() const noexcept {
        if (type == Type::Num) return static_cast<long long>(number);
        return 0;
    }
};

class Parser {
public:
    explicit Parser(std::string_view in) : in_(in) {}

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    Json parse() { return value(); }

    // Python-style number rendering: ints print bare, floats shortest round-trip.
    [[nodiscard]] static std::string pyfloat(const Json& v) {
        if (v.type == Json::Type::Num && v.is_int)
            return std::to_string(static_cast<long long>(v.number));
        return pyfloat_num(v.number);
    }
    [[nodiscard]] static std::string pyfloat_num(double d) {
        if (d == 0) d = 0; // normalize -0
        std::string s = std::format("{:.15g}", d);
        if (s.find_first_of(".eE") == std::string::npos) s += ".0";
        return s;
    }

private:
    std::string_view in_;
    std::size_t pos_ = 0;
    bool ok_ = true;

    void ws() noexcept {
        while (pos_ < in_.size() && (in_[pos_] == ' ' || in_[pos_] == '\t' || in_[pos_] == '\n' || in_[pos_] == '\r'))
            ++pos_;
    }
    bool consume(std::string_view lit) {
        if (in_.substr(pos_, lit.size()) == lit) {
            pos_ += lit.size();
            return true;
        }
        ok_ = false;
        return false;
    }
    static void encode_utf8(std::string& out, unsigned cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    [[nodiscard]] static unsigned hex4(std::string_view s, std::size_t p, bool& good) {
        unsigned cp = 0;
        for (int k = 0; k < 4; ++k) {
            char h = (p + k < s.size()) ? s[p + k] : 0;
            cp <<= 4;
            if (h >= '0' && h <= '9') cp += static_cast<unsigned>(h - '0');
            else if (h >= 'a' && h <= 'f') cp += static_cast<unsigned>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') cp += static_cast<unsigned>(h - 'A' + 10);
            else good = false;
        }
        return cp;
    }

    [[nodiscard]] std::string parse_string() {
        std::string out;
        ++pos_; // opening quote
        while (pos_ < in_.size() && in_[pos_] != '"') {
            if (in_[pos_] == '\\') {
                ++pos_;
                if (pos_ >= in_.size()) break;
                char e = in_[pos_++];
                switch (e) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'n': out += '\n'; break;
                    case 'r': out += '\r'; break;
                    case 't': out += '\t'; break;
                    case 'u': {
                        bool good = true;
                        unsigned cp = hex4(in_, pos_, good);
                        pos_ += 4;
                        if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 1 < in_.size() && in_[pos_] == '\\' &&
                            in_[pos_ + 1] == 'u') {
                            unsigned lo = hex4(in_, pos_ + 2, good);
                            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                                pos_ += 6;
                            }
                        }
                        if (!good) ok_ = false;
                        encode_utf8(out, cp);
                        break;
                    }
                    default: out += e; break;
                }
            } else {
                out += in_[pos_++];
            }
        }
        if (pos_ < in_.size()) ++pos_; // closing quote
        return out;
    }

    Json parse_number() {
        Json j;
        j.type = Json::Type::Num;
        std::size_t start = pos_;
        while (pos_ < in_.size() &&
               ((in_[pos_] >= '0' && in_[pos_] <= '9') || in_[pos_] == '-' || in_[pos_] == '+' ||
                in_[pos_] == '.' || in_[pos_] == 'e' || in_[pos_] == 'E'))
            ++pos_;
        std::string_view lex = in_.substr(start, pos_ - start);
        j.is_int = lex.find_first_of(".eE") == std::string_view::npos;
        auto res = std::from_chars(lex.data(), lex.data() + lex.size(), j.number);
        if (res.ec != std::errc()) {
            j.number = 0;
            j.is_int = true;
        }
        return j;
    }

    Json value() {
        ws();
        if (pos_ >= in_.size()) {
            ok_ = false;
            return {};
        }
        char c = in_[pos_];
        if (c == '{') {
            Json j;
            j.type = Json::Type::Obj;
            ++pos_;
            ws();
            if (pos_ < in_.size() && in_[pos_] == '}') {
                ++pos_;
                return j;
            }
            while (true) {
                ws();
                if (pos_ >= in_.size() || in_[pos_] != '"') {
                    ok_ = false;
                    break;
                }
                std::string key = parse_string();
                ws();
                if (pos_ >= in_.size() || in_[pos_] != ':') {
                    ok_ = false;
                    break;
                }
                ++pos_;
                j.obj.emplace_back(std::move(key), value());
                ws();
                if (pos_ < in_.size() && in_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                if (pos_ < in_.size() && in_[pos_] == '}') {
                    ++pos_;
                    break;
                }
                ok_ = false;
                break;
            }
            return j;
        }
        if (c == '[') {
            Json j;
            j.type = Json::Type::Arr;
            ++pos_;
            ws();
            if (pos_ < in_.size() && in_[pos_] == ']') {
                ++pos_;
                return j;
            }
            while (true) {
                j.arr.push_back(value());
                ws();
                if (pos_ < in_.size() && in_[pos_] == ',') {
                    ++pos_;
                    continue;
                }
                if (pos_ < in_.size() && in_[pos_] == ']') {
                    ++pos_;
                    break;
                }
                ok_ = false;
                break;
            }
            return j;
        }
        if (c == '"') {
            Json j;
            j.type = Json::Type::Str;
            j.str = parse_string();
            return j;
        }
        if (c == 't') {
            Json j;
            j.type = Json::Type::Bool;
            j.boolean = true;
            consume("true");
            return j;
        }
        if (c == 'f') {
            Json j;
            j.type = Json::Type::Bool;
            j.boolean = false;
            consume("false");
            return j;
        }
        if (c == 'n') {
            consume("null");
            return {};
        }
        return parse_number();
    }
};

// ---------------------------------------------------------------- http

std::size_t curl_write(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    auto* body = static_cast<std::string*>(userdata);
    body->append(ptr, size * nmemb);
    return size * nmemb;
}

// One easy handle per run: connections (and TLS sessions) are reused across
// requests to the same host, so repeat hits skip TCP+TLS setup entirely.
struct Client {
    CURL* h = nullptr;
    struct curl_slist* hdrs = nullptr;
    std::string ua;

    Client() = default;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    [[nodiscard]] bool init() {
        h = curl_easy_init();
        if (!h) return false;
        ua = std::format("User-Agent: {}", kUserAgent);
        hdrs = curl_slist_append(nullptr, ua.c_str());
        curl_easy_setopt(h, CURLOPT_HTTPHEADER, hdrs);
        curl_easy_setopt(h, CURLOPT_WRITEFUNCTION, curl_write);
        curl_easy_setopt(h, CURLOPT_TIMEOUT, kRequestTimeout);
        curl_easy_setopt(h, CURLOPT_CONNECTTIMEOUT, kRequestTimeout);
        curl_easy_setopt(h, CURLOPT_FOLLOWLOCATION, 1L);
        curl_easy_setopt(h, CURLOPT_ACCEPT_ENCODING, ""); // all built-in compressions
        curl_easy_setopt(h, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_2TLS);
        curl_easy_setopt(h, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(h, CURLOPT_TCP_FASTOPEN, 1L);
        return true;
    }
    ~Client() {
        if (hdrs) curl_slist_free_all(hdrs);
        if (h) curl_easy_cleanup(h);
    }

    [[nodiscard]] Result<std::string> get(std::string_view url) {
        std::string body;
        body.reserve(4096);
        std::string url_str{url}; // curl needs a stable C string
        curl_easy_setopt(h, CURLOPT_URL, url_str.c_str());
        curl_easy_setopt(h, CURLOPT_WRITEDATA, &body);
        CURLcode rc = curl_easy_perform(h);
        long status = 0;
        if (rc == CURLE_OK) curl_easy_getinfo(h, CURLINFO_RESPONSE_CODE, &status);
        if (rc != CURLE_OK)
            return std::unexpected(std::format("network error: {}", curl_easy_strerror(rc)));
        if (status >= 400) return std::unexpected(std::format("network error: HTTP {}", status));
        return body;
    }
};

[[nodiscard]] Result<Json> get_json(Client& c, std::string_view url) {
    auto body = c.get(url);
    if (!body) return std::unexpected(body.error());
    Parser pr(*body); // parses over a view of body; values are moved out, body freed after
    Json j = pr.parse();
    if (!pr.ok()) return std::unexpected("network error: invalid JSON response");
    return j;
}

[[nodiscard]] constexpr bool is_unreserved(unsigned char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
           c == '_' || c == '.' || c == '~';
}

[[nodiscard]] std::string url_encode(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s)
        if (is_unreserved(c))
            out += static_cast<char>(c);
        else
            std::format_to(std::back_inserter(out), "%{:02X}", c);
    return out;
}

// ---------------------------------------------------------------- unicode fold
// Streams code points straight off the input bytes; no temp code-point buffer.

struct BaseLetter {
    unsigned cp;
    std::string_view base;
};

// Sorted by cp; order enforced by static_assert below.
constexpr auto kBase = std::to_array<BaseLetter>({
    {0x00C0, "A"}, {0x00C1, "A"}, {0x00C2, "A"}, {0x00C3, "A"}, {0x00C4, "A"}, {0x00C5, "A"},
    {0x00C6, "AE"}, {0x00C7, "C"}, {0x00C8, "E"}, {0x00C9, "E"}, {0x00CA, "E"}, {0x00CB, "E"},
    {0x00CC, "I"}, {0x00CD, "I"}, {0x00CE, "I"}, {0x00CF, "I"}, {0x00D0, "D"}, {0x00D1, "N"},
    {0x00D2, "O"}, {0x00D3, "O"}, {0x00D4, "O"}, {0x00D5, "O"}, {0x00D6, "O"}, {0x00D8, "O"},
    {0x00D9, "U"}, {0x00DA, "U"}, {0x00DB, "U"}, {0x00DC, "U"}, {0x00DD, "Y"}, {0x00DE, "TH"},
    {0x00DF, "ss"}, {0x00E0, "a"}, {0x00E1, "a"}, {0x00E2, "a"}, {0x00E3, "a"}, {0x00E4, "a"},
    {0x00E5, "a"}, {0x00E6, "ae"}, {0x00E7, "c"}, {0x00E8, "e"}, {0x00E9, "e"}, {0x00EA, "e"},
    {0x00EB, "e"}, {0x00EC, "i"}, {0x00ED, "i"}, {0x00EE, "i"}, {0x00EF, "i"}, {0x00F0, "d"},
    {0x00F1, "n"}, {0x00F2, "o"}, {0x00F3, "o"}, {0x00F4, "o"}, {0x00F5, "o"}, {0x00F6, "o"},
    {0x00F8, "o"}, {0x00F9, "u"}, {0x00FA, "u"}, {0x00FB, "u"}, {0x00FC, "u"}, {0x00FD, "y"},
    {0x00FE, "th"}, {0x00FF, "y"}, {0x0100, "A"}, {0x0101, "a"}, {0x0102, "A"}, {0x0103, "a"},
    {0x0104, "A"}, {0x0105, "a"}, {0x0106, "C"}, {0x0107, "c"}, {0x0108, "C"}, {0x0109, "c"},
    {0x010A, "C"}, {0x010B, "c"}, {0x010C, "C"}, {0x010D, "c"}, {0x010E, "D"}, {0x010F, "d"},
    {0x0110, "D"}, {0x0111, "d"}, {0x0112, "E"}, {0x0113, "e"}, {0x0114, "E"}, {0x0115, "e"},
    {0x0116, "E"}, {0x0117, "e"}, {0x0118, "E"}, {0x0119, "e"}, {0x011A, "E"}, {0x011B, "e"},
    {0x011C, "G"}, {0x011D, "g"}, {0x011E, "G"}, {0x011F, "g"}, {0x0120, "G"}, {0x0121, "g"},
    {0x0122, "G"}, {0x0123, "g"}, {0x0124, "H"}, {0x0125, "h"}, {0x0126, "H"}, {0x0127, "h"},
    {0x0128, "I"}, {0x0129, "i"}, {0x012A, "I"}, {0x012B, "i"}, {0x012C, "I"}, {0x012D, "i"},
    {0x012E, "I"}, {0x012F, "i"}, {0x0130, "I"}, {0x0131, "i"}, {0x0134, "J"}, {0x0135, "j"},
    {0x0136, "K"}, {0x0137, "k"}, {0x0139, "L"}, {0x013A, "l"}, {0x013B, "L"}, {0x013C, "l"},
    {0x013D, "L"}, {0x013E, "l"}, {0x013F, "L"}, {0x0140, "l"}, {0x0141, "L"}, {0x0142, "l"},
    {0x0143, "N"}, {0x0144, "n"}, {0x0145, "N"}, {0x0146, "n"}, {0x0147, "N"}, {0x0148, "n"},
    {0x0149, "n"}, {0x014B, "N"}, {0x014C, "O"}, {0x014D, "o"}, {0x014E, "O"}, {0x014F, "o"},
    {0x0150, "O"}, {0x0151, "o"}, {0x0152, "OE"}, {0x0153, "oe"}, {0x0154, "R"}, {0x0155, "r"},
    {0x0156, "R"}, {0x0157, "r"}, {0x0158, "R"}, {0x0159, "r"}, {0x015A, "S"}, {0x015B, "s"},
    {0x015C, "S"}, {0x015D, "s"}, {0x015E, "S"}, {0x015F, "s"}, {0x0160, "S"}, {0x0161, "s"},
    {0x0162, "T"}, {0x0163, "t"}, {0x0164, "T"}, {0x0165, "t"}, {0x0166, "T"}, {0x0167, "t"},
    {0x0168, "U"}, {0x0169, "u"}, {0x016A, "U"}, {0x016B, "u"}, {0x016C, "U"}, {0x016D, "u"},
    {0x016E, "U"}, {0x016F, "u"}, {0x0170, "U"}, {0x0171, "u"}, {0x0172, "U"}, {0x0173, "u"},
    {0x0174, "W"}, {0x0175, "w"}, {0x0176, "Y"}, {0x0177, "y"}, {0x0178, "Y"}, {0x0179, "Z"},
    {0x017A, "z"}, {0x017B, "Z"}, {0x017C, "z"}, {0x017D, "Z"}, {0x017E, "z"}, {0x01CD, "A"},
    {0x01CE, "a"}, {0x0200, "A"}, {0x0201, "a"}, {0x0202, "A"}, {0x0203, "a"},
});

consteval bool base_sorted() {
    for (std::size_t i = 1; i < kBase.size(); ++i)
        if (kBase[i - 1].cp >= kBase[i].cp) return false;
    return true;
}
static_assert(base_sorted(), "kBase must be sorted by code point");

[[nodiscard]] std::string_view base_lookup(unsigned cp) noexcept {
    auto it = std::ranges::lower_bound(kBase, cp, {}, &BaseLetter::cp);
    return (it != kBase.end() && it->cp == cp) ? it->base : std::string_view{};
}

[[nodiscard]] std::string trim(std::string_view s) {
    std::size_t a = 0, b = s.size();
    while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) ++a;
    while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r'))
        --b;
    return std::string{s.substr(a, b - a)};
}

[[nodiscard]] std::string ascii_lower(std::string_view s) {
    std::string out{s};
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return out;
}

// Casefold + strip diacritics so 'malmo' matches 'Malmö'. Byte-safe for UTF-8.
[[nodiscard]] std::string fold(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size();) {
        auto c = static_cast<unsigned char>(text[i]);
        unsigned cp;
        std::size_t adv;
        if (c < 0x80) {
            cp = c;
            adv = 1;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < text.size()) {
            cp = (static_cast<unsigned>(c & 0x1F) << 6) | (static_cast<unsigned char>(text[i + 1]) & 0x3F);
            adv = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < text.size()) {
            cp = (static_cast<unsigned>(c & 0x0F) << 12) |
                 ((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(text[i + 2]) & 0x3F);
            adv = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < text.size()) {
            cp = (static_cast<unsigned>(c & 0x07) << 18) |
                 ((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 12) |
                 ((static_cast<unsigned char>(text[i + 2]) & 0x3F) << 6) |
                 (static_cast<unsigned char>(text[i + 3]) & 0x3F);
            adv = 4;
        } else {
            ++i; // stray continuation byte
            continue;
        }
        i += adv;
        if (cp >= 0x0300 && cp <= 0x036F) continue; // combining marks
        if (std::string_view base = base_lookup(cp); !base.empty()) {
            out += base;
        } else if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else {
            out.append(text.data() + (i - adv), adv); // keep other scripts as-is
        }
    }
    return trim(ascii_lower(out));
}

// ---------------------------------------------------------------- weather codes

enum class Art : std::uint8_t { Sun, Moon, Partly, Cloud, Fog, Rain, Snow, Storm };

struct WmoInfo {
    std::string_view desc;
    Art art;
    std::uint8_t r, g, b;
};

[[nodiscard]] constexpr WmoInfo wmo_lookup(int code) noexcept {
    // Zero-memory table: a switch, so nothing is stored at all.
    switch (code) {
        case 0: return {"Clear sky", Art::Sun, 250, 204, 21};
        case 1: return {"Mainly clear", Art::Sun, 250, 204, 21};
        case 2: return {"Partly cloudy", Art::Partly, 148, 187, 233};
        case 3: return {"Overcast", Art::Cloud, 156, 163, 175};
        case 45:
        case 48: return {code == 45 ? "Fog" : "Depositing rime fog", Art::Fog, 148, 163, 184};
        case 51: return {"Light drizzle", Art::Rain, 96, 165, 250};
        case 53: return {"Moderate drizzle", Art::Rain, 96, 165, 250};
        case 55: return {"Dense drizzle", Art::Rain, 96, 165, 250};
        case 56:
        case 57: return {"Freezing drizzle", Art::Rain, 125, 211, 252};
        case 61: return {"Slight rain", Art::Rain, 59, 130, 246};
        case 63: return {"Moderate rain", Art::Rain, 59, 130, 246};
        case 65: return {"Heavy rain", Art::Rain, 37, 99, 235};
        case 66: return {"Freezing rain", Art::Rain, 125, 211, 252};
        case 67: return {"Heavy freezing rain", Art::Rain, 125, 211, 252};
        case 71: return {"Slight snow", Art::Snow, 224, 242, 254};
        case 73: return {"Moderate snow", Art::Snow, 224, 242, 254};
        case 75: return {"Heavy snow", Art::Snow, 224, 242, 254};
        case 77: return {"Snow grains", Art::Snow, 224, 242, 254};
        case 80: return {"Slight showers", Art::Rain, 59, 130, 246};
        case 81: return {"Moderate showers", Art::Rain, 59, 130, 246};
        case 82: return {"Violent showers", Art::Rain, 37, 99, 235};
        case 85: return {"Slight snow showers", Art::Snow, 224, 242, 254};
        case 86: return {"Heavy snow showers", Art::Snow, 224, 242, 254};
        case 95: return {"Thunderstorm", Art::Storm, 167, 139, 250};
        case 96:
        case 99: return {"Thunderstorm, hail", Art::Storm, 167, 139, 250};
        default: return {"Unknown", Art::Cloud, 156, 163, 175};
    }
}

struct Desc {
    std::string_view text;
    Art art;
    std::uint8_t r, g, b;
};

[[nodiscard]] constexpr Desc describe(int code, bool daytime) noexcept {
    WmoInfo w = wmo_lookup(code);
    if (w.art == Art::Sun && !daytime)
        return {code == 0 ? "Clear night" : w.desc, Art::Moon, 203, 213, 225};
    return {w.desc, w.art, w.r, w.g, w.b};
}

[[nodiscard]] constexpr std::string_view glyph(Art a) noexcept {
    switch (a) {
        case Art::Sun: return "☀";
        case Art::Moon: return "☽";
        case Art::Partly: return "⛅";
        case Art::Cloud: return "☁";
        case Art::Fog: return "░";
        case Art::Rain: return "☔";
        case Art::Snow: return "❄";
        case Art::Storm: return "⚡";
    }
    std::unreachable();
}

constexpr std::array<std::string_view, 7> kArtSun = {
    "        \\   |   /", "          .-''-.", "   ---   /      \\   ---", "        |        |",
    "   ---   \\      /   ---", "          `-..-`", "        /   |   \\"};
constexpr std::array<std::string_view, 7> kArtMoon = {
    "         *      .", "           .-''-.", "      .   /      `.   *", "         |         )",
    "     *    \\       /", "           `-...-`     .", "        .       *"};
constexpr std::array<std::string_view, 7> kArtPartly = {
    "      \\  |  /", "       .-''-.", "     -(      )-  .--.", "       `-..-`  .(    ).",
    "        (   .-(       ).", "         `-(            )-'", "            `--..____.--'"};
constexpr std::array<std::string_view, 6> kArtCloud = {
    "             .--.", "          .-(    ).", "       .-(         ).", "     -(               )-",
    "       `-(          )-'", "          `--..___.-'"};
constexpr std::array<std::string_view, 6> kArtFog = {
    "      _ - _ - _ - _ -", "    - _ - _ - _ - _ -", "      _ - _ - _ - _ -",
    "    - _ - _ - _ - _ -", "      _ - _ - _ - _ -", "    - _ - _ - _ - _ -"};
constexpr std::array<std::string_view, 8> kArtRain = {
    "             .--.", "          .-(    ).", "       .-(         ).", "     -(               )-",
    "       `-(          )-'", "          `--..___.-'", "        ' , ' , ' , ' ,", "       , ' , ' , ' , '"};
constexpr std::array<std::string_view, 8> kArtSnow = {
    "             .--.", "          .-(    ).", "       .-(         ).", "     -(               )-",
    "       `-(          )-'", "          `--..___.-'", "         *   *   *   *", "       *   *   *   *"};
constexpr std::array<std::string_view, 8> kArtStorm = {
    "             .--.", "          .-(    ).", "       .-(         ).", "     -(               )-",
    "       `-(          )-'", "          `--..___.-'", "          /_/  /_/", "           /    /"};

[[nodiscard]] constexpr std::span<const std::string_view> art_lines(Art a) noexcept {
    switch (a) {
        case Art::Sun: return kArtSun;
        case Art::Moon: return kArtMoon;
        case Art::Partly: return kArtPartly;
        case Art::Cloud: return kArtCloud;
        case Art::Fog: return kArtFog;
        case Art::Rain: return kArtRain;
        case Art::Snow: return kArtSnow;
        case Art::Storm: return kArtStorm;
    }
    std::unreachable();
}

// ---------------------------------------------------------------- geo + weather

struct Place {
    double lat = 0, lon = 0;
    std::string name, region;
};

[[nodiscard]] Result<std::vector<Json>> search_places(Client& c, std::string_view name, std::string_view cc) {
    std::string q = std::format("name={}&count=20&language=en", url_encode(name));
    if (!cc.empty()) q += std::format("&countryCode={}", url_encode(cc));
    auto j = get_json(c, std::format("https://geocoding-api.open-meteo.com/v1/search?{}", q));
    if (!j) return std::unexpected(j.error());
    Json* r = j->find("results");
    if (!r || r->type != Json::Type::Arr) return std::vector<Json>{};
    return std::move(r->arr);
}

// Accented spellings of `name` hiding in the results: an ASCII query can miss
// the city and match only its airport, but that record still names its district
// in the local script - the spelling that does find the city.
[[nodiscard]] std::vector<std::string> native_spellings(std::span<const Json> places, std::string_view name) {
    std::string target = fold(name);
    std::string lname = ascii_lower(trim(name));
    constexpr std::array<std::string_view, 4> kFields = {"name", "admin3", "admin2", "admin1"};
    std::vector<std::string> out;
    for (const Json& pl : places)
        for (std::string_view f : kFields) {
            std::string cand = trim(pl.get_str(f));
            if (!cand.empty() && fold(cand) == target && ascii_lower(cand) != lname &&
                std::ranges::find(out, cand) == out.end())
                out.push_back(std::move(cand));
        }
    return out;
}

[[nodiscard]] Result<Place> geocode(Client& c, std::string_view query) {
    std::size_t cm = query.find(',');
    std::string name = trim(cm == std::string_view::npos ? query : query.substr(0, cm));
    std::string country = trim(cm == std::string_view::npos ? std::string_view{} : query.substr(cm + 1));
    if (name.empty()) return std::unexpected("empty location");

    std::string cc; // the API filters server-side on a 2-letter code, which beats guessing
    if (country.size() == 2 && ((country[0] >= 'a' && country[0] <= 'z') || (country[0] >= 'A' && country[0] <= 'Z')) &&
        ((country[1] >= 'a' && country[1] <= 'z') || (country[1] >= 'A' && country[1] <= 'Z'))) {
        cc = ascii_lower(country);
        cc[0] = static_cast<char>(cc[0] - 32);
        cc[1] = static_cast<char>(cc[1] - 32);
    }
    if (cc.empty() && !country.empty()) {
        // Common demonyms the API won't take as countryCode ("usa" is 3 letters).
        std::string low = ascii_lower(country);
        if (low == "usa") cc = "US";
        else if (low == "uk") cc = "GB";
    }
    auto places = search_places(c, name, cc);
    if (!places) return std::unexpected(places.error());

    // Retry under any local spelling the first pass revealed (malmo -> Malmö).
    for (std::string_view spelling : native_spellings(*places, name) | std::views::take(2)) {
        std::vector<long long> known;
        known.reserve(places->size());
        for (const Json& pl : *places)
            if (const Json* id = pl.find("id")) known.push_back(id->get_id());
        auto more = search_places(c, spelling, cc);
        if (!more) return std::unexpected(more.error());
        for (Json& pl : *more) {
            long long id = pl.find("id") ? pl.find("id")->get_id() : 0;
            if (std::ranges::find(known, id) == known.end()) places->push_back(std::move(pl));
        }
    }
    if (!country.empty() && cc.empty()) {
        std::string want = fold(country);
        std::vector<Json> filtered;
        filtered.reserve(places->size());
        std::ranges::copy_if(*places, std::back_inserter(filtered),
                             [&](const Json& pl) { return fold(pl.get_str("country")) == want; });
        *places = std::move(filtered);
    }
    if (places->empty()) {
        std::string q = country.empty() ? "" : std::format(" in '{}'", country);
        return std::unexpected(std::format("no place found matching '{}'{}", name, q));
    }
    // Rank populated places above airports and admin regions, then exact name,
    // then size - the raw API order puts a 76-person hamlet above a city.
    std::string target = fold(name);
    auto rank = [&](const Json& pl) {
        std::string_view fc = pl.find("feature_code") ? std::string_view{pl.find("feature_code")->str} : ""sv;
        bool ppl = fc.starts_with("PPL");
        bool exact = fold(pl.get_str("name")) == target;
        return std::make_tuple(ppl, exact, pl.get_num("population"));
    };
    std::ranges::sort(*places, std::less<>{}, rank); // ascending; best is back()
    const Json& best = places->back();
    std::string a1 = best.get_str("admin1"), cn = best.get_str("country");
    std::string region = !a1.empty() && !cn.empty() ? std::format("{}, {}", a1, cn) : (!a1.empty() ? a1 : cn);
    return Place{best.get_num("latitude"), best.get_num("longitude"), best.get_str("name"), region};
}

[[nodiscard]] Result<Place> locate_by_ip(Client& c) {
    if (auto l = get_json(c, "https://ipapi.co/json/")) {
        if (const Json* la = l->find("latitude"); la && la->type == Json::Type::Num)
            return Place{l->get_num("latitude"), l->get_num("longitude"), l->get_str("city", "Unknown"),
                         l->get_str("country_name")};
    }
    auto l = get_json(c, "http://ip-api.com/json/");
    if (!l) return std::unexpected(l.error());
    if (l->get_str("status") != "success") return std::unexpected("IP geolocation failed");
    return Place{l->get_num("lat"), l->get_num("lon"), l->get_str("city", "Unknown"), l->get_str("country")};
}

[[nodiscard]] Result<Json> fetch_weather(Client& c, double lat, double lon, std::string_view units) {
    std::string q = std::format(
        "latitude={}&longitude={}&current={}&daily={}&timezone=auto&forecast_days=4", lat, lon,
        url_encode("temperature_2m,relative_humidity_2m,apparent_temperature,is_day,precipitation,"
                   "weather_code,cloud_cover,pressure_msl,wind_speed_10m,wind_direction_10m,wind_gusts_10m"),
        url_encode("weather_code,temperature_2m_max,temperature_2m_min,sunrise,sunset,uv_index_max,"
                   "precipitation_probability_max"));
    if (units == "f") q += "&temperature_unit=fahrenheit&wind_speed_unit=mph&precipitation_unit=inch";
    return get_json(c, std::format("https://api.open-meteo.com/v1/forecast?{}", q));
}

// ---------------------------------------------------------------- formatting

constexpr std::array<std::string_view, 8> kArrows = {"↓", "↙", "←", "↖", "↑", "↗", "→", "↘"};
constexpr std::array<std::string_view, 8> kPoints = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};

// Compass point the wind comes from, and the arrow it blows toward.
[[nodiscard]] std::pair<std::string_view, std::string_view> compass(double deg) noexcept {
    double m = std::fmod(deg, 360.0);
    if (m < 0) m += 360.0;
    return {kPoints[static_cast<int>(m / 45.0 + 0.5) % 8], kArrows[static_cast<int>(m / 45.0 + 0.5) % 8]};
}

// A bar that still reads correctly with color stripped.
[[nodiscard]] std::string meter(double frac, int width = 10) {
    frac = std::clamp(frac, 0.0, 1.0);
    int filled = static_cast<int>(std::nearbyint(frac * width)); // python round() is half-even
    std::string_view empty = g_color ? "━" : "╌";
    std::string s;
    s.reserve(static_cast<std::size_t>(width) * 3 + Palette::dim.size() + Palette::reset.size());
    for (int i = 0; i < filled; ++i) s += "━";
    s += Palette::dim;
    for (int i = 0; i < width - filled; ++i) s += empty;
    s += Palette::reset;
    return s;
}

[[nodiscard]] std::string clock_time(std::string_view ts) {
    if (std::size_t t = ts.find('T'); t != std::string_view::npos && t + 6 <= ts.size())
        return std::string{ts.substr(t + 1, 5)};
    return std::string{ts.size() >= 5 ? ts.substr(ts.size() - 5) : ts};
}

[[nodiscard]] std::string day_label(std::string_view ts, int off) {
    if (off == 0) return "Today";
    int y = 0, mo = 0, d = 0;
    std::string tmp{ts}; // sscanf needs a C string
    if (std::sscanf(tmp.c_str(), "%d-%d-%d", &y, &mo, &d) == 3) {
        std::tm tm{};
        tm.tm_year = y - 1900;
        tm.tm_mon = mo - 1;
        tm.tm_mday = d;
        tm.tm_isdst = -1;
        if (std::mktime(&tm) != -1) {
            char b[16];
            std::strftime(b, sizeof b, "%a", &tm);
            return b;
        }
    }
    return std::string{ts};
}

[[nodiscard]] constexpr std::string_view uv_word(double uv) noexcept {
    if (uv < 3) return "low";
    if (uv < 6) return "moderate";
    if (uv < 8) return "high";
    if (uv < 11) return "very high";
    return "extreme";
}

// ---------------------------------------------------------------- report

struct Rows {
    Art art;
    std::uint8_t r, g, b;
    std::vector<std::string> rows;
};

[[nodiscard]] Rows build_rows(const Json& w, std::string_view place, std::string_view region) {
    const Json& cur = *w.find("current");
    const Json& cun = *w.find("current_units");
    const Json& daily = *w.find("daily");
    Desc dc = describe(static_cast<int>(cur.get_num("weather_code")), cur.get_num("is_day") != 0);

    Palette::accent = Palette::foreground(dc.r, dc.g, dc.b);
    Palette::label = Palette::foreground(dc.r, dc.g, dc.b);
    Palette::value = Palette::foreground(226, 232, 240);
    Palette::muted = Palette::foreground(120, 130, 145);

    std::string degu = cun.get_str("temperature_2m");
    std::string spdu = cun.get_str("wind_speed_10m");
    if (std::size_t p = spdu.find("mp/h"); p != std::string::npos) spdu.replace(p, 4, "mph");

    std::string title = std::format("{}{}{}", Palette::bold + Palette::accent, place, Palette::reset);
    if (!region.empty()) title += std::format("{}@{}{}", Palette::muted, region, Palette::reset);

    std::vector<std::string> rows;
    rows.reserve(20);
    rows.push_back(title);
    std::string rule = Palette::accent;
    for (std::size_t i = 0, n = visible_width(title); i < n; ++i) rule += "─";
    rows.push_back(rule + Palette::reset);

    auto row = [&](std::string_view lab, std::string_view val) {
        rows.push_back(std::format("{}{}{}{}{}{}{}", Palette::bold + Palette::label, lab, Palette::reset,
                                   Palette::muted + ": ", Palette::reset, Palette::value + std::string{val},
                                   Palette::reset));
    };
    auto num = [&](const Json& o, std::string_view k) -> std::string {
        const Json* v = o.find(k);
        static const Json zero;
        return Parser::pyfloat(v ? *v : zero);
    };
    auto numa = [&](const Json* a, std::size_t i) -> std::string { return Parser::pyfloat(a->arr[i]); };

    auto [point, arrow] = compass(cur.get_num("wind_direction_10m"));
    const Json* uv = daily.find("uv_index_max");
    const Json* rp = daily.find("precipitation_probability_max");
    const Json* tmin = daily.find("temperature_2m_min");
    const Json* tmax = daily.find("temperature_2m_max");
    const Json* times = daily.find("time");
    const Json* dwc = daily.find("weather_code");
    const Json* sr = daily.find("sunrise");
    const Json* ss = daily.find("sunset");
    double uv0 = (uv && uv->type == Json::Type::Arr && !uv->arr.empty()) ? uv->arr[0].number : 0;

    row("Condition", std::format("{} {}", glyph(dc.art), dc.text));
    row("Temperature", std::format("{}{}  {}(feels like {}{}){}", num(cur, "temperature_2m"), degu,
                                   Palette::muted, num(cur, "apparent_temperature"), degu, Palette::reset));
    row("Today", std::format("{}{} → {}{}", numa(tmin, 0), degu, numa(tmax, 0), degu));
    row("Wind", std::format("{} {} {} {}  {}(gusts {} {}){}", arrow, num(cur, "wind_speed_10m"), spdu,
                             point, Palette::muted, num(cur, "wind_gusts_10m"), spdu, Palette::reset));
    row("Humidity", std::format("{}% {}{}{}", num(cur, "relative_humidity_2m"), Palette::accent,
                                meter(cur.get_num("relative_humidity_2m") / 100), Palette::reset));
    row("Cloud cover", std::format("{}% {}{}{}", num(cur, "cloud_cover"), Palette::accent,
                                   meter(cur.get_num("cloud_cover") / 100), Palette::reset));
    row("Precipitation", std::format("{}{}  {}({}% chance today){}", num(cur, "precipitation"),
                                     cun.get_str("precipitation"), Palette::muted, numa(rp, 0),
                                     Palette::reset));
    row("Pressure", std::format("{} {}", static_cast<long long>(std::nearbyint(cur.get_num("pressure_msl"))),
                                cun.get_str("pressure_msl")));
    row("UV index",
        std::format("{} {}({}){}", std::format("{:.1f}", uv0), Palette::muted, uv_word(uv0), Palette::reset));
    row("Sun", std::format("↑ {}   ↓ {}", clock_time(sr->arr[0].str), clock_time(ss->arr[0].str)));

    rows.emplace_back();
    std::string cells;
    std::size_t ndays = times ? times->arr.size() : 0;
    for (std::size_t k = 1, n = std::min<std::size_t>(4, ndays); k < n; ++k) {
        Desc dd = describe(static_cast<int>(dwc->arr[k].number), true);
        if (!cells.empty()) cells += std::format("{}{} ", Palette::muted, "│", Palette::reset);
        cells += std::format("{}{}{:>4}{} {} {}{:>3}/{:<3}{}", Palette::bold, Palette::label,
                             day_label(times->arr[k].str, static_cast<int>(k)), Palette::reset, glyph(dd.art),
                             Palette::value, static_cast<long long>(std::nearbyint(tmin->arr[k].number)),
                             static_cast<long long>(std::nearbyint(tmax->arr[k].number)), Palette::reset);
    }
    if (!cells.empty())
        rows.push_back(std::format("{}{}  {}", Palette::muted, "Forecast", Palette::reset) + cells);
    rows.emplace_back();
    std::string l1, l2;
    for (int s = 0; s < 8; ++s) std::format_to(std::back_inserter(l1), "\x1b[4{}m   ", s);
    l1 += Palette::reset;
    for (int s = 0; s < 8; ++s) std::format_to(std::back_inserter(l2), "\x1b[10{}m   ", s);
    l2 += Palette::reset;
    rows.push_back(std::move(l1));
    rows.push_back(std::move(l2));
    return {dc.art, dc.r, dc.g, dc.b, std::move(rows)};
}

[[nodiscard]] std::string render(Art art_key, std::uint8_t r, std::uint8_t g, std::uint8_t b,
                                 std::span<const std::string> rows, int gap = 3) {
    std::span<const std::string_view> art = art_lines(art_key);
    std::size_t aw = 0;
    for (std::string_view l : art) aw = std::max(aw, l.size());
    std::size_t pad = rows.size() > art.size() ? (rows.size() - art.size()) / 2 : 0;

    std::string ac = Palette::foreground(r, g, b);
    std::string out;
    std::size_t total = std::max(pad + art.size(), rows.size());
    for (std::size_t i = 0; i < total; ++i) {
        std::string_view l = (i >= pad && i - pad < art.size()) ? art[i - pad] : ""sv;
        std::string_view rr = i < rows.size() ? std::string_view{rows[i]} : ""sv;
        out += ac;
        out += l;
        out += Palette::reset;
        out.append(aw - l.size() + static_cast<std::size_t>(gap), ' ');
        out += rr;
        while (!out.empty() && out.back() != '\n' &&
               (out.back() == ' ' || out.back() == '\t' || out.back() == '\r'))
            out.pop_back(); // rstrip, but never cross a line boundary
        out += '\n';
    }
    if (!out.empty()) out.pop_back();
    return out;
}

// ---------------------------------------------------------------- cli

int run(std::string_view units, std::string_view query, bool by_ip) {
    Client client;
    if (!client.init()) {
        std::print(stderr, "weatherfetch: network error: curl init failed\n");
        return 1;
    }
    Result<Place> pl = by_ip ? locate_by_ip(client) : geocode(client, query);
    if (!pl) {
        std::print(stderr, "weatherfetch: {}\n", pl.error());
        return 1;
    }
    Result<Json> w = fetch_weather(client, pl->lat, pl->lon, units);
    if (!w) {
        std::print(stderr, "weatherfetch: {}\n", w.error());
        return 1;
    }
    Rows rr = build_rows(*w, pl->name, pl->region);
    std::print("\n{}\n\n", render(rr.art, rr.r, rr.g, rr.b, rr.rows));
    return 0;
}

} // namespace wf

using namespace std::literals;

int main(int argc, char** argv) {
    curl_global_init(CURL_GLOBAL_DEFAULT);
    std::string units_arg;
    bool no_color = false;
    std::string query;
    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "-n" || a == "--no-color") {
            no_color = true;
        } else if (a == "-u" || a == "--units" || a.starts_with("--units=")) {
            if (a.starts_with("--units=")) {
                units_arg = a.substr(8);
            } else {
                if (i + 1 >= argc) {
                    std::print(stderr, "weatherfetch: --units needs c or f\n");
                    return 2;
                }
                units_arg = argv[++i];
            }
        } else if (a == "-h" || a == "--help") {
            std::print("usage: {} [-u {{c,f}}] [-n] [CITY[, COUNTRY]]\n", argv[0]);
            return 0;
        } else if (!a.empty() && a[0] == '-') {
            std::print(stderr, "weatherfetch: unknown option {}\n", a);
            return 2;
        } else {
            if (!query.empty()) query += ' ';
            query += a;
        }
    }
    const char* env_units = units_arg.empty() ? std::getenv("WEATHERFETCH_UNITS") : nullptr;
    std::string units = units_arg.empty() ? (env_units ? env_units : "c") : units_arg;
    for (char& c : units)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    if (units != "c" && units != "f") units = "c";

    const char* nc = std::getenv("NO_COLOR");
    if (no_color || (nc && *nc) || !isatty(STDOUT_FILENO)) wf::disable_color();

    int rc = wf::run(units, query, query.empty());
    curl_global_cleanup();
    return rc;
}
