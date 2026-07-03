/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/Vector.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibCore/Forward.h>
#include <LibWeb/Gamepad/GamepadSharedState.h>
#include <LibWeb/Gamepad/GamepadSnapshot.h>
#include <LibWebView/Forward.h>

struct SDL_Gamepad;
struct SDL_Joystick;

union SDL_Event;

namespace WebView {

class WebContentClient;

// Owns all gamepad devices on behalf of the sandboxed WebContent processes, which cannot open input device nodes
// themselves. Monitoring is consumer-driven: SDL is initialized and devices are polled only while at least one
// WebContent process has a page that uses the Gamepad API. Device connections travel to those processes as IPC
// messages, while input state is published through a shared memory buffer that the processes sample, and only when
// it has changed.
//
// Virtual test gamepads are scoped to the WebContent process that created them: their connect, input, and disconnect
// events are buffered and handed over through that process's synchronous pump_gamepad_events message instead of being
// pushed, so concurrently running tests cannot observe each other's devices.
class WEBVIEW_API GamepadManager {
public:
    static GamepadManager& the();

    void client_did_start_using_gamepads(WebContentClient&);
    void client_disconnected(WebContentClient&);

    void play_effect(Web::Gamepad::GamepadHandle, Web::Gamepad::GamepadEffect const&);
    bool stop_effects(Web::Gamepad::GamepadHandle);

    // Test-only interface; requests are rejected unless the internals object is exposed.
    Optional<Web::Gamepad::VirtualGamepad> create_virtual_gamepad(WebContentClient&);
    void set_virtual_gamepad_button(WebContentClient&, Web::Gamepad::GamepadHandle, i32 button, bool down);
    void set_virtual_gamepad_axis(WebContentClient&, Web::Gamepad::GamepadHandle, i32 axis, i16 value);
    void disconnect_virtual_gamepad(WebContentClient&, Web::Gamepad::GamepadHandle);
    Vector<Web::Gamepad::GamepadChangeEvent> pump_gamepad_events(WebContentClient&);
    Vector<Web::Gamepad::ReceivedDualRumbleEffect> virtual_gamepad_received_rumble_effects(WebContentClient&, Web::Gamepad::GamepadHandle);
    Vector<Web::Gamepad::ReceivedTriggerRumbleEffect> virtual_gamepad_received_trigger_rumble_effects(WebContentClient&, Web::Gamepad::GamepadHandle);

    struct Device {
        u32 sdl_joystick_id { 0 };
        SDL_Gamepad* sdl_gamepad { nullptr };
        Web::Gamepad::GamepadDescription description;

        // Whether the device's connected event has been delivered; set when SDL reports the device added.
        bool announced { false };

        // Real devices only: the device's slot in the shared state buffer, held from announcement until removal.
        Optional<size_t> shared_state_slot_index;

        // The input state most recently published for this device, used to publish state only when it has changed.
        // NB: Change detection compares freshly read state instead of watching SDL's input events, because SDL
        //     suppresses some of those (for example, a guide button release shortly after its press).
        Web::Gamepad::GamepadState last_published_state;

        // Virtual test devices only:
        WebContentClient* virtual_device_owner { nullptr };
        SDL_Joystick* virtual_sdl_joystick { nullptr };
        Vector<Web::Gamepad::ReceivedDualRumbleEffect> received_dual_rumble_effects;
        Vector<Web::Gamepad::ReceivedTriggerRumbleEffect> received_trigger_rumble_effects;
    };

private:
    bool ensure_sdl_initialized();
    void update_polling_state();
    void drain_sdl_events();
    void handle_sdl_event(SDL_Event const&);
    void publish_changed_input_states();

    Optional<size_t> allocate_shared_state_slot() const;
    Web::Gamepad::SharedGamepadStateSlot& shared_state_slot(size_t slot_index);

    void device_connected(u32 sdl_joystick_id);
    void device_disconnected(u32 sdl_joystick_id);

    Web::Gamepad::GamepadDescription build_description(Web::Gamepad::GamepadHandle, u32 sdl_joystick_id, SDL_Gamepad*) const;
    void read_current_state(Device const&, Web::Gamepad::GamepadState&) const;

    Device* device_for_handle(Web::Gamepad::GamepadHandle);
    Device* virtual_device_owned_by_client(WebContentClient&, Web::Gamepad::GamepadHandle);
    Device* device_for_sdl_joystick_id(u32 sdl_joystick_id);

    void announce_device(Device&);
    void emit_event_for_device(Device const&, Web::Gamepad::GamepadChangeEvent);
    void remove_device(Web::Gamepad::GamepadHandle);

    bool m_sdl_initialized { false };
    Web::Gamepad::GamepadHandle m_next_handle { 1 };

    // Keyed by handle; insertion order is preserved so devices are replayed to new consumers in connection order.
    OrderedHashMap<Web::Gamepad::GamepadHandle, NonnullOwnPtr<Device>> m_devices;

    HashTable<WebContentClient*> m_clients_using_gamepads;
    HashMap<WebContentClient*, Vector<Web::Gamepad::GamepadChangeEvent>> m_buffered_virtual_gamepad_events;
    RefPtr<Core::Timer> m_poll_timer;
    Core::AnonymousBuffer m_shared_state_buffer;

    // Scratch state reused when polling devices, so unchanged input does not allocate every tick.
    Web::Gamepad::GamepadState m_scratch_state;
};

}
