#pragma once
// A small JSON reader and string escaper for the lobby server's messages (core/relay_link.cpp). Bounded depth, no
// exceptions: a malformed document parses as null.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace json {

struct Value {
    enum Kind { Null, Bool, Number, String, Array, Object } kind = Null;
    bool b = false;
    double n = 0;
    std::string s;
    std::vector<Value> a;
    std::map<std::string, Value> o;

    const Value& operator[](const std::string& key) const {
        static const Value none;
        if (kind != Object) return none;
        auto it = o.find(key);
        return it == o.end() ? none : it->second;
    }
    std::string str(const std::string& fallback = {}) const { return kind == String ? s : fallback; }
    double num(double fallback = 0) const { return kind == Number ? n : fallback; }
    bool boolean(bool fallback = false) const { return kind == Bool ? b : fallback; }
};

namespace detail {
struct Parser {
    std::string_view t;
    size_t i = 0;
    bool ok = true;

    void ws() {
        while (i < t.size() && (t[i] == ' ' || t[i] == '\n' || t[i] == '\r' || t[i] == '\t')) i++;
    }
    bool lit(std::string_view w) {
        if (t.substr(i, w.size()) != w) return ok = false;
        i += w.size();
        return true;
    }
    static void utf8(std::string& out, uint32_t c) {
        if (c < 0x80) out += char(c);
        else if (c < 0x800) out += char(0xC0 | c >> 6), out += char(0x80 | (c & 63));
        else if (c < 0x10000) out += char(0xE0 | c >> 12), out += char(0x80 | (c >> 6 & 63)), out += char(0x80 | (c & 63));
        else
            out += char(0xF0 | c >> 18), out += char(0x80 | (c >> 12 & 63)), out += char(0x80 | (c >> 6 & 63)),
                out += char(0x80 | (c & 63));
    }
    uint32_t hex4() {
        if (i + 4 > t.size()) return ok = false, 0;
        uint32_t v = 0;
        for (int k = 0; k < 4; k++) {
            char c = t[i++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= uint32_t(c - '0');
            else if (c >= 'a' && c <= 'f') v |= uint32_t(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= uint32_t(c - 'A' + 10);
            else return ok = false, 0;
        }
        return v;
    }
    std::string string() {
        std::string out;
        if (i >= t.size() || t[i] != '"') return ok = false, out;
        i++;
        while (ok && i < t.size() && t[i] != '"') {
            char c = t[i++];
            if (c != '\\') {
                out += c;
                continue;
            }
            if (i >= t.size()) return ok = false, out;
            char e = t[i++];
            switch (e) {
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    uint32_t c1 = hex4();
                    if (c1 >= 0xD800 && c1 < 0xDC00 && t.substr(i, 2) == "\\u") {
                        i += 2;
                        uint32_t c2 = hex4();
                        c1 = 0x10000 + ((c1 - 0xD800) << 10) + (c2 - 0xDC00);
                    }
                    utf8(out, c1);
                    break;
                }
                default: out += e;
            }
        }
        if (i >= t.size()) return ok = false, out;
        i++;
        return out;
    }
    Value value(int depth) {
        Value v;
        ws();
        if (!ok || i >= t.size() || depth > 32) return ok = false, v;
        char c = t[i];
        if (c == '{') {
            v.kind = Value::Object;
            i++;
            ws();
            if (i < t.size() && t[i] == '}') return i++, v;
            while (ok) {
                ws();
                std::string k = string();
                ws();
                if (!ok || i >= t.size() || t[i] != ':') return ok = false, v;
                i++;
                v.o[k] = value(depth + 1);
                ws();
                if (i < t.size() && t[i] == ',') { i++; continue; }
                if (i < t.size() && t[i] == '}') { i++; break; }
                ok = false;
            }
        } else if (c == '[') {
            v.kind = Value::Array;
            i++;
            ws();
            if (i < t.size() && t[i] == ']') return i++, v;
            while (ok) {
                v.a.push_back(value(depth + 1));
                ws();
                if (i < t.size() && t[i] == ',') { i++; continue; }
                if (i < t.size() && t[i] == ']') { i++; break; }
                ok = false;
            }
        } else if (c == '"') {
            v.kind = Value::String;
            v.s = string();
        } else if (c == 't') {
            if (lit("true")) v.kind = Value::Bool, v.b = true;
        } else if (c == 'f') {
            if (lit("false")) v.kind = Value::Bool;
        } else if (c == 'n') {
            lit("null");
        } else {
            const char* start = t.data() + i;
            char* end = nullptr;
            std::string num(t.substr(i, std::min<size_t>(64, t.size() - i)));
            v.n = std::strtod(num.c_str(), &end);
            size_t used = size_t(end - num.c_str());
            if (!used) return ok = false, v;
            (void)start;
            i += used;
            v.kind = Value::Number;
        }
        return v;
    }
};
}  // namespace detail

inline Value Parse(std::string_view text) {
    detail::Parser p{text};
    Value v = p.value(0);
    p.ws();
    if (!p.ok || p.i != text.size()) return {};
    return v;
}

// a JSON string literal, quotes included
inline std::string Quote(std::string_view s) {
    std::string out = "\"";
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char b[8];
                    std::snprintf(b, sizeof b, "\\u%04x", c);
                    out += b;
                } else out += char(c);
        }
    }
    return out + "\"";
}

}  // namespace json
