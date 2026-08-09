#pragma once

#include <string>
#include <utility>
#include <vector>

class JsonValue {
public:
    enum class Type { Null, Boolean, Number, String, Array, Object };
    using Array = std::vector<JsonValue>;
    using Object = std::vector<std::pair<std::wstring, JsonValue>>;

    JsonValue() = default;
    static JsonValue Boolean(bool value);
    static JsonValue Number(std::wstring value);
    static JsonValue String(std::wstring value);
    static JsonValue ArrayValue();
    static JsonValue ObjectValue();

    Type type() const { return type_; }
    bool boolean() const { return boolean_; }
    const std::wstring& text() const { return text_; }
    std::wstring& text() { return text_; }
    const Array& array() const { return array_; }
    Array& array() { return array_; }
    const Object& object() const { return object_; }
    Object& object() { return object_; }

    JsonValue* Find(std::wstring_view name);
    const JsonValue* Find(std::wstring_view name) const;
    JsonValue& Set(std::wstring name, JsonValue value);

private:
    Type type_ = Type::Null;
    bool boolean_ = false;
    std::wstring text_;
    Array array_;
    Object object_;
};

bool ParseJsonUtf8(std::string_view input, JsonValue& value, std::wstring& error);
std::string SerializeJsonUtf8(const JsonValue& value);
