/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibURL/Parser.h>
#include <LibURL/URL.h>
#include <LibWeb/Loader/ContentFilter.h>

namespace Web {

static ContentFilter& make_filter(Vector<String> patterns)
{
    auto& filter = ContentFilter::the();
    MUST(filter.set_patterns(patterns));
    return filter;
}

static URL::URL url(StringView string)
{
    auto result = URL::Parser::basic_parse(string);
    EXPECT(result.has_value());
    return result.release_value();
}

TEST_CASE(empty_pattern_list)
{
    auto& filter = make_filter({});

    EXPECT(!filter.is_filtered(url("https://anything.com"sv)));
    EXPECT(!filter.is_filtered(url("data:text/plain,hi"sv)));
}

TEST_CASE(basic_blocking)
{
    Vector<String> patterns = {
        "ads."_string,
        "?banner"_string,
        "tracker"_string
    };

    auto& filter = make_filter(move(patterns));

    EXPECT(filter.is_filtered(url("https://example.com/ads.js"sv)));
    EXPECT(filter.is_filtered(url("http://site.com/page.html?banner=true"sv)));
    EXPECT(filter.is_filtered(url("https://tracker.example.org/ping"sv)));
    EXPECT(!filter.is_filtered(url("https://example.com/page.html"sv)));
}

TEST_CASE(data_urls_exempt)
{
    Vector<String> patterns = {
        { "data:"_string },
        { "evil.com"_string }
    };

    auto& filter = make_filter(move(patterns));

    EXPECT(!filter.is_filtered(url("data:text/plain,hello"sv)));
    EXPECT(!filter.is_filtered(url("data:image/png;base64,abc123"sv)));
    EXPECT(filter.is_filtered(url("http://evil.com/script.js"sv)));
}

TEST_CASE(disable_filtering)
{
    Vector<String> patterns = {
        { "example.com"_string }
    };

    auto& filter = make_filter(move(patterns));
    filter.set_filtering_enabled(false);

    EXPECT(!filter.is_filtered(url("https://example.com"sv)));
    EXPECT(!filter.is_filtered(url("http://example.com/ads"sv)));

    filter.set_filtering_enabled(true);
    EXPECT(filter.is_filtered(url("https://example.com"sv)));
}

// TEST_CASE(case_insensitive_url_matching)
// {
//     Vector<String> patterns = {
//         { "ADS."_string },
//         { "Tracker"_string },
//         { "banner.gif"_string }
//     };
//
//     auto& filter = make_filter(move(patterns));
//
//     EXPECT(filter.is_filtered(url("https://site.com/ads.js"sv)));
//     EXPECT(filter.is_filtered(url("http://tracker.example.com/"sv)));
//     EXPECT(filter.is_filtered(url("https://example.com/BANNER.GIF"sv)));
// }

TEST_CASE(substring_matches)
{
    Vector<String> patterns = {
        { "googleadservices"_string },
        { "doubleclick"_string }
    };

    auto& filter = make_filter(move(patterns));

    EXPECT(filter.is_filtered(url("https://www.googleadservices.com/pagead/conversion.js"sv)));
    EXPECT(filter.is_filtered(url("https://doubleclick.net/tag.js"sv)));
    EXPECT(!filter.is_filtered(url("https://google.com/search"sv)));
}

TEST_CASE(file_scheme_can_be_filtered)
{
    Vector<String> patterns = {
        { "secret"_string },
        { ".txt"_string }
    };

    auto& filter = make_filter(move(patterns));

    EXPECT(filter.is_filtered(url("file:///home/user/secret.txt"sv)));
    EXPECT(!filter.is_filtered(url("file:///home/user/document.pdf"sv)));
}

TEST_CASE(query_parameters_and_fragments)
{
    Vector<String> patterns = {
        { "#ad="_string },
        { "?ad="_string },
        { "#sponsored"_string }
    };

    auto& filter = make_filter(move(patterns));

    EXPECT(filter.is_filtered(url("https://site.com/page?ad=123"sv)));
    EXPECT(filter.is_filtered(url("https://site.com/page#ad=456"sv)));
    EXPECT(filter.is_filtered(url("https://site.com/page?ref=home&ad=1#sponsored"sv)));
    EXPECT(!filter.is_filtered(url("https://site.com/page?ref=home"sv)));
}

}
