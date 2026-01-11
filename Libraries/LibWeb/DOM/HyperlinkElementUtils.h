/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibURL/URL.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/UserNavigationInvolvement.h>

namespace Web::DOM {

class HyperlinkElementUtils {
public:
    virtual ~HyperlinkElementUtils();

protected:
    virtual DOM::Element& hyperlink_element_utils_element() = 0;
    virtual DOM::Element const& hyperlink_element_utils_element() const = 0;
    virtual String hyperlink_element_utils_href() const = 0;

    void follow_the_hyperlink(Optional<String> hyperlink_suffix, HTML::UserNavigationInvolvement = HTML::UserNavigationInvolvement::None);
};

}
