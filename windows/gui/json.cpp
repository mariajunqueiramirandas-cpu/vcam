#include "json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace vcam::gui {

namespace {

const Json kNull;
const std::string kEmpty;

void AppendUtf8(std::string* out, uint32_t cp)
{
    if (cp < 0x80) {
        out->push_back(static_cast<char>(cp));
    } else if (cp < 0x800) {
        out->push_back(static_cast<char>(0xC0 | (cp >> 6)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp < 0x10000) {
        out->push_back(static_cast<char>(0xE0 | (cp >> 12)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        out->push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
}

void AppendWide(std::wstring* out, uint32_t cp)
{
    if constexpr (sizeof(wchar_t) == 2) {
        if (cp >= 0x10000) {
            cp -= 0x10000;
            out->push_back(static_cast<wchar_t>(0xD800 + (cp >> 10)));
            out->push_back(static_cast<wchar_t>(0xDC00 + (cp & 0x3FF)));
            return;
        }
    }
    out->push_back(static_cast<wchar_t>(cp));
}

class Parser {
public:
    explicit Parser(std::string_view text) : s_(text) {}

    bool Parse(Json* out)
    {
        if (!Value(out, 0)) {
            return false;
        }
        Space();
        return i_ == s_.size();
    }

private:
    void Space()
    {
        while (i_ < s_.size() && (s_[i_] == ' ' || s_[i_] == '\t' || s_[i_] == '\n' || s_[i_] == '\r')) {
            i_++;
        }
    }

    bool Literal(const char* word)
    {
        const std::string_view w(word);
        if (s_.substr(i_, w.size()) != w) {
            return false;
        }
        i_ += w.size();
        return true;
    }

    bool Hex4(uint32_t* value)
    {
        if (i_ + 4 > s_.size()) {
            return false;
        }
        uint32_t v = 0;
        for (int k = 0; k < 4; k++) {
            const char c = s_[i_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else return false;
        }
        *value = v;
        return true;
    }

    bool String(std::string* out)
    {
        if (i_ >= s_.size() || s_[i_] != '"') {
            return false;
        }
        i_++;
        while (i_ < s_.size()) {
            const char c = s_[i_++];
            if (c == '"') {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return false;
            }
            if (c != '\\') {
                out->push_back(c);
                continue;
            }
            if (i_ >= s_.size()) {
                return false;
            }
            const char e = s_[i_++];
            switch (e) {
            case '"': out->push_back('"'); break;
            case '\\': out->push_back('\\'); break;
            case '/': out->push_back('/'); break;
            case 'b': out->push_back('\b'); break;
            case 'f': out->push_back('\f'); break;
            case 'n': out->push_back('\n'); break;
            case 'r': out->push_back('\r'); break;
            case 't': out->push_back('\t'); break;
            case 'u': {
                uint32_t cp = 0;
                if (!Hex4(&cp)) {
                    return false;
                }
                if (cp >= 0xD800 && cp < 0xDC00) {
                    uint32_t low = 0;
                    if (!Literal("\\u") || !Hex4(&low) || low < 0xDC00 || low >= 0xE000) {
                        return false;
                    }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                } else if (cp >= 0xDC00 && cp < 0xE000) {
                    return false;
                }
                AppendUtf8(out, cp);
                break;
            }
            default:
                return false;
            }
        }
        return false;
    }

    bool Value(Json* out, int depth)
    {
        if (depth > 64) {
            return false;
        }
        Space();
        if (i_ >= s_.size()) {
            return false;
        }
        const char c = s_[i_];
        if (c == '{') {
            i_++;
            *out = Json::Object();
            Space();
            if (i_ < s_.size() && s_[i_] == '}') {
                i_++;
                return true;
            }
            for (;;) {
                Space();
                std::string key;
                if (!String(&key)) {
                    return false;
                }
                Space();
                if (i_ >= s_.size() || s_[i_++] != ':') {
                    return false;
                }
                Json value;
                if (!Value(&value, depth + 1)) {
                    return false;
                }
                (*out)[key] = std::move(value);
                Space();
                if (i_ >= s_.size()) {
                    return false;
                }
                if (s_[i_] == ',') {
                    i_++;
                    continue;
                }
                if (s_[i_] == '}') {
                    i_++;
                    return true;
                }
                return false;
            }
        }
        if (c == '[') {
            i_++;
            *out = Json::Array();
            Space();
            if (i_ < s_.size() && s_[i_] == ']') {
                i_++;
                return true;
            }
            for (;;) {
                Json value;
                if (!Value(&value, depth + 1)) {
                    return false;
                }
                out->push_back(std::move(value));
                Space();
                if (i_ >= s_.size()) {
                    return false;
                }
                if (s_[i_] == ',') {
                    i_++;
                    continue;
                }
                if (s_[i_] == ']') {
                    i_++;
                    return true;
                }
                return false;
            }
        }
        if (c == '"') {
            std::string text;
            if (!String(&text)) {
                return false;
            }
            *out = Json(std::move(text));
            return true;
        }
        if (Literal("true")) {
            *out = Json(true);
            return true;
        }
        if (Literal("false")) {
            *out = Json(false);
            return true;
        }
        if (Literal("null")) {
            *out = Json();
            return true;
        }
        // Number.
        const size_t start = i_;
        if (i_ < s_.size() && s_[i_] == '-') i_++;
        while (i_ < s_.size() && ((s_[i_] >= '0' && s_[i_] <= '9') || s_[i_] == '.' || s_[i_] == 'e' ||
                                  s_[i_] == 'E' || s_[i_] == '+' || s_[i_] == '-')) {
            i_++;
        }
        if (i_ == start) {
            return false;
        }
        const std::string number(s_.substr(start, i_ - start));
        char* end = nullptr;
        const double v = strtod(number.c_str(), &end);
        if (!end || *end != '\0' || !std::isfinite(v)) {
            return false;
        }
        *out = Json(v);
        return true;
    }

    std::string_view s_;
    size_t i_ = 0;
};

}  // namespace

Json::Json(const std::wstring& value) : type_(Type::String)
{
    for (size_t i = 0; i < value.size(); i++) {
        uint32_t cp = static_cast<uint32_t>(value[i]);
        if constexpr (sizeof(wchar_t) == 2) {
            if (cp >= 0xD800 && cp < 0xDC00 && i + 1 < value.size()) {
                const uint32_t low = static_cast<uint32_t>(value[i + 1]);
                if (low >= 0xDC00 && low < 0xE000) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    i++;
                }
            }
        }
        if ((cp >= 0xD800 && cp < 0xE000) || cp > 0x10FFFF) {
            cp = 0xFFFD;
        }
        AppendUtf8(&string_, cp);
    }
}

Json Json::Parse(std::string_view text, bool* ok)
{
    Json out;
    Parser parser(text);
    const bool parsed = parser.Parse(&out);
    if (ok) {
        *ok = parsed;
    }
    return parsed ? out : Json();
}

int Json::integer(int fallback) const
{
    if (type_ != Type::Number || !std::isfinite(number_) || number_ < -2147483648.0 || number_ > 2147483647.0) {
        return fallback;
    }
    return static_cast<int>(number_);
}

const std::string& Json::string() const
{
    return type_ == Type::String ? string_ : kEmpty;
}

std::wstring Json::wstring() const
{
    std::wstring out;
    const std::string& s = string();
    for (size_t i = 0; i < s.size();) {
        const auto c = static_cast<unsigned char>(s[i]);
        uint32_t cp = 0xFFFD;
        size_t n = 1;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0 && i + 1 < s.size()) {
            cp = ((c & 0x1Fu) << 6) | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
            n = 2;
        } else if ((c & 0xF0) == 0xE0 && i + 2 < s.size()) {
            cp = ((c & 0x0Fu) << 12) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 6) |
                 (static_cast<unsigned char>(s[i + 2]) & 0x3Fu);
            n = 3;
        } else if ((c & 0xF8) == 0xF0 && i + 3 < s.size()) {
            cp = ((c & 0x07u) << 18) | ((static_cast<unsigned char>(s[i + 1]) & 0x3Fu) << 12) |
                 ((static_cast<unsigned char>(s[i + 2]) & 0x3Fu) << 6) | (static_cast<unsigned char>(s[i + 3]) & 0x3Fu);
            n = 4;
        }
        AppendWide(&out, cp);
        i += n;
    }
    return out;
}

const Json& Json::operator[](std::string_view key) const
{
    if (type_ == Type::Object) {
        for (const auto& [k, v] : object_) {
            if (k == key) {
                return v;
            }
        }
    }
    return kNull;
}

Json& Json::operator[](std::string_view key)
{
    if (type_ != Type::Object) {
        *this = Object();
    }
    for (auto& [k, v] : object_) {
        if (k == key) {
            return v;
        }
    }
    object_.emplace_back(std::string(key), Json());
    return object_.back().second;
}

bool Json::has(std::string_view key) const
{
    if (type_ == Type::Object) {
        for (const auto& entry : object_) {
            if (entry.first == key) {
                return true;
            }
        }
    }
    return false;
}

const Json& Json::operator[](size_t index) const
{
    return type_ == Type::Array && index < array_.size() ? array_[index] : kNull;
}

void Json::push_back(Json value)
{
    if (type_ != Type::Array) {
        *this = Array();
    }
    array_.push_back(std::move(value));
}

std::string Json::Dump() const
{
    std::string out;
    DumpTo(&out);
    return out;
}

void Json::DumpTo(std::string* out) const
{
    switch (type_) {
    case Type::Null:
        out->append("null");
        break;
    case Type::Bool:
        out->append(bool_ ? "true" : "false");
        break;
    case Type::Number: {
        if (!std::isfinite(number_)) {
            out->append("null");
            break;
        }
        char buffer[32];
        if (number_ == std::floor(number_) && std::fabs(number_) < 1e15) {
            snprintf(buffer, sizeof(buffer), "%.0f", number_);
        } else {
            snprintf(buffer, sizeof(buffer), "%.6g", number_);
        }
        out->append(buffer);
        break;
    }
    case Type::String:
        out->push_back('"');
        for (const char c : string_) {
            switch (c) {
            case '"': out->append("\\\""); break;
            case '\\': out->append("\\\\"); break;
            case '\n': out->append("\\n"); break;
            case '\r': out->append("\\r"); break;
            case '\t': out->append("\\t"); break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    char buffer[8];
                    snprintf(buffer, sizeof(buffer), "\\u%04x", static_cast<unsigned>(c));
                    out->append(buffer);
                } else {
                    out->push_back(c);
                }
            }
        }
        out->push_back('"');
        break;
    case Type::Array:
        out->push_back('[');
        for (size_t i = 0; i < array_.size(); i++) {
            if (i) out->push_back(',');
            array_[i].DumpTo(out);
        }
        out->push_back(']');
        break;
    case Type::Object:
        out->push_back('{');
        for (size_t i = 0; i < object_.size(); i++) {
            if (i) out->push_back(',');
            Json(object_[i].first).DumpTo(out);
            out->push_back(':');
            object_[i].second.DumpTo(out);
        }
        out->push_back('}');
        break;
    }
}

std::string Base64(const uint8_t* data, size_t size)
{
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((size + 2) / 3 * 4);
    for (size_t i = 0; i < size; i += 3) {
        const uint32_t b = (static_cast<uint32_t>(data[i]) << 16) | (i + 1 < size ? data[i + 1] << 8 : 0) |
                           (i + 2 < size ? data[i + 2] : 0);
        out.push_back(table[(b >> 18) & 63]);
        out.push_back(table[(b >> 12) & 63]);
        out.push_back(i + 1 < size ? table[(b >> 6) & 63] : '=');
        out.push_back(i + 2 < size ? table[b & 63] : '=');
    }
    return out;
}

}  // namespace vcam::gui
