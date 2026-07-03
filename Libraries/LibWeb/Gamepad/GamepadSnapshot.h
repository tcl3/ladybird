/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibIPC/Forward.h>
#include <LibWeb/Export.h>

namespace Web::Gamepad {

// An identifier for a connected gamepad, allocated by the UI process. Handles are allocated monotonically and never
// reused, so a stale handle in an in-flight IPC message can never refer to a different device.
using GamepadHandle = u32;

// Describes one axis or button input of a gamepad device, in the order the UI process enumerates the device's inputs.
// canonical_index is the input's position in the W3C Standard Gamepad layout, if it corresponds to one.
struct WEB_API GamepadInputDescriptor {
    Optional<u32> canonical_index;
    i16 logical_minimum { 0 };
    i16 logical_maximum { 0 };
};

// Everything WebContent needs to construct a Gamepad platform object, sent once when a device is connected.
struct WEB_API GamepadDescription {
    GamepadHandle handle { 0 };
    String id;
    bool has_standard_mapping { false };
    bool supports_dual_rumble { false };
    bool supports_trigger_rumble { false };
    bool is_virtual { false };
    Vector<GamepadInputDescriptor> axes;
    Vector<GamepadInputDescriptor> buttons;
};

// Raw logical input values for a device, parallel to the description's axis and button lists.
struct WEB_API GamepadState {
    GamepadHandle handle { 0 };
    Vector<i16> axis_values;
    Vector<i16> button_values;

    bool operator==(GamepadState const&) const = default;
};

struct WEB_API GamepadDualRumbleEffect {
    u16 strong_magnitude { 0 };
    u16 weak_magnitude { 0 };
};

struct WEB_API GamepadTriggerRumbleEffect {
    u16 left_trigger_magnitude { 0 };
    u16 right_trigger_magnitude { 0 };
};

using GamepadEffect = Variant<GamepadDualRumbleEffect, GamepadTriggerRumbleEffect>;

struct WEB_API GamepadConnectedEvent {
    GamepadDescription description;
};

struct WEB_API GamepadStateUpdatedEvent {
    GamepadState state;
};

struct WEB_API GamepadDisconnectedEvent {
    GamepadHandle handle { 0 };
};

// A change to the set of connected gamepads or to a gamepad's input state, as observed by the UI process.
using GamepadChangeEvent = Variant<GamepadConnectedEvent, GamepadStateUpdatedEvent, GamepadDisconnectedEvent>;

// A rumble command received by a virtual test gamepad, recorded for inspection via the Internals object.
struct WEB_API ReceivedDualRumbleEffect {
    u16 low_frequency_rumble { 0 };
    u16 high_frequency_rumble { 0 };
};

struct WEB_API ReceivedTriggerRumbleEffect {
    u16 left_rumble { 0 };
    u16 right_rumble { 0 };
};

struct WEB_API ReceivedRumbleEffects {
    Vector<ReceivedDualRumbleEffect> dual_rumble_effects;
    Vector<ReceivedTriggerRumbleEffect> trigger_rumble_effects;
};

// A virtual test gamepad created in the UI process. The button, axis, and trigger lists contain the input identifiers
// accepted by set_virtual_gamepad_button and set_virtual_gamepad_axis for each of the device's inputs; their meaning
// is defined by the UI process.
struct WEB_API VirtualGamepad {
    GamepadHandle handle { 0 };
    Vector<i32> buttons;
    Vector<i32> axes;
    Vector<i32> triggers;
};

}

namespace IPC {

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Gamepad::GamepadInputDescriptor const&);

template<>
WEB_API ErrorOr<Web::Gamepad::GamepadInputDescriptor> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Gamepad::GamepadDescription const&);

template<>
WEB_API ErrorOr<Web::Gamepad::GamepadDescription> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Gamepad::GamepadState const&);

template<>
WEB_API ErrorOr<Web::Gamepad::GamepadState> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Gamepad::GamepadDualRumbleEffect const&);

template<>
WEB_API ErrorOr<Web::Gamepad::GamepadDualRumbleEffect> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Gamepad::GamepadTriggerRumbleEffect const&);

template<>
WEB_API ErrorOr<Web::Gamepad::GamepadTriggerRumbleEffect> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Gamepad::GamepadConnectedEvent const&);

template<>
WEB_API ErrorOr<Web::Gamepad::GamepadConnectedEvent> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Gamepad::GamepadStateUpdatedEvent const&);

template<>
WEB_API ErrorOr<Web::Gamepad::GamepadStateUpdatedEvent> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Gamepad::GamepadDisconnectedEvent const&);

template<>
WEB_API ErrorOr<Web::Gamepad::GamepadDisconnectedEvent> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Gamepad::ReceivedDualRumbleEffect const&);

template<>
WEB_API ErrorOr<Web::Gamepad::ReceivedDualRumbleEffect> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Gamepad::ReceivedTriggerRumbleEffect const&);

template<>
WEB_API ErrorOr<Web::Gamepad::ReceivedTriggerRumbleEffect> decode(Decoder&);

template<>
WEB_API ErrorOr<void> encode(Encoder&, Web::Gamepad::VirtualGamepad const&);

template<>
WEB_API ErrorOr<Web::Gamepad::VirtualGamepad> decode(Decoder&);

}
