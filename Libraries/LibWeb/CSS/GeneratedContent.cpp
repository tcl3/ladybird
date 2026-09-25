/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/GeneratedContent.h>
#include <LibWeb/CSS/StyleValues/ContentStyleValue.h>
#include <LibWeb/CSS/StyleValues/CounterStyleValue.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>

namespace Web::CSS {

static bool style_uses_quotes(ComputedValues const& style)
{
    auto content = style.computed_content();
    return content->is_content() && any_of(content->as_content().content().values(), [](auto const& item) {
        return item->is_keyword() && first_is_one_of(item->to_keyword(), Keyword::OpenQuote, Keyword::CloseQuote, Keyword::NoOpenQuote, Keyword::NoCloseQuote);
    });
}

static bool style_uses_counters(ComputedValues const& style)
{
    if (!style.counter_increment().is_empty() || !style.counter_reset().is_empty() || !style.counter_set().is_empty())
        return true;
    auto content = style.computed_content();
    return content->is_content() && any_of(content->as_content().content().values(), [](auto const& item) {
        return item->is_counter();
    });
}

bool subtree_affects_generated_content_outside_itself(DOM::Node const& node)
{
    // https://drafts.csswg.org/css-lists-3/#nested-counters
    // "The scope of a counter therefore starts at the first element in the document that instantiates that counter
    // and includes the element's descendants and its following siblings with their descendants."
    // NB: A counter that the subtree's root resets is only visible outside the subtree to the root's following
    //     siblings and its parent's ::after. Counters reset below the root stay inside the subtree.
    auto const* root = as_if<DOM::Element>(node);
    Vector<CounterData, 0> counters_reset_by_root;
    if (root) {
        if (auto root_style = root->computed_style())
            counters_reset_by_root = root_style->counter_reset();
    }
    auto modifies_counter_from_outside = [&](Vector<CounterData, 0> const& counters) {
        return any_of(counters, [&](CounterData const& counter) {
            return !any_of(counters_reset_by_root, [&](CounterData const& reset) { return reset.name == counter.name; });
        });
    };
    auto style_affects_state_outside = [&](auto const& style) {
        return style
            && (style_uses_quotes(*style)
                || modifies_counter_from_outside(style->counter_increment())
                || modifies_counter_from_outside(style->counter_set()));
    };

    bool affects_generated_content_outside = false;
    node.for_each_in_inclusive_subtree([&](DOM::Node const& descendant) {
        auto const* element = as_if<DOM::Element>(descendant);
        if (!element)
            return TraversalDecision::Continue;

        if (!style_affects_state_outside(element->computed_style())
            && !style_affects_state_outside(element->computed_style(PseudoElement::Before))
            && !style_affects_state_outside(element->computed_style(PseudoElement::After))
            && !style_affects_state_outside(element->computed_style(PseudoElement::Marker))) {
            return TraversalDecision::Continue;
        }

        affects_generated_content_outside = true;
        return TraversalDecision::Break;
    });
    if (affects_generated_content_outside || counters_reset_by_root.is_empty())
        return affects_generated_content_outside;

    // NB: Following siblings keep the counters they inherited through the root until they are rebuilt, so any
    //     following sibling element counts as observing the root's counters.
    if (root->next_element_sibling())
        return true;
    auto parent = root->parent_element();
    if (!parent)
        return false;
    auto parent_after_style = parent->computed_style(PseudoElement::After);
    return parent_after_style && style_uses_counters(*parent_after_style);
}

}
