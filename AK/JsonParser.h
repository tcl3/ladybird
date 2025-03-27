/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/JsonValue.h>
#include <simdjson.h>

namespace AK {

class JsonParser {
public:
    static ErrorOr<JsonValue> parse(StringView);

private:
    explicit JsonParser(StringView input)
        : m_input(input)
    {
    }

    ErrorOr<JsonValue, simdjson::error_code> parse_json();
    ErrorOr<JsonValue, simdjson::error_code> parse_value(simdjson::ondemand::value value);
    ErrorOr<JsonValue, simdjson::error_code> parse_scalar(auto value_or_document);

    StringView m_input;
};

}

#if USING_AK_GLOBALLY
using AK::JsonParser;
#endif
