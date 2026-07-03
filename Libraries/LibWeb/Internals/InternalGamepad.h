/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/PlatformObject.h>
#include <LibWeb/Gamepad/GamepadSnapshot.h>

namespace Web::Internals {

// A proxy for a virtual gamepad device owned by the UI process. Input is injected and rumble commands are read back
// over IPC, keyed by the device's GamepadHandle.
class InternalGamepad : public Bindings::PlatformObject {
    WEB_PLATFORM_OBJECT(InternalGamepad, Bindings::PlatformObject);
    GC_DECLARE_ALLOCATOR(InternalGamepad);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    virtual ~InternalGamepad() override;

    Vector<i32> const& buttons() const { return m_buttons; }
    Vector<i32> const& axes() const { return m_axes; }
    Vector<i32> const& triggers() const { return m_triggers; }

    void set_button(i32 button, bool down);
    void set_axis(i32 axis, i16 value);

    GC::RootVector<JS::Object*> get_received_rumble_effects() const;
    GC::RootVector<JS::Object*> get_received_rumble_trigger_effects() const;

    void disconnect();

private:
    InternalGamepad(JS::Realm&, GC::Ref<Internals>, Gamepad::VirtualGamepad);
    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;

    Page& page() const;

    Gamepad::GamepadHandle m_handle { 0 };
    Vector<i32> m_buttons;
    Vector<i32> m_axes;
    Vector<i32> m_triggers;
    bool m_disconnected { false };
    GC::Ref<Internals> m_internals;
};

}
