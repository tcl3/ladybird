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

    ErrorOr<JsonValue> parse_json();
    ErrorOr<JsonValue> parse_element(simdjson::dom::element element);

    StringView m_input;
};

}

#if USING_AK_GLOBALLY
using AK::JsonParser;
#endif
