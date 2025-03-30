/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/GenericLexer.h>
#include <AK/JsonValue.h>

namespace AK {
template<class TValue = JsonValue, class TObject = JsonObject, class TArray = JsonArray>
class JsonParser : private GenericLexer {
public:
    static ErrorOr<TValue> parse(StringView);

private:
    explicit JsonParser(StringView input)
        : GenericLexer(input)
    {
    }

    ErrorOr<TValue> parse_json();
    ErrorOr<TValue> parse_helper();

    ErrorOr<ByteString> consume_and_unescape_string();
    ErrorOr<TValue> parse_array();
    ErrorOr<TValue> parse_object();
    ErrorOr<TValue> parse_number();
    ErrorOr<TValue> parse_string();
    ErrorOr<TValue> parse_false();
    ErrorOr<TValue> parse_true();
    ErrorOr<TValue> parse_null();

    TValue construct_value();
    TValue construct_value(auto value);

    template<typename... Args>
    TObject construct_object(Args&&... args);

    template<typename... Args>
    TArray construct_array(Args&&... args);

    void set_array_index(TArray& array, size_t index, TValue value);
    void set_object_key(TObject& object, auto key, TValue value);
};

template<>
inline JsonValue JsonParser<>::construct_value() { return JsonValue {}; }

template<>
template<>
JsonValue JsonParser<>::construct_value(auto value) { return JsonValue(value); }

template<>
template<typename... Args>
JsonObject JsonParser<>::construct_object(Args&&...) { return JsonObject {}; }

template<>
template<typename... Args>
JsonArray JsonParser<>::construct_array(Args&&...) { return JsonArray {}; }

template<>
inline void JsonParser<>::set_array_index(JsonArray& array, size_t, JsonValue value) { array.must_append(move(value)); }

template<>
template<>
void JsonParser<>::set_object_key(JsonObject& object, auto key, JsonValue value) { object.set(key, move(value)); }

}

#if USING_AK_GLOBALLY
using AK::JsonParser;
#endif
