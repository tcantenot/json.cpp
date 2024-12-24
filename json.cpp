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

#include "json.h"
#include "jtckdint.h"

#include <cassert>
#include <cctype>
#include <climits>
#include <cstdint>
#include <cstdlib>

#include "double-conversion/double-to-string.h"
#include "double-conversion/string-to-double.h"



#if defined(_MSC_VER)
namespace jt {

inline void* DefaultMallocFunc(size_t size, size_t alignment, void* = nullptr)
{
    return size > 0 ? _aligned_malloc(size, alignment) : nullptr;
}

inline void DefaultFreeFunc(void* ptr, void*)
{
    _aligned_free(ptr);
}

}
#elif defined(__GNUC__) || defined(__clang__)
#if defined(__x86_64__) || defined(_M_X64) || defined(__wasm_simd128__) || defined(__wasm_relaxed_simd__)
#include <xmmintrin.h>
namespace jt {

inline void* DefaultMallocFunc(size_t size, size_t alignment, void*)
{
	return size > 0 ? _mm_malloc(size, alignment) : nullptr;
}

inline void DefaultFreeFunc(void* ptr, void*)
{
    _mm_free(ptr);
}

}
#else
namespace jt {

inline void* DefaultMallocFunc(size_t size, size_t alignment, void*)
{
    return size > 0 ? aligned_alloc(alignment, size) : nullptr;
}

inline void DefaultFreeFunc(void* ptr, void*)
{
    free(ptr);
}

}
#endif
#endif

namespace jt {

inline void* Malloc(const JsonContext& ctx, size_t size, size_t alignment)
{
    MallocFunc m = (ctx.malloc_aligned && ctx.free_aligned) ? ctx.malloc_aligned : DefaultMallocFunc;
    return m(size, alignment, ctx.userdata);
}

inline void Free(const JsonContext& ctx, void* ptr)
{
    FreeFunc f = (ctx.malloc_aligned && ctx.free_aligned) ? ctx.free_aligned : DefaultFreeFunc;
    f(ptr, ctx.userdata);
}

}

#define KEY 1
#define COMMA 2
#define COLON 4
#define ARRAY 8
#define OBJECT 16
#define DEPTH 20

#define ASCII 0
#define C0 1
#define DQUOTE 2
#define BACKSLASH 3
#define UTF8_2 4
#define UTF8_3 5
#define UTF8_4 6
#define C1 7
#define UTF8_3_E0 8
#define UTF8_3_ED 9
#define UTF8_4_F0 10
#define BADUTF8 11
#define EVILUTF8 12

#define UTF16_MASK 0xfc00
#define UTF16_MOAR 0xd800 // 0xD800..0xDBFF
#define UTF16_CONT 0xdc00 // 0xDC00..0xDFFF

#define READ32LE(S) \
    ((uint_least32_t)(255 & (S)[3]) << 030 | \
     (uint_least32_t)(255 & (S)[2]) << 020 | \
     (uint_least32_t)(255 & (S)[1]) << 010 | \
     (uint_least32_t)(255 & (S)[0]) << 000)

#define ThomPikeCont(x) (0200 == (0300 & (x)))
#define ThomPikeByte(x) ((x) & (((1 << ThomPikeMsb(x)) - 1) | 3))
#define ThomPikeLen(x) (7 - ThomPikeMsb(x))
#define ThomPikeMsb(x) ((255 & (x)) < 252 ? Bsr(255 & ~(x)) : 1)
#define ThomPikeMerge(x, y) ((x) << 6 | (077 & (y)))

#define IsSurrogate(wc) ((0xf800 & (wc)) == 0xd800)
#define IsHighSurrogate(wc) (((wc) & UTF16_MASK) == UTF16_MOAR)
#define IsLowSurrogate(wc) (((wc) & UTF16_MASK) == UTF16_CONT)
#define MergeUtf16(hi, lo) ((((hi) - 0xD800) << 10) + ((lo) - 0xDC00) + 0x10000)
#define EncodeUtf16(wc) \
    ((0x0000 <= (wc) && (wc) <= 0xFFFF) || (0xE000 <= (wc) && (wc) <= 0xFFFF) \
       ? (wc) \
     : 0x10000 <= (wc) && (wc) <= 0x10FFFF \
       ? (((((wc) - 0x10000) >> 10) + 0xD800) | \
          (unsigned)((((wc) - 0x10000) & 1023) + 0xDC00) << 16) \
       : 0xFFFD)

namespace jt {

static const char kJsonStr[256] = {
    1,  1,  1,  1,  1,  1,  1,  1, // 0000 ascii (0)
    1,  1,  1,  1,  1,  1,  1,  1, // 0010
    1,  1,  1,  1,  1,  1,  1,  1, // 0020 c0 (1)
    1,  1,  1,  1,  1,  1,  1,  1, // 0030
    0,  0,  2,  0,  0,  0,  0,  0, // 0040 dquote (2)
    0,  0,  0,  0,  0,  0,  0,  0, // 0050
    0,  0,  0,  0,  0,  0,  0,  0, // 0060
    0,  0,  0,  0,  0,  0,  0,  0, // 0070
    0,  0,  0,  0,  0,  0,  0,  0, // 0100
    0,  0,  0,  0,  0,  0,  0,  0, // 0110
    0,  0,  0,  0,  0,  0,  0,  0, // 0120
    0,  0,  0,  0,  3,  0,  0,  0, // 0130 backslash (3)
    0,  0,  0,  0,  0,  0,  0,  0, // 0140
    0,  0,  0,  0,  0,  0,  0,  0, // 0150
    0,  0,  0,  0,  0,  0,  0,  0, // 0160
    0,  0,  0,  0,  0,  0,  0,  0, // 0170
    7,  7,  7,  7,  7,  7,  7,  7, // 0200 c1 (8)
    7,  7,  7,  7,  7,  7,  7,  7, // 0210
    7,  7,  7,  7,  7,  7,  7,  7, // 0220
    7,  7,  7,  7,  7,  7,  7,  7, // 0230
    11, 11, 11, 11, 11, 11, 11, 11, // 0240 latin1 (4)
    11, 11, 11, 11, 11, 11, 11, 11, // 0250
    11, 11, 11, 11, 11, 11, 11, 11, // 0260
    11, 11, 11, 11, 11, 11, 11, 11, // 0270
    12, 12, 4,  4,  4,  4,  4,  4, // 0300 utf8-2 (5)
    4,  4,  4,  4,  4,  4,  4,  4, // 0310
    4,  4,  4,  4,  4,  4,  4,  4, // 0320 utf8-2
    4,  4,  4,  4,  4,  4,  4,  4, // 0330
    8,  5,  5,  5,  5,  5,  5,  5, // 0340 utf8-3 (6)
    5,  5,  5,  5,  5,  9,  5,  5, // 0350
    10, 6,  6,  6,  6,  11, 11, 11, // 0360 utf8-4 (7)
    11, 11, 11, 11, 11, 11, 11, 11, // 0370
};

static const char kEscapeLiteral[128] = {
    9, 9, 9, 9, 9, 9, 9, 9, 9, 1, 2, 9, 4, 3, 9, 9, // 0x00
    9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, 9, // 0x10
    0, 0, 7, 0, 0, 0, 9, 9, 0, 0, 0, 0, 0, 0, 0, 6, // 0x20
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9, 9, 9, 0, // 0x30
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 0x40
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5, 0, 0, 0, // 0x50
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, // 0x60
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 9, // 0x70
};

alignas(signed char) static const signed char kHexToInt[256] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, // 0x00
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, // 0x10
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, // 0x20
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  -1, -1, -1, -1, -1, -1, // 0x30
    -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1, // 0x40
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, // 0x50
    -1, 10, 11, 12, 13, 14, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1, // 0x60
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, // 0x70
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, // 0x80
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, // 0x90
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, // 0xa0
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, // 0xb0
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, // 0xc0
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, // 0xd0
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, // 0xe0
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, // 0xf0
};

static const double_conversion::DoubleToStringConverter kDoubleToJson(
  double_conversion::DoubleToStringConverter::UNIQUE_ZERO |
    double_conversion::DoubleToStringConverter::EMIT_POSITIVE_EXPONENT_SIGN,
  "1e5000",
  "null",
  'e',
  -6,
  21,
  6,
  0);

static const double_conversion::StringToDoubleConverter kJsonToDouble(
  double_conversion::StringToDoubleConverter::ALLOW_CASE_INSENSITIVITY |
    double_conversion::StringToDoubleConverter::ALLOW_LEADING_SPACES |
    double_conversion::StringToDoubleConverter::ALLOW_TRAILING_JUNK |
    double_conversion::StringToDoubleConverter::ALLOW_TRAILING_SPACES,
  0.0,
  1.0,
  "Infinity",
  "NaN");

#if defined(__GNUC__) || defined(__clang__)
#define Bsr(x) (__builtin_clz(x) ^ (sizeof(int) * CHAR_BIT - 1))
#else
static int
Bsr(int x)
{
    int r = 0;
    if (x & 0xFFFF0000u) {
        x >>= 16;
        r |= 16;
    }
    if (x & 0xFF00) {
        x >>= 8;
        r |= 8;
    }
    if (x & 0xF0) {
        x >>= 4;
        r |= 4;
    }
    if (x & 0xC) {
        x >>= 2;
        r |= 2;
    }
    if (x & 0x2) {
        r |= 1;
    }
    return r;
}
#endif

static double
StringToDouble(const char* s, size_t n, int* out_processed)
{
    if (n == (size_t)-1)
        n = strlen(s);
    int processed;
    double res = kJsonToDouble.StringToDouble(s, n, &processed);
    if (out_processed)
        *out_processed = processed;
    return res;
}

static char*
UlongToString(char* p, unsigned long long x)
{
    char t;
    size_t i, a, b;
    i = 0;
    do {
        p[i++] = x % 10 + '0';
        x = x / 10;
    } while (x > 0);
    p[i] = '\0';
    if (i) {
        for (a = 0, b = i - 1; a < b; ++a, --b) {
            t = p[a];
            p[a] = p[b];
            p[b] = t;
        }
    }
    return p + i;
}

static char*
LongToString(char* p, long long x)
{
    if (x < 0)
        *p++ = '-', x = 0 - (unsigned long long)x;
    return UlongToString(p, x);
}

Json::Json(const JsonContext& ctx, unsigned long value): ctx_(ctx)
{
    if (value <= LLONG_MAX) {
        type_ = JsonType::Long;
        long_value = value;
    } else {
        type_ = JsonType::Double;
        double_value = value;
    }
}

Json::Json(const JsonContext& ctx, unsigned long long value): ctx_(ctx)
{
    if (value <= LLONG_MAX) {
        type_ = JsonType::Long;
        long_value = value;
    } else {
        type_ = JsonType::Double;
        double_value = value;
    }
}

Json::Json(const JsonContext& ctx, const char* value): ctx_(ctx)
{
    if (value) {
        type_ = JsonType::String;
        new (&string_value_) std::string(value);

        const size_t len = strlen(value) + 1;
        string_value__.str = (char*)Malloc(ctx, len, 1);
        string_value__.len = len;
        memcpy(string_value__.str, value, len);

    } else {
        type_ = JsonType::Null;
    }
}

Json::Json(const JsonContext& ctx, const std::string& value): ctx_(ctx), type_(JsonType::String), string_value_(value)
{
    const size_t len = value.size() + 1;
    string_value__.str = (char*)Malloc(ctx, len, 1);
    string_value__.len = len;
    memcpy(string_value__.str, value.data(), len);
}

Json::Json(const JsonContext& ctx, std::string&& value) : ctx_(ctx), type_(JsonType::String), string_value_(std::move(value))
{
    const size_t len = string_value_.size() + 1;
    string_value__.str = (char*)Malloc(ctx, len, 1);
    string_value__.len = len;
    memcpy(string_value__.str, string_value_.data(), len);
}

Json::~Json()
{
    if (type_ >= JsonType::String)
        clear();
}

void
Json::clear()
{
    switch (type_) {
        case JsonType::String:
            string_value_.~basic_string();
            Free(ctx_, string_value__.str);
            string_value__.str = nullptr;
            string_value__.len = 0;
            break;
        case JsonType::Array:
            array_value.~vector();
            break;
        case JsonType::Object:
            object_value.~map();
            break;
        default:
            break;
    }
    type_ = JsonType::Null;
}

Json::Json(const Json& other) : ctx_(other.ctx_), type_(other.type_)
{
    switch (type_) {
        case JsonType::Null:
            break;
        case JsonType::Bool:
            bool_value = other.bool_value;
            break;
        case JsonType::Long:
            long_value = other.long_value;
            break;
        case JsonType::Float:
            float_value = other.float_value;
            break;
        case JsonType::Double:
            double_value = other.double_value;
            break;
        case JsonType::String:
            new (&string_value_) std::string(other.string_value_);
            string_value__.str = (char*)Malloc(ctx_, other.string_value__.len, 1);
            string_value__.len = other.string_value__.len;
            memcpy(string_value__.str, other.string_value__.str, other.string_value__.len);
            break;
        case JsonType::Array:
            new (&array_value) std::vector<Json>(other.array_value);
            break;
        case JsonType::Object:
            new (&object_value) std::map<std::string, Json>(other.object_value);
            break;
        default:
            abort();
    }
}

// Note: does not copy JsonContext
Json&
Json::operator=(const Json& other)
{
    if (this != &other) {
        if (type_ >= JsonType::String)
            clear();
        type_ = other.type_;
        switch (type_) {
            case JsonType::Null:
                break;
            case JsonType::Bool:
                bool_value = other.bool_value;
                break;
            case JsonType::Long:
                long_value = other.long_value;
                break;
            case JsonType::Float:
                float_value = other.float_value;
                break;
            case JsonType::Double:
                double_value = other.double_value;
                break;
            case JsonType::String:
                new (&string_value_) std::string(other.string_value_);
                string_value__.str = (char*)Malloc(ctx_, other.string_value__.len, 1);
                string_value__.len = other.string_value__.len;
                memcpy(string_value__.str, other.string_value__.str, other.string_value__.len);
                break;
            case JsonType::Array:
                new (&array_value) std::vector<Json>(other.array_value);
                break;
            case JsonType::Object:
                new (&object_value)
                  std::map<std::string, Json>(other.object_value);
                break;
            default:
                abort();
        }
    }
    return *this;
}

Json::Json(Json&& other) noexcept : ctx_(other.ctx_), type_(other.type_)
{
    switch (type_) {
        case JsonType::Null:
            break;
        case JsonType::Bool:
            bool_value = other.bool_value;
            break;
        case JsonType::Long:
            long_value = other.long_value;
            break;
        case JsonType::Float:
            float_value = other.float_value;
            break;
        case JsonType::Double:
            double_value = other.double_value;
            break;
        case JsonType::String:
            new (&string_value_) std::string(std::move(other.string_value_));
            string_value__.str = other.string_value__.str;
            string_value__.len = other.string_value__.len;
            other.string_value__.str = nullptr;
            other.string_value__.len = 0;
            break;
        case JsonType::Array:
            new (&array_value) std::vector<Json>(std::move(other.array_value));
            break;
        case JsonType::Object:
            new (&object_value)
              std::map<std::string, Json>(std::move(other.object_value));
            break;
        default:
            abort();
    }
    other.type_ = JsonType::Null;
}

Json&
Json::operator=(Json&& other) noexcept
{
    if (this != &other) {
        if (type_ >= JsonType::String)
            clear();
        type_ = other.type_;
        switch (type_) {
            case JsonType::Null:
                break;
            case JsonType::Bool:
                bool_value = other.bool_value;
                break;
            case JsonType::Long:
                long_value = other.long_value;
                break;
            case JsonType::Float:
                float_value = other.float_value;
                break;
            case JsonType::Double:
                double_value = other.double_value;
                break;
            case JsonType::String:
                new (&string_value_) std::string(std::move(other.string_value_));

                if(&other.ctx_ == &ctx_) {
                    string_value__.str = other.string_value__.str;
                    string_value__.len = other.string_value__.len;
                    other.string_value__.str = nullptr;
                    other.string_value__.len = 0;
                }
                else {
                    string_value__.str = (char*)Malloc(ctx_, other.string_value__.len, 1);
                    string_value__.len = other.string_value__.len;
                    memcpy(string_value__.str, other.string_value__.str, other.string_value__.len);
                    Free(other.ctx_, other.string_value__.str);
                    other.string_value__.str = nullptr;
                    other.string_value__.len = 0;
                }
                break;
            case JsonType::Array:
                new (&array_value)
                  std::vector<Json>(std::move(other.array_value));
                break;
            case JsonType::Object:
                new (&object_value)
                  std::map<std::string, Json>(std::move(other.object_value));
                break;
            default:
                abort();
        }
        other.type_ = JsonType::Null;
    }
    return *this;
}

double
Json::getNumber() const
{
    switch (type_) {
        case JsonType::Long:
            return long_value;
        case JsonType::Float:
            return float_value;
        case JsonType::Double:
            return double_value;
        default:
            abort();
    }
}

long long
Json::getLong() const
{
    switch (type_) {
        case JsonType::Long:
            return long_value;
        default:
            abort();
    }
}

bool
Json::getBool() const
{
    switch (type_) {
        case JsonType::Bool:
            return bool_value;
        default:
            abort();
    }
}

float
Json::getFloat() const
{
    switch (type_) {
        case JsonType::Float:
            return float_value;
        case JsonType::Double:
            return double_value;
        default:
            abort();
    }
}

double
Json::getDouble() const
{
    switch (type_) {
        case JsonType::Float:
            return float_value;
        case JsonType::Double:
            return double_value;
        default:
            abort();
    }
}

std::string&
Json::getString()
{
    switch (type_) {
        case JsonType::String:
            return string_value_;
        default:
            abort();
    }
}

std::vector<Json>&
Json::getArray()
{
    switch (type_) {
        case JsonType::Array:
            return array_value;
        default:
            abort();
    }
}

std::map<std::string, Json>&
Json::getObject()
{
    switch (type_) {
        case JsonType::Object:
            return object_value;
        default:
            abort();
    }
}

void
Json::setArray()
{
    if (type_ >= JsonType::String)
        clear();
    type_ = JsonType::Array;
    new (&array_value) std::vector<Json>();
}

void
Json::setObject()
{
    if (type_ >= JsonType::String)
        clear();
    type_ = JsonType::Object;
    new (&object_value) std::map<std::string, Json>();
}

bool
Json::contains(const std::string& key) const
{
    if (type_ != JsonType::Object)
        return false;
    return object_value.find(key) != object_value.end();
}

Json&
Json::operator[](size_t index)
{
    if (type_ != JsonType::Array)
        setArray();
    if (index >= array_value.size()) {
        //array_value.resize(index + 1);
        array_value.emplace_back(Json{ctx_});
    }
    return array_value[index];
}

Json&
Json::operator[](const std::string& key)
{
    if (type_ != JsonType::Object)
        setObject();
    //return object_value[key];

    auto it = object_value.find(key);
    if(it != object_value.end())
        return it->second;

    return object_value.emplace(key, Json{ctx_}).first->second;
}

std::string
Json::toString() const
{
    std::string b;
    marshal(b, false, 0);
    return b;
}

std::string
Json::toStringPretty() const
{
    std::string b;
    marshal(b, true, 0);
    return b;
}

void
Json::marshal(std::string& b, bool pretty, int indent) const
{
    switch (type_) {
        case JsonType::Null:
            b += "null";
            break;
        case JsonType::String:
            stringify(b, string_value_.c_str());
            break;
        case JsonType::Bool:
            b += bool_value ? "true" : "false";
            break;
        case JsonType::Long: {
            char buf[64];
            b.append(buf, LongToString(buf, long_value) - buf);
            break;
        }
        case JsonType::Float: {
            char buf[128];
            double_conversion::StringBuilder db(buf, 128);
            kDoubleToJson.ToShortestSingle(float_value, &db);
            db.Finalize();
            b += buf;
            break;
        }
        case JsonType::Double: {
            char buf[128];
            double_conversion::StringBuilder db(buf, 128);
            kDoubleToJson.ToShortest(double_value, &db);
            db.Finalize();
            b += buf;
            break;
        }
        case JsonType::Array: {
            bool once = false;
            b += '[';
            for (auto i = array_value.begin(); i != array_value.end(); ++i) {
                if (once) {
                    b += ',';
                    if (pretty)
                        b += ' ';
                } else {
                    once = true;
                }
                i->marshal(b, pretty, indent);
            }
            b += ']';
            break;
        }
        case JsonType::Object: {
            bool once = false;
            b += '{';
            for (auto i = object_value.begin(); i != object_value.end(); ++i) {
                if (once) {
                    b += ',';
                } else {
                    once = true;
                }
                if (pretty && object_value.size() > 1) {
                    b += '\n';
                    ++indent;
                    for (int j = 0; j < indent; ++j)
                        b += "  ";
                }
                stringify(b, i->first.c_str());
                b += ':';
                if (pretty)
                    b += ' ';
                i->second.marshal(b, pretty, indent);
                if (pretty && object_value.size() > 1)
                    --indent;
            }
            if (pretty && object_value.size() > 1) {
                b += '\n';
                for (int j = 0; j < indent; ++j)
                    b += "  ";
                ++indent;
            }
            b += '}';
            break;
        }
        default:
            abort();
    }
}

void
Json::stringify(std::string& b, const char* input)
{
    b += '"';
    serialize(b, input);
    b += '"';
}

void
Json::serialize(std::string& sb, const char* input)
{
    size_t i, j, m;
    wint_t x, a, b;
    unsigned long long w;
    size_t len = strlen(input);
    for (i = 0; i < len;) {
        x = input[i++] & 255;
        if (x >= 0300) {
            a = ThomPikeByte(x);
            m = ThomPikeLen(x) - 1;
            if (i + m <= len) {
                for (j = 0;;) {
                    b = input[i + j] & 0xff;
                    if (!ThomPikeCont(b))
                        break;
                    a = ThomPikeMerge(a, b);
                    if (++j == m) {
                        x = a;
                        i += j;
                        break;
                    }
                }
            }
        }
        switch (0 <= x && x <= 127 ? kEscapeLiteral[x] : 9) {
            case 0:
                sb += x;
                break;
            case 1:
                sb += "\\t";
                break;
            case 2:
                sb += "\\n";
                break;
            case 3:
                sb += "\\r";
                break;
            case 4:
                sb += "\\f";
                break;
            case 5:
                sb += "\\\\";
                break;
            case 6:
                sb += "\\/";
                break;
            case 7:
                sb += "\\\"";
                break;
            case 9:
                w = EncodeUtf16(x);
                do {
                    char esc[6];
                    esc[0] = '\\';
                    esc[1] = 'u';
                    esc[2] = "0123456789abcdef"[(w & 0xF000) >> 014];
                    esc[3] = "0123456789abcdef"[(w & 0x0F00) >> 010];
                    esc[4] = "0123456789abcdef"[(w & 0x00F0) >> 004];
                    esc[5] = "0123456789abcdef"[(w & 0x000F) >> 000];
                    sb.append(esc, 6);
                } while ((w >>= 16));
                break;
            default:
                abort();
        }
    }
}

static inline JsonStatus ReturnColonCommaErrorStatus(int context)
{
    if (context & COLON)
        return JsonStatus::missing_colon;
    return JsonStatus::missing_comma;
}

static inline JsonStatus ReturnColonCommaKeyErrorStatus(int context)
{
    if (context & KEY)
        return JsonStatus::object_key_must_be_string;
    return ReturnColonCommaErrorStatus(context);
}

static inline int EncodeUTF8(char w[4], int c)
{
    int i = 0;
    if (c <= 0x7f) {
        w[0] = c;
        i = 1;
    } else if (c <= 0x7ff) {
        w[0] = 0300 | (c >> 6);
        w[1] = 0200 | (c & 077);
        i = 2;
    } else if (c <= 0xffff) {
        if (IsSurrogate(c)) {
            c = 0xfffd;
        }
        w[0] = 0340 | (c >> 12);
        w[1] = 0200 | ((c >> 6) & 077);
        w[2] = 0200 | (c & 077);
        i = 3;
    } else if (~(c >> 18) & 007) {
        w[0] = 0360 | (c >> 18);
        w[1] = 0200 | ((c >> 12) & 077);
        w[2] = 0200 | ((c >> 6) & 077);
        w[3] = 0200 | (c & 077);
        i = 4;
    } else {
        c = 0xfffd;
        w[0] = 0340 | (c >> 12);
        w[1] = 0200 | ((c >> 6) & 077);
        w[2] = 0200 | (c & 077);
        i = 3;
    }
    return i;
}

// Echo invalid \uXXXX sequences
// Rather than corrupting UTF-8!
static inline const char* BadUnicode()
{
    return "\\u";
}

JsonStatus
Json::parse(const JsonContext& ctx, Json& json, const char*& p, const char* e, int context, int depth)
{
    char w[4];
    long long x;
    const char* a;
    int A, B, C, D, c, d, i, u;
    if (!depth)
        return JsonStatus::depth_exceeded;
    for (a = p, d = +1; p < e;) {
        switch ((c = *p++ & 255)) {
            case ' ': // spaces
            case '\n':
            case '\r':
            case '\t':
                a = p;
                break;

            case ',': // present in list and object
                if (context & COMMA) {
                    context = 0;
                    a = p;
                    break;
                } else {
                    return JsonStatus::unexpected_comma;
                }

            case ':': // present only in object after key
                if (context & COLON) {
                    context = 0;
                    a = p;
                    break;
                } else {
                    return JsonStatus::unexpected_colon;
                }

            case 'n': // null
                if (context & (KEY | COLON | COMMA))
                    return ReturnColonCommaKeyErrorStatus(context);
                if (p + 3 <= e && READ32LE(p - 1) == READ32LE("null")) {
                    p += 3;
                    return JsonStatus::success;
                } else {
                    return JsonStatus::illegal_character;
                }

            case 'f': // false
                if (context & (KEY | COLON | COMMA))
                    return ReturnColonCommaKeyErrorStatus(context);
                if (p + 4 <= e && READ32LE(p) == READ32LE("alse")) {
                    json.type_ = JsonType::Bool;
                    json.bool_value = false;
                    p += 4;
                    return JsonStatus::success;
                } else {
                    return JsonStatus::illegal_character;
                }

            case 't': // true
                if (context & (KEY | COLON | COMMA))
                    return ReturnColonCommaKeyErrorStatus(context);
                if (p + 3 <= e && READ32LE(p - 1) == READ32LE("true")) {
                    json.type_ = JsonType::Bool;
                    json.bool_value = true;
                    p += 3;
                    return JsonStatus::success;
                } else {
                    return JsonStatus::illegal_character;
                }

            default:
                return JsonStatus::illegal_character;

            case '-': // negative
                if (context & (COLON | COMMA | KEY))
                    return ReturnColonCommaKeyErrorStatus(context);
                if (p < e && isdigit(*p)) {
                    d = -1;
                    break;
                } else {
                    return JsonStatus::bad_negative;
                }

            case '0': { // zero or number
                if (context & (COLON | COMMA | KEY))
                    return ReturnColonCommaKeyErrorStatus(context);

                bool bUseDouble = false;
                if (p < e) {
                    if (*p == '.') {
                        if (p + 1 == e || !isdigit(p[1]))
                            return JsonStatus::bad_double;
                        bUseDouble = true;
                    } else if (*p == 'e' || *p == 'E') {
                        bUseDouble = true;
                    } else if (isdigit(*p)) {
                        return JsonStatus::unexpected_octal;
                    }
                }
                if(bUseDouble) {
                    json.type_ = JsonType::Double;
                    json.double_value = StringToDouble(a, e - a, &c);
                    if (c <= 0)
                        return JsonStatus::bad_double;
                    if (a + c < e && (a[c] == 'e' || a[c] == 'E'))
                        return JsonStatus::bad_exponent;
                    p = a + c;
                    return JsonStatus::success;
                } else {
                    json.type_ = JsonType::Long;
                    json.long_value = 0;
                    return JsonStatus::success;
                }
            }

            case '1':
            case '2':
            case '3':
            case '4':
            case '5':
            case '6':
            case '7':
            case '8':
            case '9': { // integer
                if (context & (COLON | COMMA | KEY))
                    return ReturnColonCommaKeyErrorStatus(context);

                bool bUseDouble = false;
                for (x = (c - '0') * d; p < e; ++p) {
                    c = *p & 255;
                    if (isdigit(c)) {
                        if (ckd_mul(&x, x, 10) ||
                            ckd_add(&x, x, (c - '0') * d)) {
                            bUseDouble = true;
                            break;
                        }
                    } else if (c == '.') {
                        if (p + 1 == e || !isdigit(p[1]))
                            return JsonStatus::bad_double;
                        bUseDouble = true;
                        break;
                    } else if (c == 'e' || c == 'E') {
                        bUseDouble = true;
                        break;
                    } else {
                        break;
                    }
                }
                if(bUseDouble) {
                    json.type_ = JsonType::Double;
                    json.double_value = StringToDouble(a, e - a, &c);
                    if (c <= 0)
                        return JsonStatus::bad_double;
                    if (a + c < e && (a[c] == 'e' || a[c] == 'E'))
                        return JsonStatus::bad_exponent;
                    p = a + c;
                    return JsonStatus::success;
                } else {
                    json.type_ = JsonType::Long;
                    json.long_value = x;
                    return JsonStatus::success;
                }
            }

            case '[': { // Array
                if (context & (COLON | COMMA | KEY))
                    return ReturnColonCommaKeyErrorStatus(context);
                json.setArray();
                Json value(ctx);
                for (context = ARRAY, i = 0;;) {
                    JsonStatus status = parse(ctx, value, p, e, context, depth - 1);
                    if (status == JsonStatus::absent_value)
                        return JsonStatus::success;
                    if (status != JsonStatus::success)
                        return status;
                    json.array_value.emplace_back(std::move(value));
                    context = ARRAY | COMMA;
                }
            }

            case ']':
                if (context & ARRAY)
                    return JsonStatus::absent_value;
                return JsonStatus::unexpected_end_of_array;

            case '}':
                if (context & OBJECT)
                    return JsonStatus::absent_value;
                return JsonStatus::unexpected_end_of_object;

            case '{': { // Object
                if (context & (COLON | COMMA | KEY))
                    return ReturnColonCommaKeyErrorStatus(context);
                json.setObject();
                context = KEY | OBJECT;
                Json key(ctx), value(ctx);
                for (;;) {
                    JsonStatus status = parse(ctx, key, p, e, context, depth - 1);
                    if (status == JsonStatus::absent_value)
                        return JsonStatus::success;
                    if (status != JsonStatus::success)
                        return status;
                    if (key.type_ != JsonType::String)
                        return JsonStatus::object_key_must_be_string;
                    status = parse(ctx, value, p, e, COLON, depth - 1);
                    if (status == JsonStatus::absent_value)
                        return JsonStatus::object_missing_value;
                    if (status != JsonStatus::success)
                        return status;
                    json.object_value.emplace(std::move(key.string_value_),
                                              std::move(value));
                    context = KEY | COMMA | OBJECT;
                    key.clear();
                }
            }

            case '"': { // string
                if (context & (COLON | COMMA))
                    return ReturnColonCommaErrorStatus(context);

                // TODO: use ctx
                std::string b;

                bool bDone = false;
                JsonStatus status = JsonStatus::success;
                while(!bDone && (status == JsonStatus::success)) {
                    if (p >= e) {
                        status = JsonStatus::unexpected_end_of_string;
                        break;
                    }
                    const char k = kJsonStr[(c = *p++ & 255)];
                    switch (k) {

                        case ASCII:
                            b += c;
                            break;

                        case DQUOTE:
                            json.type_ = JsonType::String;
                            new (&json.string_value_) std::string(std::move(b));
                            bDone = true;
                            break;

                        case BACKSLASH:
                            if (p >= e) {
                                status = JsonStatus::unexpected_end_of_string;
                                break;
                            }
                            switch ((c = *p++ & 255)) {
                                case '"':
                                case '/':
                                case '\\':
                                    b += c;
                                    break;
                                case 'b':
                                    b += '\b';
                                    break;
                                case 'f':
                                    b += '\f';
                                    break;
                                case 'n':
                                    b += '\n';
                                    break;
                                case 'r':
                                    b += '\r';
                                    break;
                                case 't':
                                    b += '\t';
                                    break;
                                case 'x':
                                    if (p + 2 <= e && //
                                        (A = kHexToInt[p[0] & 255]) !=
                                          -1 && // HEX
                                        (B = kHexToInt[p[1] & 255]) != -1) { //
                                        c = A << 4 | B;
                                        if (!(0x20 <= c && c <= 0x7E)) {
                                            status = JsonStatus::hex_escape_not_printable;
                                            break;
                                        }
                                        p += 2;
                                        b += c;
                                        break;
                                    } else {
                                        status = JsonStatus::invalid_hex_escape;
                                        break;
                                    }
                                case 'u':
                                    if (p + 4 <= e && //
                                        (A = kHexToInt[p[0] & 255]) != -1 && //
                                        (B = kHexToInt[p[1] & 255]) !=
                                          -1 && // UCS-2
                                        (C = kHexToInt[p[2] & 255]) != -1 && //
                                        (D = kHexToInt[p[3] & 255]) != -1) { //
                                        c = A << 12 | B << 8 | C << 4 | D;
                                        if (!IsSurrogate(c)) {
                                            p += 4;
                                            i = EncodeUTF8(w, c);
                                            b.append(w, i);
                                        } else if (IsHighSurrogate(c)) {
                                            if (p + 4 + 6 <= e && //
                                                p[4] == '\\' && //
                                                p[5] == 'u' && //
                                                (A = kHexToInt[p[6] & 255]) !=
                                                  -1 && // UTF-16
                                                (B = kHexToInt[p[7] & 255]) !=
                                                  -1 && //
                                                (C = kHexToInt[p[8] & 255]) !=
                                                  -1 && //
                                                (D = kHexToInt[p[9] & 255]) !=
                                                  -1) { //
                                                u =
                                                  A << 12 | B << 8 | C << 4 | D;
                                                if (IsLowSurrogate(u)) {
                                                    p += 4 + 6;
                                                    c = MergeUtf16(c, u);
                                                    i = EncodeUTF8(w, c);
                                                    b.append(w, i);
                                                } else {
                                                    b += BadUnicode();
                                                    break;
                                                }
                                            } else {
                                                b += BadUnicode();
                                                break;
                                            }
                                        } else {
                                            b += BadUnicode();
                                            break;
                                        }
                                    } else {
                                        status = JsonStatus::invalid_unicode_escape;
                                        break;
                                    }
                                    break;
                                default:
                                    status = JsonStatus::invalid_escape_character;
                                    break;
                            }
                            break;

                        case UTF8_2:
                            if (p < e && //
                                (p[0] & 0300) == 0200) { //
                                c = (c & 037) << 6 | //
                                    (p[0] & 077); //
                                p += 1;
                                i = EncodeUTF8(w, c);
                                b.append(w, i);
                                break;
                            } else {
                                status = JsonStatus::malformed_utf8;
                                break;
                            }

                        case UTF8_3_E0:
                        case UTF8_3_ED:
                            if(k == UTF8_3_E0) {
                                if (p + 2 <= e && //
                                    (p[0] & 0377) < 0240 && //
                                    (p[0] & 0300) == 0200 && //
                                    (p[1] & 0300) == 0200) {
                                    status = JsonStatus::overlong_utf8_0x7ff;
                                    break;
                                }
                            } else { // k == UTF8_3_ED
                                if (p + 2 <= e && //
                                (p[0] & 0377) >= 0240) { //
                                    if (p + 5 <= e && //
                                        (p[0] & 0377) >= 0256 && //
                                        (p[1] & 0300) == 0200 && //
                                        (p[2] & 0377) == 0355 && //
                                        (p[3] & 0377) >= 0260 && //
                                        (p[4] & 0300) == 0200) { //
                                        A = (0355 & 017) << 12 | // CESU-8
                                            (p[0] & 077) << 6 | //
                                            (p[1] & 077); //
                                        B = (0355 & 017) << 12 | //
                                            (p[3] & 077) << 6 | //
                                            (p[4] & 077); //
                                        c = ((A - 0xDB80) << 10) + //
                                            ((B - 0xDC00) + 0x10000); //
                                        i = EncodeUTF8(w, c);
                                        b.append(w, i);
                                        break;
                                    } else if ((p[0] & 0300) == 0200 && //
                                               (p[1] & 0300) == 0200) { //
                                        status = JsonStatus::utf16_surrogate_in_utf8;
                                        break;
                                    } else {
                                        status = JsonStatus::malformed_utf8;
                                        break;
                                    }
                                }
                            }
                            // fallthrough

                        case UTF8_3:
                            if (p + 2 <= e && //
                                (p[0] & 0300) == 0200 && //
                                (p[1] & 0300) == 0200) { //
                                c = (c & 017) << 12 | //
                                    (p[0] & 077) << 6 | //
                                    (p[1] & 077); //
                                p += 2;
                                i = EncodeUTF8(w, c);
                                b.append(w, i);
                                break;
                            } else {
                                status = JsonStatus::malformed_utf8;
                                break;
                            }

                        case UTF8_4_F0:
                            if (p + 3 <= e && (p[0] & 0377) < 0220 &&
                                (((uint_least32_t)(p[+2] & 0377) << 030 |
                                  (uint_least32_t)(p[+1] & 0377) << 020 |
                                  (uint_least32_t)(p[+0] & 0377) << 010 |
                                  (uint_least32_t)(p[-1] & 0377) << 000) &
                                 0xC0C0C000) == 0x80808000) {
                                status = JsonStatus::overlong_utf8_0xffff;
                                break;
                            }
                            // fallthrough
                        case UTF8_4:
                            if (p + 3 <= e && //
                                ((A =
                                    ((uint_least32_t)(p[+2] & 0377) << 030 | //
                                     (uint_least32_t)(p[+1] & 0377) << 020 | //
                                     (uint_least32_t)(p[+0] & 0377) << 010 | //
                                     (uint_least32_t)(p[-1] & 0377)
                                       << 000)) & //
                                 0xC0C0C000) == 0x80808000) { //
                                A = (A & 7) << 18 | //
                                    (A & (077 << 010)) << (12 - 010) | //
                                    (A & (077 << 020)) >> -(6 - 020) | //
                                    (A & (077 << 030)) >> 030; //
                                if (A <= 0x10FFFF) {
                                    c = A;
                                    p += 3;
                                    i = EncodeUTF8(w, c);
                                    b.append(w, i);
                                    break;
                                } else {
                                    status = JsonStatus::utf8_exceeds_utf16_range;
                                    break;
                                }
                            } else {
                                status = JsonStatus::malformed_utf8;
                                break;
                            }

                        case EVILUTF8:
                            if (p < e && (p[0] & 0300) == 0200) {
                                status = JsonStatus::overlong_ascii;
                                break;
                            }
                            // fallthrough
                        case BADUTF8:
                            status = JsonStatus::illegal_utf8_character;
                            break;
                        case C0:
                            status = JsonStatus::non_del_c0_control_code_in_string;
                            break;
                        case C1:
                            status = JsonStatus::c1_control_code_in_string;
                            break;
                        default:
                            status = JsonStatus::internal_error_unreachable_code;
                            break;
                    }
                }
                return status;
            }
        }
    }
    if (depth == DEPTH)
        return JsonStatus::absent_value;
    return JsonStatus::unexpected_eof;
}

std::pair<JsonStatus, Json>
Json::parse(const JsonContext& ctx, const char* s, size_t len)
{
    JsonStatus s2;
    std::pair<JsonStatus, Json> res = { { }, { ctx } };
    const char* p = s;
    const char* e = s + len;
    res.first = parse(ctx, res.second, p, e, 0, DEPTH);
    if (res.first == JsonStatus::success) {
        Json j2(ctx);
        s2 = parse(ctx, j2, p, e, 0, DEPTH);
        if (s2 != JsonStatus::absent_value)
            res.first = JsonStatus::trailing_content;
    }
    return res;
}

const char*
Json::StatusToString(JsonStatus status)
{
    switch (status) {
        case JsonStatus::success:
            return "success";
        case JsonStatus::bad_double:
            return "bad_double";
        case JsonStatus::absent_value:
            return "absent_value";
        case JsonStatus::bad_negative:
            return "bad_negative";
        case JsonStatus::bad_exponent:
            return "bad_exponent";
        case JsonStatus::missing_comma:
            return "missing_comma";
        case JsonStatus::missing_colon:
            return "missing_colon";
        case JsonStatus::malformed_utf8:
            return "malformed_utf8";
        case JsonStatus::depth_exceeded:
            return "depth_exceeded";
        case JsonStatus::stack_overflow:
            return "stack_overflow";
        case JsonStatus::unexpected_eof:
            return "unexpected_eof";
        case JsonStatus::overlong_ascii:
            return "overlong_ascii";
        case JsonStatus::unexpected_comma:
            return "unexpected_comma";
        case JsonStatus::unexpected_colon:
            return "unexpected_colon";
        case JsonStatus::unexpected_octal:
            return "unexpected_octal";
        case JsonStatus::trailing_content:
            return "trailing_content";
        case JsonStatus::illegal_character:
            return "illegal_character";
        case JsonStatus::invalid_hex_escape:
            return "invalid_hex_escape";
        case JsonStatus::overlong_utf8_0x7ff:
            return "overlong_utf8_0x7ff";
        case JsonStatus::overlong_utf8_0xffff:
            return "overlong_utf8_0xffff";
        case JsonStatus::object_missing_value:
            return "object_missing_value";
        case JsonStatus::illegal_utf8_character:
            return "illegal_utf8_character";
        case JsonStatus::invalid_unicode_escape:
            return "invalid_unicode_escape";
        case JsonStatus::utf16_surrogate_in_utf8:
            return "utf16_surrogate_in_utf8";
        case JsonStatus::unexpected_end_of_array:
            return "unexpected_end_of_array";
        case JsonStatus::hex_escape_not_printable:
            return "hex_escape_not_printable";
        case JsonStatus::invalid_escape_character:
            return "invalid_escape_character";
        case JsonStatus::utf8_exceeds_utf16_range:
            return "utf8_exceeds_utf16_range";
        case JsonStatus::unexpected_end_of_string:
            return "unexpected_end_of_string";
        case JsonStatus::unexpected_end_of_object:
            return "unexpected_end_of_object";
        case JsonStatus::object_key_must_be_string:
            return "object_key_must_be_string";
        case JsonStatus::c1_control_code_in_string:
            return "c1_control_code_in_string";
        case JsonStatus::non_del_c0_control_code_in_string:
            return "non_del_c0_control_code_in_string";
        case JsonStatus::internal_error_unreachable_code:
            return "internal_error_unreachable_code";
        default:
            abort();
    }
}

} // namespace jt
