#include "dali/Json.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace dali::json {

Value& Value::operator[](const std::string& key) {
    if (type_ != Type::Object) { type_ = Type::Object; obj_.clear(); }
    for (auto& kv : obj_) if (kv.first == key) return kv.second;
    obj_.emplace_back(key, Value());
    return obj_.back().second;
}

const Value& Value::get(const std::string& key) const {
    static const Value null;
    if (type_ != Type::Object) return null;
    for (auto& kv : obj_) if (kv.first == key) return kv.second;
    return null;
}

bool Value::has(const std::string& key) const {
    if (type_ != Type::Object) return false;
    for (auto& kv : obj_) if (kv.first == key) return true;
    return false;
}

static void escapeString(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) { char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
                else out += (char) c;
        }
    }
    out += '"';
}

void Value::dumpTo(std::string& out, bool pretty, int indent) const {
    auto nl = [&](int ind) { if (pretty) { out += '\n'; out.append((size_t) ind * 2, ' '); } };
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += b_ ? "true" : "false"; break;
        case Type::Number: {
            if (!std::isfinite(n_)) { out += "0"; break; }
            char buf[32];
            if (n_ == std::floor(n_) && std::fabs(n_) < 1e15) std::snprintf(buf, sizeof buf, "%.0f", n_);
            else std::snprintf(buf, sizeof buf, "%.9g", n_);
            out += buf; break;
        }
        case Type::String: escapeString(out, s_); break;
        case Type::Array:
            out += '[';
            for (size_t i = 0; i < arr_.size(); ++i) {
                if (i) out += ',';
                nl(indent + 1); arr_[i].dumpTo(out, pretty, indent + 1);
            }
            if (!arr_.empty()) nl(indent);
            out += ']'; break;
        case Type::Object:
            out += '{';
            for (size_t i = 0; i < obj_.size(); ++i) {
                if (i) out += ',';
                nl(indent + 1); escapeString(out, obj_[i].first); out += pretty ? ": " : ":";
                obj_[i].second.dumpTo(out, pretty, indent + 1);
            }
            if (!obj_.empty()) nl(indent);
            out += '}'; break;
    }
}

std::string Value::dump(bool pretty) const { std::string s; dumpTo(s, pretty, 0); return s; }

// ---------------------------------------------------------------- parser
namespace {
struct Parser {
    const std::string& t; size_t p = 0; std::string err;
    explicit Parser(const std::string& s) : t(s) {}
    void ws() { while (p < t.size() && (t[p] == ' ' || t[p] == '\n' || t[p] == '\r' || t[p] == '\t')) ++p; }
    bool fail(const char* m) { if (err.empty()) err = std::string(m) + " at " + std::to_string(p); return false; }

    static void appendUtf8(std::string& o, unsigned cp) {
        if (cp < 0x80) o += (char) cp;
        else if (cp < 0x800) { o += (char) (0xC0 | (cp >> 6)); o += (char) (0x80 | (cp & 0x3F)); }
        else if (cp < 0x10000) { o += (char) (0xE0 | (cp >> 12)); o += (char) (0x80 | ((cp >> 6) & 0x3F)); o += (char) (0x80 | (cp & 0x3F)); }
        else { o += (char) (0xF0 | (cp >> 18)); o += (char) (0x80 | ((cp >> 12) & 0x3F)); o += (char) (0x80 | ((cp >> 6) & 0x3F)); o += (char) (0x80 | (cp & 0x3F)); }
    }
    bool hex4(unsigned& v) {
        if (p + 4 > t.size()) return fail("bad \\u escape");
        v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = t[p++]; v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else return fail("bad hex");
        }
        return true;
    }
    bool str(std::string& o) {
        if (t[p] != '"') return fail("expected string");
        ++p;
        while (p < t.size()) {
            char c = t[p++];
            if (c == '"') return true;
            if (c == '\\') {
                if (p >= t.size()) return fail("bad escape");
                char e = t[p++];
                switch (e) {
                    case '"': o += '"'; break; case '\\': o += '\\'; break; case '/': o += '/'; break;
                    case 'b': o += '\b'; break; case 'f': o += '\f'; break; case 'n': o += '\n'; break;
                    case 'r': o += '\r'; break; case 't': o += '\t'; break;
                    case 'u': {
                        unsigned cp = 0; if (!hex4(cp)) return false;
                        if (cp >= 0xD800 && cp < 0xDC00 && p + 6 <= t.size() && t[p] == '\\' && t[p + 1] == 'u') {
                            p += 2; unsigned lo = 0; if (!hex4(lo)) return false;
                            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                        }
                        appendUtf8(o, cp); break;
                    }
                    default: return fail("bad escape");
                }
            } else o += c;
        }
        return fail("unterminated string");
    }
    bool val(Value& v, int depth) {
        if (depth > 64) return fail("too deep");
        ws();
        if (p >= t.size()) return fail("unexpected end");
        char c = t[p];
        if (c == '{') {
            ++p; v = Value::object(); ws();
            if (p < t.size() && t[p] == '}') { ++p; return true; }
            while (true) {
                ws(); std::string k; if (!str(k)) return false;
                ws(); if (p >= t.size() || t[p] != ':') return fail("expected ':'"); ++p;
                Value child; if (!val(child, depth + 1)) return false;
                v[k] = std::move(child);
                ws(); if (p >= t.size()) return fail("unexpected end");
                if (t[p] == ',') { ++p; continue; }
                if (t[p] == '}') { ++p; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++p; v = Value::array(); ws();
            if (p < t.size() && t[p] == ']') { ++p; return true; }
            while (true) {
                Value child; if (!val(child, depth + 1)) return false;
                v.push(std::move(child));
                ws(); if (p >= t.size()) return fail("unexpected end");
                if (t[p] == ',') { ++p; continue; }
                if (t[p] == ']') { ++p; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') { std::string s; if (!str(s)) return false; v = Value(std::move(s)); return true; }
        if (t.compare(p, 4, "true") == 0) { p += 4; v = Value(true); return true; }
        if (t.compare(p, 5, "false") == 0) { p += 5; v = Value(false); return true; }
        if (t.compare(p, 4, "null") == 0) { p += 4; v = Value(); return true; }
        const char* start = t.c_str() + p; char* end = nullptr;
        double d = std::strtod(start, &end);
        if (end == start) return fail("unexpected token");
        p += (size_t) (end - start); v = Value(d); return true;
    }
};
} // namespace

bool Value::parse(const std::string& text, Value& out, std::string* error) {
    Parser ps(text);
    Value v;
    bool ok = ps.val(v, 0);
    if (ok) { ps.ws(); if (ps.p != text.size()) ok = ps.fail("trailing characters"); }
    if (!ok) { if (error) *error = ps.err; return false; }
    out = std::move(v);
    return true;
}

} // namespace dali::json
