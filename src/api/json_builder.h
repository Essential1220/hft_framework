#pragma once
// ============================================
// json_builder.h - Lightweight JSON string builder (轻量 JSON 字符串构建器)
// No external dependency. Builds JSON via std::string append.
// ============================================

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace hft {

class JsonBuilder {
public:
    JsonBuilder& begin_object() { raw_ += '{'; needs_comma_ .push_back(false); return *this; }
    JsonBuilder& end_object()   { needs_comma_.pop_back(); raw_ += '}'; mark_comma(); return *this; }
    JsonBuilder& begin_array()  { raw_ += '['; needs_comma_.push_back(false); return *this; }
    JsonBuilder& end_array()    { needs_comma_.pop_back(); raw_ += ']'; mark_comma(); return *this; }

    JsonBuilder& key(const char* k) {
        comma();
        raw_ += '"';
        escape_append(k);
        raw_ += "\":";
        return *this;
    }

    JsonBuilder& value(const char* v) {
        if (!in_key_) comma();
        raw_ += '"';
        escape_append(v);
        raw_ += '"';
        if (!in_key_) mark_comma();
        return *this;
    }

    JsonBuilder& value(const std::string& v) { return value(v.c_str()); }

    JsonBuilder& value(int v) {
        if (!in_key_) comma();
        raw_ += std::to_string(v);
        if (!in_key_) mark_comma();
        return *this;
    }

    JsonBuilder& value(int64_t v) {
        if (!in_key_) comma();
        raw_ += std::to_string(v);
        if (!in_key_) mark_comma();
        return *this;
    }

    JsonBuilder& value(size_t v) {
        if (!in_key_) comma();
        raw_ += std::to_string(v);
        if (!in_key_) mark_comma();
        return *this;
    }

    JsonBuilder& value(double v) {
        if (!in_key_) comma();
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%.6f", v);
        raw_ += buf;
        if (!in_key_) mark_comma();
        return *this;
    }

    JsonBuilder& value(bool v) {
        if (!in_key_) comma();
        raw_ += v ? "true" : "false";
        if (!in_key_) mark_comma();
        return *this;
    }

    JsonBuilder& null_value() {
        if (!in_key_) comma();
        raw_ += "null";
        if (!in_key_) mark_comma();
        return *this;
    }

    JsonBuilder& kv(const char* k, const char* v)      { key(k); in_key_=true; value(v); in_key_=false; mark_comma(); return *this; }
    JsonBuilder& kv(const char* k, const std::string& v){ return kv(k, v.c_str()); }
    JsonBuilder& kv(const char* k, int v)               { key(k); in_key_=true; value(v); in_key_=false; mark_comma(); return *this; }
    JsonBuilder& kv(const char* k, int64_t v)           { key(k); in_key_=true; value(v); in_key_=false; mark_comma(); return *this; }
    JsonBuilder& kv(const char* k, size_t v)            { key(k); in_key_=true; value(v); in_key_=false; mark_comma(); return *this; }
    JsonBuilder& kv(const char* k, double v)            { key(k); in_key_=true; value(v); in_key_=false; mark_comma(); return *this; }
    JsonBuilder& kv(const char* k, bool v)              { key(k); in_key_=true; value(v); in_key_=false; mark_comma(); return *this; }

    // On GCC/Linux, long long is a distinct type from int64_t (long) and int.
    // Guard with enable_if so the overload disappears on MSVC where int64_t IS long long.
    template<typename T = long long,
             typename std::enable_if<!std::is_same<T, int>::value
                                  && !std::is_same<T, int64_t>::value
                                  && !std::is_same<T, size_t>::value, int>::type = 0>
    JsonBuilder& value(long long v) {
        if (!in_key_) comma();
        raw_ += std::to_string(v);
        if (!in_key_) mark_comma();
        return *this;
    }

    template<typename T = long long,
             typename std::enable_if<!std::is_same<T, int>::value
                                  && !std::is_same<T, int64_t>::value
                                  && !std::is_same<T, size_t>::value, int>::type = 0>
    JsonBuilder& kv(const char* k, long long v) { key(k); in_key_=true; value(v); in_key_=false; mark_comma(); return *this; }

    std::string build() const { return raw_; }
    const std::string& str() const { return raw_; }

    char* to_cstr() const {
        char* s = new char[raw_.size() + 1];
        std::memcpy(s, raw_.c_str(), raw_.size() + 1);
        return s;
    }

private:
    void comma() {
        if (!needs_comma_.empty() && needs_comma_.back()) {
            raw_ += ',';
            needs_comma_.back() = false;
        }
    }
    void mark_comma() {
        if (!needs_comma_.empty()) needs_comma_.back() = true;
    }
    void escape_append(const char* s) {
        for (; *s; ++s) {
            switch (*s) {
                case '"':  raw_ += "\\\""; break;
                case '\\': raw_ += "\\\\"; break;
                case '\n': raw_ += "\\n";  break;
                case '\r': raw_ += "\\r";  break;
                case '\t': raw_ += "\\t";  break;
                default:   raw_ += *s;     break;
            }
        }
    }

    std::string raw_;
    std::vector<bool> needs_comma_;
    bool in_key_ = false;
};

} // namespace hft
