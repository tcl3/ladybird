/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Bindings/InternalGamepad.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/Internals/InternalGamepad.h>
#include <LibWeb/Internals/Internals.h>
#include <LibWeb/Page/Page.h>

namespace Web::Internals {

GC_DEFINE_ALLOCATOR(InternalGamepad);

InternalGamepad::InternalGamepad(JS::Realm& realm, GC::Ref<Internals> internals, Gamepad::VirtualGamepad virtual_gamepad)
    : Bindings::PlatformObject(realm)
    , m_handle(virtual_gamepad.handle)
    , m_buttons(move(virtual_gamepad.buttons))
    , m_axes(move(virtual_gamepad.axes))
    , m_triggers(move(virtual_gamepad.triggers))
    , m_internals(internals)
{
}

InternalGamepad::~InternalGamepad() = default;

void InternalGamepad::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(InternalGamepad);
    Base::initialize(realm);
}

void InternalGamepad::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_internals);
}

void InternalGamepad::finalize()
{
    Base::finalize();
    disconnect();
}

Page& InternalGamepad::page() const
{
    return as<HTML::Window>(HTML::relevant_global_object(*this)).page();
}

void InternalGamepad::set_button(i32 button, bool down)
{
    page().client().set_virtual_gamepad_button(m_handle, button, down);
}

void InternalGamepad::set_axis(i32 axis, i16 value)
{
    page().client().set_virtual_gamepad_axis(m_handle, axis, value);
}

GC::RootVector<JS::Object*> InternalGamepad::get_received_rumble_effects() const
{
    GC::RootVector<JS::Object*> received_rumble_effects;
    auto received_effects = page().client().virtual_gamepad_received_rumble_effects(m_handle);
    for (auto const& received_rumble_effect : received_effects.dual_rumble_effects) {
        auto object = JS::Object::create(realm(), nullptr);
        object->define_direct_property("lowFrequencyRumble"_utf16, JS::Value(received_rumble_effect.low_frequency_rumble), JS::default_attributes);
        object->define_direct_property("highFrequencyRumble"_utf16, JS::Value(received_rumble_effect.high_frequency_rumble), JS::default_attributes);
        received_rumble_effects.append(object);
    }
    return received_rumble_effects;
}

GC::RootVector<JS::Object*> InternalGamepad::get_received_rumble_trigger_effects() const
{
    GC::RootVector<JS::Object*> received_rumble_trigger_effects;
    auto received_effects = page().client().virtual_gamepad_received_rumble_effects(m_handle);
    for (auto const& received_rumble_trigger_effect : received_effects.trigger_rumble_effects) {
        auto object = JS::Object::create(realm(), nullptr);
        object->define_direct_property("leftRumble"_utf16, JS::Value(received_rumble_trigger_effect.left_rumble), JS::default_attributes);
        object->define_direct_property("rightRumble"_utf16, JS::Value(received_rumble_trigger_effect.right_rumble), JS::default_attributes);
        received_rumble_trigger_effects.append(object);
    }
    return received_rumble_trigger_effects;
}

void InternalGamepad::disconnect()
{
    if (m_disconnected)
        return;
    m_disconnected = true;

    m_internals->disconnect_virtual_gamepad(*this);
    page().client().disconnect_virtual_gamepad(m_handle);
}

}
