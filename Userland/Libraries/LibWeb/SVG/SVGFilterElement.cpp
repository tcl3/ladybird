/*
 * Copyright (c) 2024, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/SVGFilterElementPrototype.h>
#include <LibWeb/SVG/AttributeNames.h>
#include <LibWeb/SVG/AttributeParser.h>
#include <LibWeb/SVG/SVGFilterElement.h>

namespace Web::SVG {

JS_DEFINE_ALLOCATOR(SVGFilterElement);

SVGFilterElement::SVGFilterElement(DOM::Document& document, DOM::QualifiedName qualified_name)
    : SVGElement(document, qualified_name)
{
}

void SVGFilterElement::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(SVGFilterElement);
}

void SVGFilterElement::attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value)
{
    SVGElement::attribute_changed(name, old_value, value);
}

JS::NonnullGCPtr<SVGAnimatedLength> SVGFilterElement::x() const
{
    return svg_animated_length_for_property(CSS::PropertyID::X);
}

JS::NonnullGCPtr<SVGAnimatedLength> SVGFilterElement::y() const
{
    return svg_animated_length_for_property(CSS::PropertyID::Y);
}

JS::NonnullGCPtr<SVGAnimatedLength> SVGFilterElement::width() const
{
    return svg_animated_length_for_property(CSS::PropertyID::Width);
}

JS::NonnullGCPtr<SVGAnimatedLength> SVGFilterElement::height() const
{
    return svg_animated_length_for_property(CSS::PropertyID::Height);
}

}
