// -*- mode:c++;indent-tabs-mode:nil;c-basic-offset:4;coding:utf-8 -*-
// vi: set et ft=cpp ts=4 sts=4 sw=4 fenc=utf-8 :vi
//
// Copyright 2024 Mozilla Foundation
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once
#include <map>
#include <string>
#include <vector>

namespace jt {

using MallocFunc = void*(*)(size_t size, size_t alignment, void * userdata);
using FreeFunc = void(*)(void * ptr, void * userdata);

struct JsonContext
{
    MallocFunc malloc_aligned = nullptr;
    FreeFunc free_aligned = nullptr;
    void * userdata = nullptr;
};

struct JsonValue;

struct StringView
{
    char * str;
    size_t len; // Note: store the '\0' for easier compatibility with API requiring null-terminated strings
};

struct JsonArray
{
    JsonValue * ptr;
    size_t len;
};

struct JsonHashTrie
{
    JsonHashTrie* children[4];
    const char* key;
    JsonValue* value;
};

enum class JsonType
{
    Null,
    Bool,
    Long,
    Float,
    Double,
    String,
    Array,
    Object
};

struct JsonValue
{
    JsonType type;
    union
    {
        bool bool_value;
        float float_value;
        double double_value;
        long long long_value;
        std::string string_value_;
        //StringView string_value__; // TODO
        std::vector<JsonValue> array_value;
        std::map<std::string, JsonValue> object_value;
    };
    StringView string_value__ = { nullptr, 0 }; // TODO_REMOVE: tmp dev
};

enum class JsonStatus
{
    success,
    bad_double,
    absent_value,
    bad_negative,
    bad_exponent,
    missing_comma,
    missing_colon,
    malformed_utf8,
    depth_exceeded,
    stack_overflow,
    unexpected_eof,
    overlong_ascii,
    unexpected_comma,
    unexpected_colon,
    unexpected_octal,
    trailing_content,
    illegal_character,
    invalid_hex_escape,
    overlong_utf8_0x7ff,
    overlong_utf8_0xffff,
    object_missing_value,
    illegal_utf8_character,
    invalid_unicode_escape,
    utf16_surrogate_in_utf8,
    unexpected_end_of_array,
    hex_escape_not_printable,
    invalid_escape_character,
    utf8_exceeds_utf16_range,
    unexpected_end_of_string,
    unexpected_end_of_object,
    object_key_must_be_string,
    c1_control_code_in_string,
    non_del_c0_control_code_in_string,
    internal_error_unreachable_code
};

class Json
{
  private:
    const JsonContext & ctx_;
    JsonType type_;
    union
    {
        bool bool_value;
        float float_value;
        double double_value;
        long long long_value;
        std::string string_value_;
        //StringView string_value__; // TODO
        std::vector<Json> array_value;
        std::map<std::string, Json> object_value;
    };
    StringView string_value__ = { nullptr, 0 }; // TODO_REMOVE: tmp dev

  public:
    static const char* StatusToString(JsonStatus);
    static std::pair<JsonStatus, Json> parse(const JsonContext& ctx, const char* s, size_t len);

    Json(const Json&);
    Json(Json&&) noexcept;
    Json(const JsonContext& ctx, unsigned long);
    Json(const JsonContext& ctx, unsigned long long);
    Json(const JsonContext& ctx, const char*);
    // TODO: remove
    Json(const JsonContext& ctx, const std::string&);
    ~Json();

    Json(const JsonContext& ctx, const std::nullptr_t = nullptr) : ctx_(ctx), type_(JsonType::Null)
    {
    }

    Json(const JsonContext& ctx, bool value) : ctx_(ctx), type_(JsonType::Bool), bool_value(value)
    {
    }

    Json(const JsonContext& ctx, int value) : ctx_(ctx), type_(JsonType::Long), long_value(value)
    {
    }

    Json(const JsonContext& ctx, float value) : ctx_(ctx), type_(JsonType::Float), float_value(value)
    {
    }

    Json(const JsonContext& ctx, unsigned value) : ctx_(ctx), type_(JsonType::Long), long_value(value)
    {
    }

    Json(const JsonContext& ctx, long value) : ctx_(ctx), type_(JsonType::Long), long_value(value)
    {
    }

    Json(const JsonContext& ctx, long long value) : ctx_(ctx), type_(JsonType::Long), long_value(value)
    {
    }

    Json(const JsonContext& ctx, double value) : ctx_(ctx), type_(JsonType::Double), double_value(value)
    {
    }

    // TODO: remove
    Json(const JsonContext& ctx, std::string&& value);

    JsonType getType() const
    {
        return type_;
    }

    #if 0
    bool isNull() const
    {
        return type_ == JsonType::Null;
    }

    bool isBool() const
    {
        return type_ == JsonType::Bool;
    }

    bool isNumber() const
    {
        return isFloat() || isDouble() || isLong();
    }

    bool isLong() const
    {
        return type_ == JsonType::Long;
    }

    bool isFloat() const
    {
        return type_ == JsonType::Float;
    }

    bool isDouble() const
    {
        return type_ == JsonType::Double;
    }

    bool isString() const
    {
        return type_ == JsonType::String;
    }

    bool isArray() const
    {
        return type_ == JsonType::Array;
    }

    bool isObject() const
    {
        return type_ == JsonType::Object;
    }
    #endif

    // TODO: use bool getXXX(XXX& v) const instead of XXX getXXX() const + abort()
    bool getBool() const;
    float getFloat() const;
    double getDouble() const;
    double getNumber() const;
    long long getLong() const;

    // TODO: remove and replace by StringView getString() const
    std::string& getString();

    std::vector<Json>& getArray();
    std::map<std::string, Json>& getObject();

    // TODO: replace by: bool contains(const char* str) const
    bool contains(const std::string&) const;

    void setArray();
    void setObject();

    std::string toString() const;
    std::string toStringPretty() const;

    Json& operator=(const Json&);
    Json& operator=(Json&&) noexcept;

    Json& operator[](size_t);
    Json& operator[](const std::string&);

    operator std::string() const
    {
        return toString();
    }

  private:
    void clear();
    void marshal(std::string&, bool, int) const;
    static void stringify(std::string&, const char* input);
    static void serialize(std::string&, const char* input);
    static JsonStatus parse(const JsonContext& ctx, Json&, const char*&, const char*, int, int);
};

} // namespace jt
