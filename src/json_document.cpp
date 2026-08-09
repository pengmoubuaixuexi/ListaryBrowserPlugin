#include "json_document.h"

#include "text_util.h"

#include <cwctype>
#include <sstream>

JsonValue JsonValue::Boolean(bool value) {
    JsonValue result;
    result.type_ = Type::Boolean;
    result.boolean_ = value;
    return result;
}

JsonValue JsonValue::Number(std::wstring value) {
    JsonValue result;
    result.type_ = Type::Number;
    result.text_ = std::move(value);
    return result;
}

JsonValue JsonValue::String(std::wstring value) {
    JsonValue result;
    result.type_ = Type::String;
    result.text_ = std::move(value);
    return result;
}

JsonValue JsonValue::ArrayValue() {
    JsonValue result;
    result.type_ = Type::Array;
    return result;
}

JsonValue JsonValue::ObjectValue() {
    JsonValue result;
    result.type_ = Type::Object;
    return result;
}

JsonValue* JsonValue::Find(std::wstring_view name) {
    if (type_ != Type::Object) return nullptr;
    for (auto& [key, value] : object_) {
        if (key == name) return &value;
    }
    return nullptr;
}

const JsonValue* JsonValue::Find(std::wstring_view name) const {
    if (type_ != Type::Object) return nullptr;
    for (const auto& [key, value] : object_) {
        if (key == name) return &value;
    }
    return nullptr;
}

JsonValue& JsonValue::Set(std::wstring name, JsonValue value) {
    if (type_ != Type::Object) {
        type_ = Type::Object;
        text_.clear();
        array_.clear();
        object_.clear();
    }
    for (auto& [key, current] : object_) {
        if (key == name) {
            current = std::move(value);
            return current;
        }
    }
    object_.emplace_back(std::move(name), std::move(value));
    return object_.back().second;
}

namespace {
class Parser {
public:
    explicit Parser(std::wstring input) : input_(std::move(input)) {}

    bool Parse(JsonValue& value, std::wstring& error) {
        SkipSpace();
        if (!ParseValue(value)) {
            error = L"JSON 解析失败，位置 " + std::to_wstring(position_) + L"：" + error_;
            return false;
        }
        SkipSpace();
        if (position_ != input_.size()) {
            error = L"JSON 末尾存在无法识别的内容，位置 " + std::to_wstring(position_) + L"。";
            return false;
        }
        return true;
    }

private:
    void SkipSpace() {
        while (position_ < input_.size() && std::iswspace(input_[position_])) ++position_;
    }

    bool Consume(wchar_t expected) {
        SkipSpace();
        if (position_ >= input_.size() || input_[position_] != expected) return false;
        ++position_;
        return true;
    }

    bool ParseValue(JsonValue& value) {
        SkipSpace();
        if (position_ >= input_.size()) return Fail(L"缺少值");
        switch (input_[position_]) {
        case L'n': return ParseLiteral(L"null", JsonValue(), value);
        case L't': return ParseLiteral(L"true", JsonValue::Boolean(true), value);
        case L'f': return ParseLiteral(L"false", JsonValue::Boolean(false), value);
        case L'"': {
            std::wstring text;
            if (!ParseString(text)) return false;
            value = JsonValue::String(std::move(text));
            return true;
        }
        case L'[': return ParseArray(value);
        case L'{': return ParseObject(value);
        default: return ParseNumber(value);
        }
    }

    bool ParseLiteral(std::wstring_view literal, JsonValue literalValue, JsonValue& value) {
        if (input_.substr(position_, literal.size()) != literal) return Fail(L"无效字面量");
        position_ += literal.size();
        value = std::move(literalValue);
        return true;
    }

    bool ParseString(std::wstring& value) {
        if (input_[position_++] != L'"') return Fail(L"缺少字符串起始引号");
        while (position_ < input_.size()) {
            wchar_t ch = input_[position_++];
            if (ch == L'"') return true;
            if (ch < 0x20) return Fail(L"字符串包含控制字符");
            if (ch != L'\\') {
                value.push_back(ch);
                continue;
            }
            if (position_ >= input_.size()) return Fail(L"字符串转义不完整");
            ch = input_[position_++];
            switch (ch) {
            case L'"': value.push_back(L'"'); break;
            case L'\\': value.push_back(L'\\'); break;
            case L'/': value.push_back(L'/'); break;
            case L'b': value.push_back(L'\b'); break;
            case L'f': value.push_back(L'\f'); break;
            case L'n': value.push_back(L'\n'); break;
            case L'r': value.push_back(L'\r'); break;
            case L't': value.push_back(L'\t'); break;
            case L'u': {
                unsigned code = 0;
                if (!ParseHex4(code)) return false;
                value.push_back(static_cast<wchar_t>(code));
                break;
            }
            default: return Fail(L"未知字符串转义");
            }
        }
        return Fail(L"字符串缺少结束引号");
    }

    bool ParseHex4(unsigned& value) {
        if (position_ + 4 > input_.size()) return Fail(L"Unicode 转义不完整");
        value = 0;
        for (int i = 0; i < 4; ++i) {
            const wchar_t ch = input_[position_++];
            value <<= 4;
            if (ch >= L'0' && ch <= L'9') value |= static_cast<unsigned>(ch - L'0');
            else if (ch >= L'a' && ch <= L'f') value |= static_cast<unsigned>(ch - L'a' + 10);
            else if (ch >= L'A' && ch <= L'F') value |= static_cast<unsigned>(ch - L'A' + 10);
            else return Fail(L"Unicode 转义包含非十六进制字符");
        }
        return true;
    }

    bool ParseArray(JsonValue& value) {
        ++position_;
        value = JsonValue::ArrayValue();
        SkipSpace();
        if (Consume(L']')) return true;
        while (true) {
            JsonValue item;
            if (!ParseValue(item)) return false;
            value.array().push_back(std::move(item));
            SkipSpace();
            if (Consume(L']')) return true;
            if (!Consume(L',')) return Fail(L"数组项之间缺少逗号");
        }
    }

    bool ParseObject(JsonValue& value) {
        ++position_;
        value = JsonValue::ObjectValue();
        SkipSpace();
        if (Consume(L'}')) return true;
        while (true) {
            SkipSpace();
            if (position_ >= input_.size() || input_[position_] != L'"') return Fail(L"对象属性名必须是字符串");
            std::wstring name;
            if (!ParseString(name)) return false;
            if (!Consume(L':')) return Fail(L"对象属性名后缺少冒号");
            JsonValue item;
            if (!ParseValue(item)) return false;
            value.object().emplace_back(std::move(name), std::move(item));
            SkipSpace();
            if (Consume(L'}')) return true;
            if (!Consume(L',')) return Fail(L"对象属性之间缺少逗号");
        }
    }

    bool ParseNumber(JsonValue& value) {
        const std::size_t start = position_;
        if (input_[position_] == L'-') ++position_;
        if (position_ >= input_.size()) return Fail(L"数字不完整");
        if (input_[position_] == L'0') {
            ++position_;
        } else {
            if (input_[position_] < L'1' || input_[position_] > L'9') return Fail(L"无效数字");
            while (position_ < input_.size() && std::iswdigit(input_[position_])) ++position_;
        }
        if (position_ < input_.size() && input_[position_] == L'.') {
            ++position_;
            const std::size_t digits = position_;
            while (position_ < input_.size() && std::iswdigit(input_[position_])) ++position_;
            if (position_ == digits) return Fail(L"小数部分缺少数字");
        }
        if (position_ < input_.size() && (input_[position_] == L'e' || input_[position_] == L'E')) {
            ++position_;
            if (position_ < input_.size() && (input_[position_] == L'+' || input_[position_] == L'-')) ++position_;
            const std::size_t digits = position_;
            while (position_ < input_.size() && std::iswdigit(input_[position_])) ++position_;
            if (position_ == digits) return Fail(L"指数部分缺少数字");
        }
        value = JsonValue::Number(input_.substr(start, position_ - start));
        return true;
    }

    bool Fail(std::wstring message) {
        error_ = std::move(message);
        return false;
    }

    std::wstring input_;
    std::size_t position_ = 0;
    std::wstring error_;
};

void AppendIndent(std::string& output, int depth) {
    output.append(static_cast<std::size_t>(depth) * 2, ' ');
}

void AppendString(std::string& output, std::wstring_view value) {
    output.push_back('"');
    for (wchar_t ch : value) {
        switch (ch) {
        case L'"': output += "\\\""; break;
        case L'\\': output += "\\\\"; break;
        case L'\b': output += "\\b"; break;
        case L'\f': output += "\\f"; break;
        case L'\n': output += "\\n"; break;
        case L'\r': output += "\\r"; break;
        case L'\t': output += "\\t"; break;
        default:
            if (ch < 0x20 || ch >= 0x7f) {
                static constexpr char digits[] = "0123456789abcdef";
                output += "\\u0000";
                output[output.size() - 4] = digits[(ch >> 12) & 0xf];
                output[output.size() - 3] = digits[(ch >> 8) & 0xf];
                output[output.size() - 2] = digits[(ch >> 4) & 0xf];
                output[output.size() - 1] = digits[ch & 0xf];
            } else {
                output += WideToUtf8(std::wstring_view(&ch, 1));
            }
        }
    }
    output.push_back('"');
}

void Serialize(const JsonValue& value, std::string& output, int depth) {
    switch (value.type()) {
    case JsonValue::Type::Null: output += "null"; break;
    case JsonValue::Type::Boolean: output += value.boolean() ? "true" : "false"; break;
    case JsonValue::Type::Number: output += WideToUtf8(value.text()); break;
    case JsonValue::Type::String: AppendString(output, value.text()); break;
    case JsonValue::Type::Array: {
        output.push_back('[');
        if (!value.array().empty()) {
            output.push_back('\n');
            for (std::size_t i = 0; i < value.array().size(); ++i) {
                AppendIndent(output, depth + 1);
                Serialize(value.array()[i], output, depth + 1);
                output += i + 1 == value.array().size() ? "\n" : ",\n";
            }
            AppendIndent(output, depth);
        }
        output.push_back(']');
        break;
    }
    case JsonValue::Type::Object: {
        output.push_back('{');
        if (!value.object().empty()) {
            output.push_back('\n');
            for (std::size_t i = 0; i < value.object().size(); ++i) {
                AppendIndent(output, depth + 1);
                AppendString(output, value.object()[i].first);
                output += ": ";
                Serialize(value.object()[i].second, output, depth + 1);
                output += i + 1 == value.object().size() ? "\n" : ",\n";
            }
            AppendIndent(output, depth);
        }
        output.push_back('}');
        break;
    }
    }
}
}

bool ParseJsonUtf8(std::string_view input, JsonValue& value, std::wstring& error) {
    if (input.size() >= 3 && static_cast<unsigned char>(input[0]) == 0xef &&
        static_cast<unsigned char>(input[1]) == 0xbb && static_cast<unsigned char>(input[2]) == 0xbf) {
        input.remove_prefix(3);
    }
    const std::wstring wide = Utf8ToWide(input);
    if (!input.empty() && wide.empty()) {
        error = L"配置文件不是有效 UTF-8。";
        return false;
    }
    Parser parser(wide);
    return parser.Parse(value, error);
}

std::string SerializeJsonUtf8(const JsonValue& value) {
    std::string output;
    Serialize(value, output, 0);
    output.push_back('\n');
    return output;
}
