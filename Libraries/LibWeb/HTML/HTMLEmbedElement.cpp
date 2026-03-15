/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/HTMLEmbedElementPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CascadedProperties.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/StyleValues/DisplayStyleValue.h>
#include <LibWeb/CSS/StyleValues/KeywordStyleValue.h>
#include <LibWeb/HTML/HTMLEmbedElement.h>
#include <LibWeb/HTML/Parser/HTMLParser.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(HTMLEmbedElement);

HTMLEmbedElement::HTMLEmbedElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : HTMLElement(document, move(qualified_name))
{
}

HTMLEmbedElement::~HTMLEmbedElement() = default;

void HTMLEmbedElement::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(HTMLEmbedElement);
    Base::initialize(realm);
}

bool HTMLEmbedElement::is_presentational_hint(FlyString const& name) const
{
    if (Base::is_presentational_hint(name))
        return true;

    return first_is_one_of(name,
        HTML::AttributeNames::align,
        HTML::AttributeNames::height,
        HTML::AttributeNames::hspace,
        HTML::AttributeNames::vspace,
        HTML::AttributeNames::width);
}

void HTMLEmbedElement::apply_presentational_hints(GC::Ref<CSS::CascadedProperties> cascaded_properties) const
{
    Base::apply_presentational_hints(cascaded_properties);
    for_each_attribute([&](auto& name, auto& value) {
        // https://html.spec.whatwg.org/multipage/rendering.html#attributes-for-embedded-content-and-images
        if (name == HTML::AttributeNames::align) {
            auto vertical_align_keyword = [&]() -> Optional<CSS::Keyword> {
                if (value.equals_ignoring_ascii_case("top"sv))
                    return CSS::Keyword::Top;
                if (value.equals_ignoring_ascii_case("texttop"sv))
                    return CSS::Keyword::TextTop;
                if (value.equals_ignoring_ascii_case("center"sv) || value.equals_ignoring_ascii_case("middle"sv)
                    || value.equals_ignoring_ascii_case("absmiddle"sv) || value.equals_ignoring_ascii_case("abscenter"sv))
                    return CSS::Keyword::Middle;
                if (value.equals_ignoring_ascii_case("baseline"sv))
                    return CSS::Keyword::Baseline;
                if (value.equals_ignoring_ascii_case("bottom"sv) || (value.equals_ignoring_ascii_case("absbottom"sv)))
                    return CSS::Keyword::Bottom;
                return {};
            }();

            if (vertical_align_keyword.has_value()) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::VerticalAlign, CSS::KeywordStyleValue::create(vertical_align_keyword.value()));
            } else if (value.equals_ignoring_ascii_case("left"sv)) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::Float, CSS::KeywordStyleValue::create(CSS::Keyword::Left));
            } else if (value.equals_ignoring_ascii_case("right"sv)) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::Float, CSS::KeywordStyleValue::create(CSS::Keyword::Right));
            }
        } else if (name == HTML::AttributeNames::height) {
            if (auto parsed_value = parse_dimension_value(value))
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::Height, *parsed_value);
        }
        // https://html.spec.whatwg.org/multipage/rendering.html#attributes-for-embedded-content-and-images:maps-to-the-dimension-property
        else if (name == HTML::AttributeNames::hspace) {
            if (auto parsed_value = parse_dimension_value(value)) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::MarginLeft, *parsed_value);
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::MarginRight, *parsed_value);
            }
        } else if (name == HTML::AttributeNames::vspace) {
            if (auto parsed_value = parse_dimension_value(value)) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::MarginTop, *parsed_value);
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::MarginBottom, *parsed_value);
            }
        } else if (name == HTML::AttributeNames::width) {
            if (auto parsed_value = parse_dimension_value(value)) {
                cascaded_properties->set_property_from_presentational_hint(CSS::PropertyID::Width, *parsed_value);
            }
        }
    });
}

void HTMLEmbedElement::adjust_computed_style(CSS::ComputedProperties& style)
{
    // https://drafts.csswg.org/css-display-3/#unbox
    if (style.display().is_contents())
        style.set_property(CSS::PropertyID::Display, CSS::DisplayStyleValue::create(CSS::Display::from_short(CSS::Display::Short::None)));
}

}
