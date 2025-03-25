/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonParser.h>

namespace AK {

ErrorOr<JsonValue> JsonParser::parse(StringView input)
{
    JsonParser parser(input);
    return parser.parse_json();
}

ErrorOr<JsonValue> JsonParser::parse_element(simdjson::dom::element const element)
{
    JsonValue value;
    switch (element.type()) {
    case simdjson::dom::element_type::NULL_VALUE:
        value = JsonValue {};
        break;
    case simdjson::dom::element_type::BOOL: {
        bool result;
        if (element.get_bool().get(result))
            return Error::from_string_literal("Error parsing JSON bool value");
        value = JsonValue { result };
        break;
    }
    case simdjson::dom::element_type::INT64: {
        i64 result;
        if (element.get_int64().get(result))
            return Error::from_string_literal("Error parsing JSON integer value");
        value = JsonValue { result };
        break;
    }
    case simdjson::dom::element_type::UINT64: {
        u64 result;
        if (element.get_uint64().get(result))
            return Error::from_string_literal("Error parsing JSON unsigned integer value");
        value = JsonValue { element.get_uint64().value_unsafe() };
        break;
    }
    case simdjson::dom::element_type::DOUBLE: {
        double result;
        if (element.get_double().get(result))
            return Error::from_string_literal("Error parsing JSON double value");

        value = JsonValue { result };
        break;
    }
    case simdjson::dom::element_type::STRING: {
        std::string_view string_view;
        if (element.get_string().get(string_view))
            return Error::from_string_literal("Error parsing JSON string value");
        StringView ak_string_view { string_view.data(), string_view.size() };
        auto string = String::from_utf8_without_validation(ak_string_view.bytes());
        value = JsonValue { move(string) };
        break;
    }
    case simdjson::dom::element_type::ARRAY: {
        JsonArray result_array;
        simdjson::dom::array array;
        if (element.get_array().get(array))
            return Error::from_string_literal("Error parsing JSON array");
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
        if (element.get_object().get(object))
            return Error::from_string_literal("Error parsing JSON object");

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

ErrorOr<JsonValue> JsonParser::parse_json()
{
    auto bytes = m_input.bytes();
    if ((bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
        || (bytes.size() >= 2 && ((bytes[0] == 0xFF && bytes[1] == 0xFE) || (bytes[0] == 0xFE && bytes[1] == 0xFF)))) {
        return Error::from_string_literal("Encountered BOM while parsing JSON");
    }

    simdjson::dom::parser parser;
    simdjson::padded_string padded_string(m_input.characters_without_null_termination(), m_input.length());
    simdjson::dom::element element;

    if (parser.parse(padded_string).get(element))
        return Error::from_string_literal("Error parsing JSON root element");

    return parse_element(element);
}

}
