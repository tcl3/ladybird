/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/HashMap.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibWeb/Export.h>
#include <LibWeb/Gamepad/GamepadSharedState.h>
#include <LibWeb/Gamepad/GamepadSnapshot.h>

namespace Web::Gamepad {

// A process-wide record of the gamepads the UI process has told us about, along with the most recent raw input state
// for each. Documents created after a gamepad was connected (for example, a new iframe or a navigation) consult this
// registry, since the gamepad_connected IPC message only reaches documents that existed when it arrived.
class WEB_API GamepadRegistry {
public:
    static GamepadRegistry& the();

    void gamepad_connected(GamepadDescription const&);
    void gamepad_state_updated(GamepadState const&);
    void gamepad_disconnected(GamepadHandle);

    void set_shared_state_buffer(Core::AnonymousBuffer);

    // Samples every shared state buffer slot written to since the previous pass, updating the latest known states.
    // Returns the states that changed.
    Vector<GamepadState> take_changed_shared_states();

    void for_each_connected_gamepad(Function<void(GamepadDescription const&)>) const;
    Optional<GamepadState const&> latest_state(GamepadHandle) const;

private:
    OrderedHashMap<GamepadHandle, GamepadDescription> m_descriptions;
    HashMap<GamepadHandle, GamepadState> m_latest_states;
    Core::AnonymousBuffer m_shared_state_buffer;

    // The sequence number each slot held when it was last sampled, so unchanged slots are skipped without copying.
    Array<u32, MAX_SHARED_GAMEPADS> m_last_observed_slot_sequences {};
};

}
