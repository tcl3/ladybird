/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/QuickSort.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/PaintableWithLines.h>
#include <LibWeb/Painting/TextPaintable.h>
#include <LibWeb/Painting/VisualLineIterator.h>

namespace Web::Painting {

// Walk up the paint tree from a text node's paintable to find the outermost block-level
// PaintableWithLines that contains it within the editing boundary. For contenteditable elements
// with block children (e.g. <div contenteditable><h1>...</h1> text</div>), this returns the
// contenteditable's PaintableWithLines so the subtree walk picks up fragments from all child blocks.
// For non-editable content, returns the nearest block-level PaintableWithLines.
static PaintableWithLines const* containing_block_paintable_with_lines(DOM::Text const& dom_node)
{
    auto const* layout_node = dom_node.unsafe_layout_node();
    if (!layout_node)
        return nullptr;

    auto const* paintable = layout_node->first_paintable();
    if (!paintable)
        return nullptr;

    auto editing_host = const_cast<DOM::Text&>(dom_node).editing_host();

    PaintableWithLines const* result = nullptr;
    for (auto const* ancestor = paintable; ancestor; ancestor = ancestor->parent()) {
        if (auto const* paintable_with_lines = as_if<PaintableWithLines>(*ancestor)) {
            if (paintable_with_lines->layout_node().is_block_container()) {
                result = paintable_with_lines;

                // If not inside a contenteditable, the nearest block is sufficient.
                if (!editing_host)
                    return result;

                // If this block is the editing host itself, stop here.
                if (paintable_with_lines->layout_node().dom_node() == editing_host.ptr())
                    return result;
            }
        }
    }

    return result;
}

static bool is_fragment_within_boundary(PaintableFragment const& fragment, DOM::Node const* boundary)
{
    if (!boundary)
        return true;
    auto const* dom_node = fragment.layout_node().dom_node();
    return dom_node && boundary->is_inclusive_ancestor_of(*dom_node);
}

Optional<VisualLineIterator> VisualLineIterator::create(DOM::Text const& dom_node, size_t offset)
{
    auto const* layout_node = dom_node.unsafe_layout_node();
    if (!layout_node)
        return {};

    auto const* block_paintable = containing_block_paintable_with_lines(dom_node);
    if (!block_paintable)
        return {};

    // Scope fragment collection to the contenteditable boundary (if any) to avoid picking up
    // fragments from sibling contenteditable elements sharing the same block formatting context.
    auto boundary = const_cast<DOM::Text&>(dom_node).editing_host();

    // Collect all text fragment pointers from the block's paint subtree. For each fragment, record
    // which block container it belongs to so we can construct a correct visual line key: fragments
    // share a visual line only if they are in the same block AND have the same line_index.
    struct FragmentEntry {
        PaintableFragment const* fragment;
        PaintableWithLines const* owning_block;
    };
    Vector<FragmentEntry> entries;

    block_paintable->for_each_in_inclusive_subtree_of_type<PaintableWithLines>([&](auto const& paintable_with_lines) {
        // Determine the innermost block container for fragments in this PaintableWithLines.
        PaintableWithLines const* current_block = &paintable_with_lines;
        if (!current_block->layout_node().is_block_container()) {
            for (auto const* ancestor = current_block->parent(); ancestor; ancestor = ancestor->parent()) {
                if (auto const* pwl = as_if<PaintableWithLines>(*ancestor); pwl && pwl->layout_node().is_block_container()) {
                    current_block = pwl;
                    break;
                }
            }
        }

        for (auto const& fragment : paintable_with_lines.fragments()) {
            if (!is<TextPaintable>(fragment.paintable()))
                continue;
            if (!is_fragment_within_boundary(fragment, boundary))
                continue;
            entries.append({ &fragment, current_block });
        }
        return TraversalDecision::Continue;
    });

    if (entries.is_empty())
        return {};

    // Sort by (block y-position, line_index, x-position). Fragments from different blocks are
    // ordered by their block's vertical position. Within the same block, line_index correctly
    // groups fragments on the same visual line regardless of baseline differences.
    quick_sort(entries, [](auto const& a, auto const& b) {
        if (a.owning_block != b.owning_block)
            return a.owning_block->absolute_rect().y() < b.owning_block->absolute_rect().y();
        if (a.fragment->line_index() != b.fragment->line_index())
            return a.fragment->line_index() < b.fragment->line_index();
        return a.fragment->absolute_rect().x() < b.fragment->absolute_rect().x();
    });

    // Build the sorted fragment pointer list and line boundary index.
    Vector<PaintableFragment const*> fragments;
    fragments.ensure_capacity(entries.size());
    for (auto const& entry : entries)
        fragments.append(entry.fragment);

    // A new visual line starts when either the owning block or the line_index changes.
    Vector<size_t> line_boundaries;
    line_boundaries.append(0);

    for (size_t i = 1; i < entries.size(); ++i) {
        if (entries[i].owning_block != entries[i - 1].owning_block
            || entries[i].fragment->line_index() != entries[i - 1].fragment->line_index()) {
            line_boundaries.append(i);
        }
    }
    // Sentinel: one past the last fragment.
    line_boundaries.append(fragments.size());

    // Find the fragment containing the given offset for this layout node.
    PaintableFragment const* origin_fragment = nullptr;
    for (auto const* fragment : fragments) {
        if (&fragment->layout_node() != layout_node)
            continue;
        if (offset >= fragment->start_offset() && offset <= fragment->start_offset() + fragment->length_in_code_units()) {
            origin_fragment = fragment;
            break;
        }
    }

    if (!origin_fragment)
        return {};

    // Determine which line the origin fragment is on by finding it in the sorted fragment list.
    size_t origin_line = 0;
    for (size_t i = 0; i + 1 < line_boundaries.size(); ++i) {
        auto line_start = line_boundaries[i];
        auto line_end = line_boundaries[i + 1];
        bool found = false;
        for (size_t j = line_start; j < line_end; ++j) {
            if (fragments[j] == origin_fragment) {
                found = true;
                break;
            }
        }
        if (found) {
            origin_line = i;
            break;
        }
    }

    return VisualLineIterator { move(fragments), move(line_boundaries), origin_line, origin_fragment, offset };
}

VisualLineIterator::VisualLineIterator(Vector<PaintableFragment const*> fragments, Vector<size_t> line_boundaries, size_t current_line, PaintableFragment const* origin_fragment, size_t origin_offset)
    : m_fragments(move(fragments))
    , m_line_boundaries(move(line_boundaries))
    , m_current_line(current_line)
    , m_origin_fragment(origin_fragment)
    , m_origin_offset(origin_offset)
{
}

bool VisualLineIterator::next_line()
{
    if (m_current_line + 1 >= line_count())
        return false;
    ++m_current_line;
    return true;
}

bool VisualLineIterator::previous_line()
{
    if (m_current_line == 0)
        return false;
    --m_current_line;
    return true;
}

CSSPixels VisualLineIterator::cursor_x() const
{
    auto x = m_origin_fragment->absolute_rect().x();
    if (!m_origin_fragment->glyph_run())
        return x;

    size_t code_units_seen = 0;
    auto offset_within_fragment = m_origin_offset - m_origin_fragment->start_offset();

    for (auto const& glyph : m_origin_fragment->glyph_run()->glyphs()) {
        if (code_units_seen >= offset_within_fragment)
            break;
        x += CSSPixels::nearest_value_for(glyph.glyph_width);
        code_units_seen += glyph.length_in_code_units;
    }

    return x;
}

Optional<VisualLinePosition> VisualLineIterator::position_for_x(CSSPixels x) const
{
    auto line_start = m_line_boundaries[m_current_line];
    auto line_end = m_line_boundaries[m_current_line + 1];

    if (line_start == line_end)
        return {};

    // Find the fragment on this line that contains the target x, or the last one if x is past all.
    PaintableFragment const* best_fragment = m_fragments[line_start];
    for (size_t i = line_start; i < line_end; ++i) {
        best_fragment = m_fragments[i];
        if (x <= m_fragments[i]->absolute_rect().right())
            break;
    }

    // Map the x position to an offset within the fragment.
    auto target_point = CSSPixelPoint { x, best_fragment->absolute_rect().y() };
    auto target_offset = best_fragment->index_in_node_for_point(target_point);

    // Resolve the DOM::Text node from this fragment's layout node.
    auto const* target_dom_node = best_fragment->layout_node().dom_node();
    if (!target_dom_node)
        return {};
    auto const* target_text = as_if<DOM::Text>(*target_dom_node);
    if (!target_text)
        return {};

    return VisualLinePosition { *target_text, target_offset };
}

size_t VisualLineIterator::line_start_for_node(Layout::Node const& layout_node) const
{
    auto line_start = m_line_boundaries[m_current_line];
    auto line_end = m_line_boundaries[m_current_line + 1];

    for (size_t i = line_start; i < line_end; ++i) {
        if (&m_fragments[i]->layout_node() == &layout_node)
            return m_fragments[i]->start_offset();
    }

    return 0;
}

size_t VisualLineIterator::line_end_for_node(Layout::Node const& layout_node) const
{
    auto line_start = m_line_boundaries[m_current_line];
    auto line_end = m_line_boundaries[m_current_line + 1];

    size_t end_offset = 0;
    for (size_t i = line_start; i < line_end; ++i) {
        if (&m_fragments[i]->layout_node() == &layout_node)
            end_offset = m_fragments[i]->start_offset() + m_fragments[i]->length_in_code_units();
    }

    return end_offset;
}

}
