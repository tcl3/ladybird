/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Optional.h>
#include <AK/Types.h>
#include <LibWeb/Export.h>
#include <LibWeb/Gamepad/GamepadSnapshot.h>

namespace Web::Gamepad {

// Raw gamepad input state travels from the UI process to WebContent processes through a shared memory buffer rather
// than IPC messages: the UI process writes each device's state into that device's slot whenever it changes, and each
// WebContent process samples every slot once per update-the-rendering pass. A slot is guarded by a single-writer
// seqlock: its sequence number is odd while the UI process is mid-write, and a reader retries until it observes the
// same even sequence number before and after copying the slot.
inline constexpr size_t MAX_SHARED_GAMEPADS = 8;
inline constexpr size_t MAX_SHARED_GAMEPAD_AXES = 8;
inline constexpr size_t MAX_SHARED_GAMEPAD_BUTTONS = 32;

// NB: This struct must remain trivial; instances live in shared memory, which is zero-filled on creation.
struct SharedGamepadStateSlot {
    u32 sequence;
    GamepadHandle handle; // 0 while the slot is unoccupied.
    u32 axis_count;
    u32 button_count;
    Array<i16, MAX_SHARED_GAMEPAD_AXES> axis_values;
    Array<i16, MAX_SHARED_GAMEPAD_BUTTONS> button_values;
};

struct SharedGamepadStateBuffer {
    Array<SharedGamepadStateSlot, MAX_SHARED_GAMEPADS> slots;
};

WEB_API void publish_gamepad_state_to_slot(SharedGamepadStateSlot&, GamepadState const&);
WEB_API void clear_gamepad_state_slot(SharedGamepadStateSlot&);

// Returns the slot's state if the slot has been written to since last_observed_sequence, which must be the value the
// previous call for this slot left behind (initially 0, matching untouched slots). Returns nothing if the slot is
// unchanged or unoccupied, or if a consistent copy could not be obtained within a bounded number of attempts; callers
// keep their previous state in that case.
WEB_API Optional<GamepadState> read_gamepad_state_from_slot(SharedGamepadStateSlot const&, u32& last_observed_sequence);

}
