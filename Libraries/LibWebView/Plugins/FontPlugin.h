/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/RefPtr.h>
#include <AK/Traits.h>
#include <AK/Vector.h>
#include <LibGfx/Font/FontDatabase.h>
#include <LibWeb/Platform/FontPlugin.h>
#include <LibWebView/Forward.h>

namespace WebView {

struct GenericFontFallbackCacheKey {
    Web::Platform::GenericFont generic_font;
    Optional<String> locale;
    bool operator==(GenericFontFallbackCacheKey const&) const = default;
};

}

template<>
struct AK::Traits<WebView::GenericFontFallbackCacheKey> : public AK::DefaultTraits<WebView::GenericFontFallbackCacheKey> {
    static unsigned hash(WebView::GenericFontFallbackCacheKey const& key)
    {
        return pair_int_hash(to_underlying(key.generic_font), key.locale.has_value() ? key.locale->hash() : 0);
    }
};

namespace WebView {

class WEBVIEW_API FontPlugin final : public Web::Platform::FontPlugin {
public:
    FontPlugin(bool is_layout_test_mode, Gfx::SystemFontProvider* = nullptr);
    virtual ~FontPlugin();

    virtual RefPtr<Gfx::Font> default_font(float point_size) override;
    virtual Gfx::Font& default_fixed_width_font() override;
    virtual FlyString generic_font_name(Web::Platform::GenericFont) override;
    virtual Vector<FlyString> generic_font_fallback_list(Web::Platform::GenericFont, Optional<String> const& locale) override;
    virtual Vector<FlyString> symbol_font_names() override;

    void update_generic_fonts();

private:
    Vector<FlyString> m_generic_font_names;
    Vector<FlyString> m_symbol_font_names;
    FlyString m_default_font_name;
    RefPtr<Gfx::Font> m_default_fixed_width_font;
    mutable HashMap<GenericFontFallbackCacheKey, Vector<FlyString>> m_generic_font_fallback_cache;
    bool m_is_layout_test_mode { false };
};

}
