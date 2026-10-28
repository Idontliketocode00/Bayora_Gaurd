// Tiny JSON helpers: enough for our flat request/response objects and the
// Ollama reply. NOT a general JSON library.
#pragma once
#include <cstdio>
#include <map>
#include <string>

namespace bg {

// A value from a flat JSON object. Strings are stored unescaped (is_str=true);
// numbers, true/false/null, arrays and objects are stored as their raw text.
struct JVal {
    bool is_str = false;
    std::string v;
};
using JObj = std::map<std::string, JVal>;

inline std::string json_escape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"':  o += "\\\""; break;
            case '\\': o += "\\\\"; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) { char b[8]; std::snprintf(b, sizeof b, "\\u%04x", c); o += b; }
                else o += static_cast<char>(c);  // UTF-8 bytes pass through untouched
        }
    }
    return o;
}

inline void append_utf8(std::string& o, unsigned cp) {
    if (cp < 0x80) o += static_cast<char>(cp);
    else if (cp < 0x800) { o += static_cast<char>(0xC0 | (cp >> 6)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) { o += static_cast<char>(0xE0 | (cp >> 12)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
    else { o += static_cast<char>(0xF0 | (cp >> 18)); o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)); o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)); o += static_cast<char>(0x80 | (cp & 0x3F)); }
}

inline void skip_ws(const std::string& t, size_t& i) {
    while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) ++i;
}

inline bool hex4(const std::string& t, size_t i, unsigned& out) {
    if (i + 4 > t.size()) return false;
    out = 0;
    for (size_t k = 0; k < 4; ++k) {
        char c = t[i + k];
        out <<= 4;
        if (c >= '0' && c <= '9') out |= c - '0';
        else if (c >= 'a' && c <= 'f') out |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') out |= c - 'A' + 10;
        else return false;
    }
    return true;
}

// Parses a JSON string starting at t[i] == '"'. Leaves i just after the closing quote.
inline bool parse_string(const std::string& t, size_t& i, std::string& out) {
    if (i >= t.size() || t[i] != '"') return false;
    ++i;
    out.clear();
    while (i < t.size()) {
        char c = t[i++];
        if (c == '"') return true;
        if (c != '\\') { out += c; continue; }
        if (i >= t.size()) return false;
        char e = t[i++];
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
                unsigned cp;
                if (!hex4(t, i, cp)) return false;
                i += 4;
                if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= t.size() && t[i] == '\\' && t[i + 1] == 'u') {
                    unsigned lo;
                    if (hex4(t, i + 2, lo) && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        i += 6;
                    }
                }
                append_utf8(out, cp);
                break;
            }
            default: return false;
        }
    }
    return false;  // unterminated
}

// Parses one flat JSON object: {"k":"string","n":5,"ok":true,"arr":[...]}.
// Nested arrays/objects are kept as raw text (we only skip over them).
inline bool json_parse_flat(const std::string& t, JObj& out) {
    out.clear();
    size_t i = 0;
    skip_ws(t, i);
    if (i >= t.size() || t[i] != '{') return false;
    ++i;
    while (true) {
        skip_ws(t, i);
        if (i < t.size() && t[i] == '}') return true;
        std::string key;
        if (!parse_string(t, i, key)) return false;
        skip_ws(t, i);
        if (i >= t.size() || t[i] != ':') return false;
        ++i;
        skip_ws(t, i);
        if (i >= t.size()) return false;
        JVal val;
        if (t[i] == '"') {
            val.is_str = true;
            if (!parse_string(t, i, val.v)) return false;
        } else if (t[i] == '{' || t[i] == '[') {
            size_t start = i;
            int depth = 0;
            bool in_str = false;
            for (; i < t.size(); ++i) {
                char c = t[i];
                if (in_str) {
                    if (c == '\\') ++i;
                    else if (c == '"') in_str = false;
                } else if (c == '"') in_str = true;
                else if (c == '{' || c == '[') ++depth;
                else if (c == '}' || c == ']') { if (--depth == 0) { ++i; break; } }
            }
            if (depth != 0) return false;
            val.v = t.substr(start, i - start);
        } else {
            size_t start = i;
            while (i < t.size() && t[i] != ',' && t[i] != '}' && t[i] != ' ' && t[i] != '\n' && t[i] != '\r' && t[i] != '\t') ++i;
            val.v = t.substr(start, i - start);
        }
        out[key] = val;
        skip_ws(t, i);
        if (i >= t.size()) return false;
        if (t[i] == ',') { ++i; continue; }
        if (t[i] == '}') return true;
        return false;
    }
}

inline std::string jget(const JObj& o, const std::string& key, const std::string& dflt = "") {
    auto it = o.find(key);
    return it == o.end() ? dflt : it->second.v;
}

}  // namespace bg
