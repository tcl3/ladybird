/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/FallbackJsonParser.h>
#include <AK/GenericShorthands.h>
#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonParser.h>

namespace AK {

ErrorOr<JsonValue> JsonParser::parse(StringView input)
{
    JsonParser parser(input);
    auto result_or_error = parser.parse_json();
    if (result_or_error.is_error()) {
        auto error_code = result_or_error.error();
        if (first_is_one_of(error_code, simdjson::error_code::STRING_ERROR, simdjson::error_code::UTF8_ERROR)) {
            return FallbackJsonParser::parse(input);
        }

        return Error::from_string_literal("Unable to parse JSON");
    }

    return result_or_error.release_value();
}

ErrorOr<JsonValue, simdjson::error_code> JsonParser::parse_element(simdjson::dom::element const element)
{
    JsonValue value;
    switch (element.type()) {
    case simdjson::dom::element_type::NULL_VALUE:
        value = JsonValue {};
        break;
    case simdjson::dom::element_type::BOOL: {
        bool result;
        if (auto error = element.get_bool().get(result))
            return error;
        value = JsonValue { result };
        break;
    }
    case simdjson::dom::element_type::INT64: {
        i64 result;
        if (auto error = element.get_int64().get(result))
            return error;
        value = JsonValue { result };
        break;
    }
    case simdjson::dom::element_type::UINT64: {
        u64 result;
        if (auto error = element.get_uint64().get(result))
            return error;
        value = JsonValue { element.get_uint64().value_unsafe() };
        break;
    }
    case simdjson::dom::element_type::DOUBLE: {
        double result;
        if (auto error = element.get_double().get(result))
            return error;

        value = JsonValue { result };
        break;
    }
    case simdjson::dom::element_type::STRING: {
        std::string_view string_view;

        if (auto error = element.get_string().get(string_view))
            return error;
        StringView ak_string_view { string_view.data(), string_view.size() };
        auto string = String::from_utf8_without_validation(ak_string_view.bytes());
        value = JsonValue { move(string) };
        break;
    }
    case simdjson::dom::element_type::ARRAY: {
        JsonArray result_array;
        simdjson::dom::array array;
        if (auto error = element.get_array().get(array))
            return error;
        result_array.ensure_capacity(array.size());
        for (auto child : array) {
            result_array.must_append(TRY(parse_element(child)));
        }
        value = JsonValue { move(result_array) };
        break;
    }
    case simdjson::dom::element_type::OBJECT: {
        JsonObject result_object;
        simdjson::dom::object object;
        if (auto error = element.get_object().get(object))
            return error;

        result_object.ensure_capacity(object.size());
        for (auto field : object) {
            StringView view { field.key.data(), field.key.size() };
            auto key = String::from_utf8_without_validation(view.bytes());
            auto object_value = TRY(parse_element(field.value));
            result_object.set(move(key), move(object_value));
        }
        value = JsonValue { move(result_object) };
        break;
    }
    }

    return value;
}

ErrorOr<JsonValue, simdjson::error_code> JsonParser::parse_json()
{
    auto bytes = m_input.bytes();
    if ((bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
        || (bytes.size() >= 2 && ((bytes[0] == 0xFF && bytes[1] == 0xFE) || (bytes[0] == 0xFE && bytes[1] == 0xFF)))) {
        // Correct error code?
        return simdjson::error_code::STRING_ERROR;
    }

    simdjson::dom::parser parser;
    simdjson::padded_string padded_string(m_input.characters_without_null_termination(), m_input.length());
    simdjson::dom::element element;

    if (auto error = parser.parse(padded_string).get(element))
        return error;

    return parse_element(element);
}

}
