// A small JSON value for the messages between the GUI host and its page.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vcam::gui {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool value) : type_(Type::Bool), bool_(value) {}
    Json(int value) : type_(Type::Number), number_(value) {}
    Json(int64_t value) : type_(Type::Number), number_(static_cast<double>(value)) {}
    Json(uint64_t value) : type_(Type::Number), number_(static_cast<double>(value)) {}
    Json(uint32_t value) : type_(Type::Number), number_(value) {}
    Json(double value) : type_(Type::Number), number_(value) {}
    Json(const char* value) : type_(Type::String), string_(value) {}
    Json(std::string value) : type_(Type::String), string_(std::move(value)) {}
    Json(const std::wstring& value);  // converted to UTF-8

    static Json Array() { Json j; j.type_ = Type::Array; return j; }
    static Json Object() { Json j; j.type_ = Type::Object; return j; }
    // Returns a Null value and sets *ok = false on malformed input.
    static Json Parse(std::string_view text, bool* ok = nullptr);

    Type type() const { return type_; }
    bool is_null() const { return type_ == Type::Null; }
    bool is_object() const { return type_ == Type::Object; }
    bool is_array() const { return type_ == Type::Array; }

    bool boolean(bool fallback = false) const { return type_ == Type::Bool ? bool_ : fallback; }
    double number(double fallback = 0) const { return type_ == Type::Number ? number_ : fallback; }
    int integer(int fallback = 0) const;
    const std::string& string() const;  // "" unless a string
    std::wstring wstring() const;

    // Objects: missing keys read as Null; writing inserts.
    const Json& operator[](std::string_view key) const;
    Json& operator[](std::string_view key);
    bool has(std::string_view key) const;
    // Arrays.
    size_t size() const { return type_ == Type::Array ? array_.size() : 0; }
    const Json& operator[](size_t index) const;
    void push_back(Json value);
    const std::vector<Json>& items() const { return array_; }

    std::string Dump() const;

private:
    void DumpTo(std::string* out) const;

    Type type_ = Type::Null;
    bool bool_ = false;
    double number_ = 0;
    std::string string_;
    std::vector<Json> array_;
    std::vector<std::pair<std::string, Json>> object_;
};

std::string Base64(const uint8_t* data, size_t size);

}  // namespace vcam::gui
