// Minimal, dependency-free JSON value/parser/writer for the Structure Data.
#pragma once
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace dali::json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool b) : type_(Type::Bool), b_(b) {}
    Value(int n) : type_(Type::Number), n_(n) {}
    Value(double n) : type_(Type::Number), n_(n) {}
    Value(const char* s) : type_(Type::String), s_(s) {}
    Value(std::string s) : type_(Type::String), s_(std::move(s)) {}

    static Value array() { Value v; v.type_ = Type::Array; return v; }
    static Value object() { Value v; v.type_ = Type::Object; return v; }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isObject() const { return type_ == Type::Object; }
    bool isArray() const { return type_ == Type::Array; }

    bool asBool(bool def = false) const { return type_ == Type::Bool ? b_ : def; }
    double asNumber(double def = 0) const { return type_ == Type::Number ? n_ : def; }
    int asInt(int def = 0) const { return type_ == Type::Number ? (int) n_ : def; }
    const std::string& asString() const { static const std::string e; return type_ == Type::String ? s_ : e; }

    // arrays
    void push(Value v) { type_ = Type::Array; arr_.push_back(std::move(v)); }
    size_t size() const { return type_ == Type::Array ? arr_.size() : obj_.size(); }
    const Value& operator[](size_t i) const { static const Value n; return i < arr_.size() ? arr_[i] : n; }
    const std::vector<Value>& items() const { return arr_; }

    // objects (insertion order preserved for readable output)
    Value& operator[](const std::string& key);
    const Value& get(const std::string& key) const;
    bool has(const std::string& key) const;
    const std::vector<std::pair<std::string, Value>>& members() const { return obj_; }

    std::string dump(bool pretty = false) const;
    static bool parse(const std::string& text, Value& out, std::string* error = nullptr);

private:
    void dumpTo(std::string& out, bool pretty, int indent) const;
    Type type_ = Type::Null;
    bool b_ = false;
    double n_ = 0;
    std::string s_;
    std::vector<Value> arr_;
    std::vector<std::pair<std::string, Value>> obj_;
};

} // namespace dali::json
