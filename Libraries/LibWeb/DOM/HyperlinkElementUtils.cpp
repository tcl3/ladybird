/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2025, Shannon Booth <shannon@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibURL/Parser.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/HyperlinkElementUtils.h>
#include <LibWeb/HTML/AttributeNames.h>
#include <LibWeb/HTML/Navigable.h>

namespace Web::DOM {

HyperlinkElementUtils::~HyperlinkElementUtils() = default;

// https://html.spec.whatwg.org/multipage/links.html#following-hyperlinks-2
void HyperlinkElementUtils::follow_the_hyperlink(Optional<String> hyperlink_suffix, HTML::UserNavigationInvolvement user_involvement)
{
    auto& element = hyperlink_element_utils_element();

    // 1. If subject cannot navigate, then return.
    if (element.cannot_navigate())
        return;

    // 2. Let targetAttributeValue be the empty string.
    String target_attribute_value;

    // 3. If subject is an a or area element, then set targetAttributeValue to the result of getting an element's target given subject.
    if (element.is_html_anchor_element() || element.is_html_area_element() || element.is_svg_a_element())
        target_attribute_value = element.get_an_elements_target();

    // 4. Let urlRecord be the result of encoding-parsing a URL given subject's href attribute value, relative to subject's node document.
    auto url_record = element.document().encoding_parse_url(hyperlink_element_utils_href());

    // 5. If urlRecord is failure, then return.
    if (!url_record.has_value())
        return;

    // 6. Let noopener be the result of getting an element's noopener with subject, urlRecord, and targetAttributeValue.
    auto noopener = element.get_an_elements_noopener(*url_record, target_attribute_value);

    // 7. Let targetNavigable be the first return value of applying the rules for choosing a navigable given
    //    targetAttributeValue, subject's node navigable, and noopener.
    auto target_navigable = element.document().navigable()->choose_a_navigable(target_attribute_value, noopener).navigable;

    // 8. If targetNavigable is null, then return.
    if (!target_navigable)
        return;

    // 9. Let urlString be the result of applying the URL serializer to urlRecord.
    auto url_string = url_record->serialize();

    // 10. If hyperlinkSuffix is non-null, then append it to urlString.
    if (hyperlink_suffix.has_value())
        url_string = MUST(String::formatted("{}{}", url_string, *hyperlink_suffix));

    // 11. Let referrerPolicy be the current state of subject's referrerpolicy content attribute.
    auto referrer_policy = ReferrerPolicy::from_string(element.attribute(HTML::AttributeNames::referrerpolicy).value_or({})).value_or(ReferrerPolicy::ReferrerPolicy::EmptyString);

    // FIXME: 12. If subject's link types includes the noreferrer keyword, then set referrerPolicy to "no-referrer".

    // 13. Navigate targetNavigable to urlString using subject's node document, with referrerPolicy set to referrerPolicy and userInvolvement set to userInvolvement.
    auto url = URL::Parser::basic_parse(url_string);
    VERIFY(url.has_value());
    MUST(target_navigable->navigate({ .url = url.release_value(), .source_document = element.document(), .referrer_policy = referrer_policy, .user_involvement = user_involvement }));
}

}
