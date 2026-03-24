/*
 * Copyright (c) 2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/PaintableFragment.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

// A position within a visual line: a text node and code unit offset.
struct VisualLinePosition {
    DOM::Text const& text_node;
    size_t offset;
};

// A lightweight iterator over visual lines within a block formatting context.
//
// On construction, collects all text fragment pointers from the block's paint subtree into a flat,
// line-grouped index. Subsequent queries (next/previous line, hit-test, cursor position) are simple
// index lookups with no further tree walks.
//
// Analogous to Blink's AbstractLineBox: CreateFor() + NextLine()/PreviousLine() + PositionForPoint().
class VisualLineIterator {
public:
    // Build from a DOM text position. Walks from the text node's paintable up to the block-level
    // PaintableWithLines, collects all fragments, and groups them by line_index.
    // Returns nullopt if no layout or paint data is available.
    static Optional<VisualLineIterator> create(DOM::Text const&, size_t offset);

    // Current line index.
    size_t line_index() const { return m_current_line; }

    // Navigate to the next or previous visual line. Returns false if no such line exists.
    bool next_line();
    bool previous_line();

    // The fragment containing the position this iterator was created from.
    PaintableFragment const& origin_fragment() const { return *m_origin_fragment; }

    // Absolute x-coordinate of the cursor at the origin offset within origin_fragment.
    CSSPixels cursor_x() const;

    // Find the text position on the current line closest to a given absolute x-coordinate.
    Optional<VisualLinePosition> position_for_x(CSSPixels x) const;

    // Line boundary offsets for a given layout node on the current line.
    size_t line_start_for_node(Layout::Node const&) const;
    size_t line_end_for_node(Layout::Node const&) const;

    // Number of lines in this block.
    size_t line_count() const { return m_line_boundaries.size() - 1; }

private:
    VisualLineIterator(Vector<PaintableFragment const*>, Vector<size_t>, size_t current_line, PaintableFragment const*, size_t origin_offset);

    // Flat list of all text fragment pointers from the block subtree,
    // filtered by editability boundary, sorted by (line_index, x-position).
    Vector<PaintableFragment const*> m_fragments;

    // m_line_boundaries[i] = index into m_fragments of the first fragment on line i.
    // Size is number_of_lines + 1 (sentinel at end for easy range computation).
    Vector<size_t> m_line_boundaries;

    size_t m_current_line;
    PaintableFragment const* m_origin_fragment;
    size_t m_origin_offset;
};

}
