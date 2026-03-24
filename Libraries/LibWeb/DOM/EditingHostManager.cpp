/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2025-2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/EditingHostManager.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/CommandNames.h>
#include <LibWeb/GraphemeEdgeTracker.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Selection/Selection.h>
#include <LibWeb/UIEvents/InputTypes.h>

namespace Web::DOM {

static GC::Ptr<Text> next_text_node_within(Node& current, Node& boundary)
{
    for (auto* node = current.next_in_pre_order(&boundary); node; node = node->next_in_pre_order(&boundary)) {
        if (auto* text = as_if<Text>(node); text && text->length() > 0 && text->is_editable())
            return *text;
    }
    return {};
}

static GC::Ptr<Text> previous_text_node_within(Node& current, Node& boundary)
{
    for (auto* node = current.previous_in_pre_order(); node && node != &boundary; node = node->previous_in_pre_order()) {
        if (auto* text = as_if<Text>(node); text && text->length() > 0 && text->is_editable())
            return *text;
    }
    return {};
}

static bool is_line_break_node(Node& node)
{
    auto* layout_node = node.layout_node();
    if (!layout_node)
        return false;
    if (layout_node->is_break_node())
        return true;
    auto display = layout_node->display();
    return display.is_outside_and_inside() && display.is_block_outside();
}

static bool has_line_break_between(Node& first, Node& second, Node& boundary)
{
    for (auto* node = first.next_in_pre_order(&boundary); node && node != &second; node = node->next_in_pre_order(&boundary)) {
        if (is_line_break_node(*node))
            return true;
    }
    return false;
}

static GC::Ptr<Text> next_text_node_across_line_break(Node& current, Node& boundary)
{
    for (auto* node = current.next_in_pre_order(&boundary); node; node = node->next_in_pre_order(&boundary)) {
        if (is_line_break_node(*node))
            return next_text_node_within(*node, boundary);
    }
    return {};
}

static GC::Ptr<Text> previous_text_node_across_line_break(Node& current, Node& boundary)
{
    for (auto* node = current.previous_in_pre_order(); node && node != &boundary; node = node->previous_in_pre_order()) {
        if (is_line_break_node(*node))
            return previous_text_node_within(*node, boundary);
    }
    return {};
}

GC_DEFINE_ALLOCATOR(EditingHostManager);

GC::Ref<EditingHostManager> EditingHostManager::create(JS::Realm& realm, GC::Ref<Document> document)
{
    return realm.create<EditingHostManager>(document);
}

EditingHostManager::EditingHostManager(GC::Ref<Document> document)
    : m_document(document)
{
}

void EditingHostManager::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
    visitor.visit(m_active_contenteditable_element);
}

void EditingHostManager::handle_insert(FlyString const&, Utf16String const& value)
{
    // https://w3c.github.io/editing/docs/execCommand/#additional-requirements
    // When the user instructs the user agent to insert text inside an editing host, such as by typing on the keyboard
    // while the cursor is in an editable node, the user agent must call execCommand("inserttext", false, value) on the
    // relevant document, with value equal to the text the user provided. If the user inserts multiple characters at
    // once or in quick succession, this specification does not define whether it is treated as one insertion or several
    // consecutive insertions.
    auto editing_result = m_document->exec_command(Editing::CommandNames::insertText, false, value);
    if (editing_result.is_exception())
        dbgln("handle_insert(): editing resulted in exception: {}", editing_result.exception());
}

void EditingHostManager::select_all()
{
    if (!m_active_contenteditable_element) {
        return;
    }
    auto selection = m_document->get_selection();
    if (!selection->anchor_node() || !selection->focus_node()) {
        return;
    }
    MUST(selection->set_base_and_extent(*selection->anchor_node(), 0, *selection->focus_node(), selection->focus_node()->length()));
}

void EditingHostManager::set_selection_anchor(GC::Ref<DOM::Node> anchor_node, size_t anchor_offset)
{
    auto selection = m_document->get_selection();
    MUST(selection->collapse(*anchor_node, anchor_offset));
    m_document->reset_cursor_blink_cycle();
}

void EditingHostManager::set_selection_focus(GC::Ref<DOM::Node> focus_node, size_t focus_offset)
{
    if (!m_active_contenteditable_element || !m_active_contenteditable_element->is_ancestor_of(*focus_node))
        return;
    auto selection = m_document->get_selection();
    if (!selection->anchor_node())
        return;
    MUST(selection->set_base_and_extent(*selection->anchor_node(), selection->anchor_offset(), *focus_node, focus_offset));
    m_document->reset_cursor_blink_cycle();
}

GC::Ptr<Selection::Selection> EditingHostManager::get_selection_for_navigation(CollapseSelection collapse) const
{
    // In order for navigation to happen inside an editing host, the document must have a selection,
    auto selection = m_document->get_selection();
    if (!selection)
        return {};

    // and the focus node must be inside a text node,
    auto focus_node = selection->focus_node();
    if (!is<Text>(focus_node.ptr()))
        return {};

    // and if we're performing collapsed navigation (i.e. moving the caret), the focus node must be editable.
    if (collapse == CollapseSelection::Yes && !focus_node->is_editable())
        return {};

    m_document->update_layout(UpdateLayoutReason::EditingCursorNavigation);
    return selection;
}

void EditingHostManager::move_selection_focus_to_text_node(Selection::Selection& selection, Node& old_focus_node, Text& target, unsigned offset, bool collapse_selection)
{
    old_focus_node.invalidate_cursor_paint_cache();
    if (collapse_selection) {
        MUST(selection.collapse(&target, offset));
    } else {
        MUST(selection.set_base_and_extent(*selection.anchor_node(), selection.anchor_offset(), target, offset));
    }
    m_document->reset_cursor_blink_cycle();
    selection.scroll_focus_into_view();
}

void EditingHostManager::move_cursor_to_start(CollapseSelection collapse)
{
    auto selection = get_selection_for_navigation(collapse);
    if (!selection)
        return;
    auto node = selection->focus_node();

    // In a contenteditable, find the first text node on the current line by walking backward past adjacent inline text
    // nodes, stopping at line breaks.
    if (m_active_contenteditable_element) {
        while (auto previous = previous_text_node_within(*node, *m_active_contenteditable_element)) {
            if (has_line_break_between(*previous, *node, *m_active_contenteditable_element))
                break;
            node = previous;
        }
    }

    if (collapse == CollapseSelection::Yes) {
        MUST(selection->collapse(node, 0));
        m_document->reset_cursor_blink_cycle();
    } else {
        MUST(selection->set_base_and_extent(*selection->anchor_node(), selection->anchor_offset(), *node, 0));
    }
    selection->scroll_focus_into_view();
}

void EditingHostManager::move_cursor_to_end(CollapseSelection collapse)
{
    auto selection = get_selection_for_navigation(collapse);
    if (!selection)
        return;
    auto node = selection->focus_node();

    // In a contenteditable, find the last text node on the current line by walking forward past adjacent inline text
    // nodes, stopping at line breaks.
    if (m_active_contenteditable_element) {
        while (auto next = next_text_node_within(*node, *m_active_contenteditable_element)) {
            if (has_line_break_between(*node, *next, *m_active_contenteditable_element))
                break;
            node = next;
        }
    }

    if (collapse == CollapseSelection::Yes) {
        m_document->reset_cursor_blink_cycle();
        MUST(selection->collapse(node, node->length()));
    } else {
        MUST(selection->set_base_and_extent(*selection->anchor_node(), selection->anchor_offset(), *node, node->length()));
    }
    selection->scroll_focus_into_view();
}

template<typename MoveFunction>
void EditingHostManager::increment_cursor_position(CollapseSelection collapse, MoveFunction move)
{
    auto selection = get_selection_for_navigation(collapse);
    if (!selection)
        return;

    auto collapse_selection = collapse == CollapseSelection::Yes;
    if (collapse_selection && !selection->is_collapsed()) {
        move(*selection, collapse_selection);
        return;
    }

    auto old_focus_node = selection->focus_node();
    auto old_focus_offset = selection->focus_offset();

    move(*selection, collapse_selection);

    // The end of one text node and the start of the next are the same visual cursor position when they are adjacent
    // inline content on the same line. Cross to the next text node, and advance one additional unit only if there is
    // no line break between them.
    if (m_active_contenteditable_element
        && selection->focus_node() == old_focus_node
        && selection->focus_offset() == old_focus_offset) {
        auto next_text = next_text_node_within(*old_focus_node, *m_active_contenteditable_element);
        if (!next_text)
            return;
        move_selection_focus_to_text_node(*selection, *old_focus_node, *next_text, 0, collapse_selection);
        if (!has_line_break_between(*old_focus_node, *next_text, *m_active_contenteditable_element))
            move(*selection, collapse_selection);
    }
}

template<typename MoveFunction>
void EditingHostManager::decrement_cursor_position(CollapseSelection collapse, MoveFunction move)
{
    auto selection = get_selection_for_navigation(collapse);
    if (!selection)
        return;

    auto collapse_selection = collapse == CollapseSelection::Yes;
    if (collapse_selection && !selection->is_collapsed()) {
        move(*selection, collapse_selection);
        return;
    }

    auto old_focus_node = selection->focus_node();
    auto old_focus_offset = selection->focus_offset();

    move(*selection, collapse_selection);

    // The start of one text node and the end of the previous are the same visual cursor position when they are
    // adjacent inline content on the same line. Cross to the previous text node, and retreat one additional unit only
    // if there is no line break between them.
    if (m_active_contenteditable_element
        && selection->focus_node() == old_focus_node
        && selection->focus_offset() == old_focus_offset) {
        auto previous_text = previous_text_node_within(*old_focus_node, *m_active_contenteditable_element);
        if (!previous_text)
            return;
        move_selection_focus_to_text_node(*selection, *old_focus_node, *previous_text, previous_text->length(), collapse_selection);
        if (!has_line_break_between(*previous_text, *old_focus_node, *m_active_contenteditable_element))
            move(*selection, collapse_selection);
    }
}

void EditingHostManager::increment_cursor_position_offset(CollapseSelection collapse)
{
    increment_cursor_position(collapse, [](auto& selection, auto collapse_selection) {
        selection.move_offset_to_next_character(collapse_selection);
    });
}

void EditingHostManager::decrement_cursor_position_offset(CollapseSelection collapse)
{
    decrement_cursor_position(collapse, [](auto& selection, auto collapse_selection) {
        selection.move_offset_to_previous_character(collapse_selection);
    });
}

void EditingHostManager::increment_cursor_position_to_next_word(CollapseSelection collapse)
{
    increment_cursor_position(collapse, [](auto& selection, auto collapse_selection) {
        selection.move_offset_to_next_word(collapse_selection);
    });
}

void EditingHostManager::decrement_cursor_position_to_previous_word(CollapseSelection collapse)
{
    decrement_cursor_position(collapse, [](auto& selection, auto collapse_selection) {
        selection.move_offset_to_previous_word(collapse_selection);
    });
}

void EditingHostManager::increment_cursor_position_to_next_line(CollapseSelection collapse)
{
    auto selection = get_selection_for_navigation(collapse);
    if (!selection)
        return;

    auto collapse_selection = collapse == CollapseSelection::Yes;
    auto old_focus_node = selection->focus_node();
    auto old_focus_offset = selection->focus_offset();

    // Selection::move_offset_to_next_line uses VisualLineIterator which handles within-block
    // cross-node navigation (e.g. across <b> boundaries or <br> within the same block).
    selection->move_offset_to_next_line(collapse_selection);

    // If the cursor moved (different node or different offset), we are done.
    if (!m_active_contenteditable_element
        || selection->focus_node() != old_focus_node
        || selection->focus_offset() != old_focus_offset)
        return;

    // The cursor did not move, meaning we are at the last line in the current block. Fall back to
    // DOM walking for crossing block boundaries (e.g. <p> to <p>).
    auto next_text = next_text_node_across_line_break(*old_focus_node, *m_active_contenteditable_element);
    if (!next_text)
        return;

    auto& focus_text_node = as<Text>(*old_focus_node);
    auto position_in_line = old_focus_offset - find_line_start(focus_text_node, old_focus_offset);
    auto offset = min(position_in_line, find_line_end(*next_text, 0));

    move_selection_focus_to_text_node(*selection, *old_focus_node, *next_text, offset, collapse_selection);
}

void EditingHostManager::decrement_cursor_position_to_previous_line(CollapseSelection collapse)
{
    auto selection = get_selection_for_navigation(collapse);
    if (!selection)
        return;

    auto collapse_selection = collapse == CollapseSelection::Yes;
    auto old_focus_node = selection->focus_node();
    auto old_focus_offset = selection->focus_offset();

    // Selection::move_offset_to_previous_line uses VisualLineIterator which handles within-block
    // cross-node navigation (e.g. across <b> boundaries or <br> within the same block).
    selection->move_offset_to_previous_line(collapse_selection);

    // If the cursor moved (different node or different offset), we are done.
    if (!m_active_contenteditable_element
        || selection->focus_node() != old_focus_node
        || selection->focus_offset() != old_focus_offset)
        return;

    // The cursor did not move, meaning we are at the first line in the current block. Fall back to
    // DOM walking for crossing block boundaries (e.g. <p> to <p>).
    auto previous_text = previous_text_node_across_line_break(*old_focus_node, *m_active_contenteditable_element);
    if (!previous_text)
        return;

    auto& focus_text_node = as<Text>(*old_focus_node);
    auto position_in_line = old_focus_offset - find_line_start(focus_text_node, old_focus_offset);
    auto last_line_start = find_line_start(*previous_text, previous_text->data().length_in_code_units());
    auto offset = last_line_start + min(position_in_line, previous_text->data().length_in_code_units() - last_line_start);

    move_selection_focus_to_text_node(*selection, *old_focus_node, *previous_text, offset, collapse_selection);
}

void EditingHostManager::handle_delete(FlyString const& input_type)
{
    // https://w3c.github.io/editing/docs/execCommand/#additional-requirements
    // When the user instructs the user agent to delete the previous character inside an editing host, such as by
    // pressing the Backspace key while the cursor is in an editable node, the user agent must call
    // execCommand("delete") on the relevant document.
    // When the user instructs the user agent to delete the next character inside an editing host, such as by pressing
    // the Delete key while the cursor is in an editable node, the user agent must call execCommand("forwarddelete") on
    // the relevant document.
    auto command = input_type == UIEvents::InputTypes::deleteContentBackward ? Editing::CommandNames::delete_ : Editing::CommandNames::forwardDelete;
    auto editing_result = m_document->exec_command(command, false, {});
    if (editing_result.is_exception())
        dbgln("handle_delete(): editing resulted in exception: {}", editing_result.exception());
}

EventResult EditingHostManager::handle_return_key(FlyString const& ui_input_type)
{
    VERIFY(ui_input_type == UIEvents::InputTypes::insertParagraph || ui_input_type == UIEvents::InputTypes::insertLineBreak);

    // https://w3c.github.io/editing/docs/execCommand/#additional-requirements
    // When the user instructs the user agent to insert a line break inside an editing host, such as by pressing the
    // Enter key while the cursor is in an editable node, the user agent must call execCommand("insertparagraph") on the
    // relevant document.
    // When the user instructs the user agent to insert a line break inside an editing host without breaking out of the
    // current block, such as by pressing Shift-Enter or Option-Enter while the cursor is in an editable node, the user
    // agent must call execCommand("insertlinebreak") on the relevant document.
    auto command = ui_input_type == UIEvents::InputTypes::insertParagraph
        ? Editing::CommandNames::insertParagraph
        : Editing::CommandNames::insertLineBreak;
    auto editing_result = m_document->exec_command(command, false, {});
    if (editing_result.is_exception()) {
        dbgln("handle_return_key(): editing resulted in exception: {}", editing_result.exception());
        return EventResult::Dropped;
    }
    return editing_result.value() ? EventResult::Handled : EventResult::Dropped;
}

bool EditingHostManager::is_within_active_contenteditable(Node const& node) const
{
    if (!m_active_contenteditable_element)
        return false;
    Node const* active = m_active_contenteditable_element.ptr();
    return node.find_in_shadow_including_ancestry([&](Node const& it) { return &it == active; });
}

}
