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
            dbgln("Falling back to legacy json parser. Error code: {}", (int)error_code);
            return FallbackJsonParser::parse(input);
        }

        return Error::from_string_literal("Unable to parse JSON");
    }

    return result_or_error.release_value();
}

ErrorOr<JsonValue, simdjson::error_code> JsonParser::parse_value(simdjson::ondemand::value value)
{
    simdjson::ondemand::json_type type;
    if (auto error = value.type().get(type))
        return error;

    switch (type) {
    case simdjson::ondemand::json_type::array: {
        JsonArray result_array;
        simdjson::ondemand::array array;
        if (auto error = value.get_array().get(array))
            return error;

        for (auto child_or_error : array) {
            simdjson::ondemand::value child;
            if (auto error = child_or_error.get(child))
                return error;
            result_array.must_append(TRY(parse_value(child)));
        }
        return JsonValue { move(result_array) };
    }
    case simdjson::ondemand::json_type::object: {
        JsonObject result_object;
        simdjson::ondemand::object object;
        if (auto error = value.get_object().get(object))
            return error;

        for (auto field_or_error : object) {
            std::string_view key_view;
            if (auto error = field_or_error.unescaped_key(key_view))
                return error;
            StringView ak_key_view { key_view.data(), key_view.size() };
            auto key = String::from_utf8_without_validation(ak_key_view.bytes());

            simdjson::ondemand::value value;
            if (auto error = field_or_error.value().get(value))
                return error;
            auto object_value = TRY(parse_value(value));
            result_object.set(move(key), move(object_value));
        }
        return JsonValue { move(result_object) };
    }
    default:
        return parse_scalar(value);
    }

    VERIFY_NOT_REACHED();
}

ErrorOr<JsonValue, simdjson::error_code> JsonParser::parse_scalar(auto value_or_document)
{
    simdjson::ondemand::json_type type;
    if (auto error = value_or_document.type().get(type))
        return error;

    switch (type) {
    case simdjson::ondemand::json_type::null:
        return JsonValue {};
    case simdjson::ondemand::json_type::boolean: {
        bool result;
        if (auto error = value_or_document.get_bool().get(result))
            return error;
        return JsonValue { result };
    }
    case simdjson::ondemand::json_type::number: {
        simdjson::ondemand::number number;
        if (auto error = value_or_document.get_number().get(number))
            return error;

        switch (number.get_number_type()) {
        case simdjson::ondemand::number_type::signed_integer: {
            i64 result;
            if (auto error = value_or_document.get_int64().get(result))
                return error;
            if (result == 0 && value_or_document.is_negative())
                return JsonValue { -0.0 };

            return JsonValue { result };
        }
        case simdjson::ondemand::number_type::unsigned_integer: {
            u64 result;
            if (auto error = value_or_document.get_uint64().get(result))
                return error;
            return JsonValue { result };
        }
        case simdjson::ondemand::number_type::floating_point_number: {
            double result;
            if (auto error = value_or_document.get_double().get(result))
                return error;

            return JsonValue { result };
        }
        case simdjson::arm64::number_type::big_integer:
            break;
        }
        break;
    }
    case simdjson::ondemand::json_type::string: {
        std::string_view string_view;
        if (auto error = value_or_document.get_wobbly_string().get(string_view)) {
            dbgln("Failed to get wobbly string: {}", (int)error);
            return error;
        }
        StringView ak_string_view { string_view.data(), string_view.size() };
        auto string = String::from_utf8_without_validation(ak_string_view.bytes());
        return JsonValue { move(string) };
    }
    default:
        break;
    }

    VERIFY_NOT_REACHED();
}

ErrorOr<JsonValue, simdjson::error_code> JsonParser::parse_json()
{
    auto bytes = m_input.bytes();
    if ((bytes.size() >= 3 && bytes[0] == 0xEF && bytes[1] == 0xBB && bytes[2] == 0xBF)
        || (bytes.size() >= 2 && ((bytes[0] == 0xFF && bytes[1] == 0xFE) || (bytes[0] == 0xFE && bytes[1] == 0xFF)))) {
        // Correct error code?
        return simdjson::error_code::STRING_ERROR;
    }

    simdjson::ondemand::parser parser;
    simdjson::padded_string padded_string(m_input.characters_without_null_termination(), m_input.length());
    simdjson::ondemand::document document;

    // iterate_raw also fails with unpaired surrogates :(
    if (auto error = parser.iterate(padded_string).get(document))
        return error;

    bool is_scalar;
    if (auto error = document.is_scalar().get(is_scalar))
        return error;

    if (is_scalar)
        return parse_scalar(move(document));

    simdjson::ondemand::value root;
    if (auto error = document.get_value().get(root))
        return error;

    auto result = TRY(parse_value(root));
    if (!document.at_end())
        return simdjson::error_code::TRAILING_CONTENT;

    return result;
}

}
