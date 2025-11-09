/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// ContentFilter.cpp

#include <AK/Queue.h>
#include <AK/StringBuilder.h>
#include <LibWeb/Loader/ContentFilter.h>

namespace Web {

ContentFilter& ContentFilter::the()
{
    static ContentFilter filter;
    return filter;
}

ContentFilter::ContentFilter() = default;

ContentFilter::~ContentFilter() = default;

bool ContentFilter::is_filtered(URL::URL const& url) const
{
    if (!filtering_enabled())
        return false;

    if (url.scheme() == "data")
        return false;

    return contains(url.to_string());
}

bool ContentFilter::contains(StringView text) const
{
    if (m_nodes.is_empty())
        return false;

    u32 state = 0;
    for (u8 ch : text.bytes()) {
        u32 temp = state;
        while (temp != 0 && !m_nodes[temp].children.contains(ch)) {
            temp = m_nodes[temp].failure_link;
        }
        if (m_nodes[temp].children.contains(ch)) {
            state = m_nodes[temp].children.get(ch).value();
        } else {
            state = 0;
        }
        if (m_nodes[state].output_link != 0)
            return true;
    }
    return false;
}

ErrorOr<void> ContentFilter::set_patterns(ReadonlySpan<String> patterns)
{
    m_nodes.clear_with_capacity();
    m_nodes.append({});

    for (u32 i = 0; i < patterns.size(); ++i) {
        auto const& pattern = patterns[i];
        u32 node = 0;

        for (u8 ch : pattern.bytes_as_string_view()) {
            auto child_opt = m_nodes[node].children.get(ch);
            if (child_opt.has_value()) {
                node = child_opt.value();
            } else {
                u32 new_node = m_nodes.size();
                m_nodes.append({});
                TRY(m_nodes[node].children.try_set(ch, new_node));
                node = new_node;
            }
        }

        if (m_nodes[node].output_link == 0)
            m_nodes[node].output_link = i + 1;
    }

    Queue<u32> queue;
    for (auto const& entry : m_nodes[0].children) {
        u32 child = entry.value;
        m_nodes[child].failure_link = 0;
        queue.enqueue(child);
    }

    while (!queue.is_empty()) {
        u32 current = queue.dequeue();

        for (auto const& entry : m_nodes[current].children) {
            u8 character = entry.key;
            u32 child = entry.value;

            u32 fail = m_nodes[current].failure_link;
            while (fail != 0 && !m_nodes[fail].children.contains(character))
                fail = m_nodes[fail].failure_link;

            u32 next_failure_link = 0;
            if (m_nodes[fail].children.contains(character))
                next_failure_link = m_nodes[fail].children.get(character).value();

            m_nodes[child].failure_link = next_failure_link;

            u32 inherited = m_nodes[next_failure_link].output_link;
            if (inherited && m_nodes[child].output_link == 0)
                m_nodes[child].output_link = inherited;

            queue.enqueue(child);
        }
    }

    return {};
}

}
