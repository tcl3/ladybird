/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/NeverDestroyed.h>
#include <LibCore/Timer.h>
#include <LibWebView/Application.h>
#include <LibWebView/GamepadManager.h>
#include <LibWebView/WebContentClient.h>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_joystick.h>

#if defined(AK_OS_WINDOWS)
#    include <objbase.h>
#endif

namespace WebView {

// How often we ask SDL for device and input changes while at least one WebContent process uses the Gamepad API.
// Together with each WebContent process's own sampling cadence, this bounds input latency.
static constexpr int GAMEPAD_POLL_INTERVAL_MS = 8;

// WebContent validates effects to at most 5000 ms and normally ends them with an explicit stop before the SDL
// expiration passed by play_effect() fires; the slack keeps that ordering. The maximum is enforced here as well so a
// compromised WebContent process cannot request an unbounded rumble.
static constexpr u32 GAMEPAD_EFFECT_MAX_DURATION_MS = 5'000;
static constexpr u32 GAMEPAD_EFFECT_EXPIRATION_SLACK_MS = 1'000;

static u32 effect_expiration_backstop_ms(u32 duration)
{
    return min(duration, GAMEPAD_EFFECT_MAX_DURATION_MS) + GAMEPAD_EFFECT_EXPIRATION_SLACK_MS;
}

// https://w3c.github.io/gamepad/#dfn-standard-gamepad
// Type     Index   Location
// Button   0       Bottom button in right cluster
//          1       Right button in right cluster
//          2       Left button in right cluster
//          3       Top button in right cluster
//          4       Top left front button
//          5       Top right front button
//          6       Bottom left front button
//          7       Bottom right front button
//          8       Left button in center cluster
//          9       Right button in center cluster
//          10      Left stick pressed button
//          11      Right stick pressed button
//          12      Top button in left cluster
//          13      Bottom button in left cluster
//          14      Left button in left cluster
//          15      Right button in left cluster
//          16      Center button in center cluster
static Array<Variant<SDL_GamepadButton, SDL_GamepadAxis>, 17> const standard_gamepad_button_layout {
    SDL_GAMEPAD_BUTTON_SOUTH,
    SDL_GAMEPAD_BUTTON_EAST,
    SDL_GAMEPAD_BUTTON_WEST,
    SDL_GAMEPAD_BUTTON_NORTH,
    SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,
    SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER,
    SDL_GAMEPAD_AXIS_LEFT_TRIGGER,
    SDL_GAMEPAD_AXIS_RIGHT_TRIGGER,
    SDL_GAMEPAD_BUTTON_BACK,
    SDL_GAMEPAD_BUTTON_START,
    SDL_GAMEPAD_BUTTON_LEFT_STICK,
    SDL_GAMEPAD_BUTTON_RIGHT_STICK,
    SDL_GAMEPAD_BUTTON_DPAD_UP,
    SDL_GAMEPAD_BUTTON_DPAD_DOWN,
    SDL_GAMEPAD_BUTTON_DPAD_LEFT,
    SDL_GAMEPAD_BUTTON_DPAD_RIGHT,
    SDL_GAMEPAD_BUTTON_GUIDE,
};

static Array<SDL_GamepadButton, 11> const non_standard_gamepad_button_layout {
    SDL_GAMEPAD_BUTTON_MISC1,
    SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1,
    SDL_GAMEPAD_BUTTON_LEFT_PADDLE1,
    SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2,
    SDL_GAMEPAD_BUTTON_LEFT_PADDLE2,
    SDL_GAMEPAD_BUTTON_TOUCHPAD,
    SDL_GAMEPAD_BUTTON_MISC2,
    SDL_GAMEPAD_BUTTON_MISC3,
    SDL_GAMEPAD_BUTTON_MISC4,
    SDL_GAMEPAD_BUTTON_MISC5,
    SDL_GAMEPAD_BUTTON_MISC6,
};

// axes     0       Horizontal axis for left stick (negative left/positive right)
//          1       Vertical axis for left stick (negative up/positive down)
//          2       Horizontal axis for right stick (negative left/positive right)
//          3       Vertical axis for right stick (negative up/positive down)
static Array<SDL_GamepadAxis, 4> const standard_gamepad_axes_layout {
    SDL_GAMEPAD_AXIS_LEFTX,
    SDL_GAMEPAD_AXIS_LEFTY,
    SDL_GAMEPAD_AXIS_RIGHTX,
    SDL_GAMEPAD_AXIS_RIGHTY,
};

static constexpr char const* VIRTUAL_GAMEPAD_NAME = "Ladybird Virtual Gamepad";

static SDLCALL bool record_received_dual_rumble_effect(void* user_data, u16 low_frequency_rumble, u16 high_frequency_rumble)
{
    auto* device = static_cast<GamepadManager::Device*>(user_data);
    device->received_dual_rumble_effects.append({ low_frequency_rumble, high_frequency_rumble });
    return true;
}

static SDLCALL bool record_received_trigger_rumble_effect(void* user_data, u16 left_rumble, u16 right_rumble)
{
    auto* device = static_cast<GamepadManager::Device*>(user_data);
    device->received_trigger_rumble_effects.append({ left_rumble, right_rumble });
    return true;
}

GamepadManager& GamepadManager::the()
{
    static NeverDestroyed<GamepadManager> s_the;
    return *s_the;
}

bool GamepadManager::ensure_sdl_initialized()
{
    if (m_sdl_initialized)
        return true;

    if (!m_shared_state_buffer.is_valid()) {
        auto shared_state_buffer = Core::AnonymousBuffer::create_with_size(sizeof(Web::Gamepad::SharedGamepadStateBuffer));
        if (shared_state_buffer.is_error()) {
            dbgln("Failed to allocate the shared gamepad state buffer: {}", shared_state_buffer.error());
            return false;
        }
        m_shared_state_buffer = shared_state_buffer.release_value();
    }

#if defined(AK_OS_WINDOWS)
    // NOTE: Initialize COM in the multithreaded model before SDL gets a chance to initialize it apartment-threaded,
    //       which would add a high overhead to calls across threads. If another component already chose a model,
    //       keep it.
    (void)CoInitializeEx(0, COINIT_MULTITHREADED);
#endif

    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        dbgln("Failed to initialize SDL3: {}", SDL_GetError());
        return false;
    }

    m_sdl_initialized = true;
    return true;
}

void GamepadManager::update_polling_state()
{
    bool should_poll = m_sdl_initialized && !m_clients_using_gamepads.is_empty();

    if (!should_poll) {
        if (m_poll_timer)
            m_poll_timer->stop();
        return;
    }

    if (!m_poll_timer) {
        m_poll_timer = Core::Timer::create_repeating(GAMEPAD_POLL_INTERVAL_MS, [this] {
            drain_sdl_events();
            publish_changed_input_states();
        });
    }

    m_poll_timer->start();
}

void GamepadManager::client_did_start_using_gamepads(WebContentClient& client)
{
    if (m_clients_using_gamepads.contains(&client))
        return;

    if (!ensure_sdl_initialized())
        return;

    // Pick up any pending device changes before registering the new consumer, so that it learns about every device
    // exactly once, via the replay below.
    drain_sdl_events();
    publish_changed_input_states();

    m_clients_using_gamepads.set(&client);
    client.async_set_gamepad_state_buffer(m_shared_state_buffer);

    for (auto& entry : m_devices) {
        auto& device = *entry.value;
        if (device.virtual_device_owner || !device.announced)
            continue;
        client.async_gamepad_connected(device.description);
    }

    update_polling_state();
}

void GamepadManager::client_disconnected(WebContentClient& client)
{
    m_clients_using_gamepads.remove(&client);

    // Silence any devices the client was still rumbling; the explicit stop it owed will never arrive.
    for (auto& entry : m_devices) {
        if (entry.value->effect_owner == &client)
            stop_effects(entry.key);
    }

    Vector<Web::Gamepad::GamepadHandle> owned_virtual_devices;
    for (auto& entry : m_devices) {
        if (entry.value->virtual_device_owner == &client)
            owned_virtual_devices.append(entry.key);
    }
    for (auto handle : owned_virtual_devices)
        remove_device(handle);

    // NB: After removing the devices, so that removal does not buffer disconnect events for the dead client.
    m_buffered_virtual_gamepad_events.remove(&client);

    update_polling_state();
}

void GamepadManager::play_effect(WebContentClient& client, Web::Gamepad::GamepadHandle handle, Web::Gamepad::GamepadEffect const& effect)
{
    auto* device = device_for_handle(handle);
    if (!device)
        return;

    // WebContent owns effect durations and explicitly sends a zero-magnitude effect or a stop request when an effect
    // ends. The expiration passed to SDL is a backstop that silences the device if the client dies or hangs before
    // doing so; a zero-magnitude effect is itself a stop, so it carries no expiration.
    // NB: Each SDL rumble call replaces both the previous effect and its pending expiration, so a chained effect
    //     cannot be cut short by its predecessor's backstop.
    auto apply_rumble = [&](auto sdl_rumble_function, u16 first_magnitude, u16 second_magnitude, u32 duration) {
        bool is_stop = first_magnitude == 0 && second_magnitude == 0;
        device->effect_owner = is_stop ? nullptr : &client;
        sdl_rumble_function(device->sdl_gamepad, first_magnitude, second_magnitude, is_stop ? 0 : effect_expiration_backstop_ms(duration));
    };
    effect.visit(
        [&](Web::Gamepad::GamepadDualRumbleEffect const& dual_rumble_effect) {
            apply_rumble(SDL_RumbleGamepad, dual_rumble_effect.strong_magnitude, dual_rumble_effect.weak_magnitude, dual_rumble_effect.duration);
        },
        [&](Web::Gamepad::GamepadTriggerRumbleEffect const& trigger_rumble_effect) {
            apply_rumble(SDL_RumbleGamepadTriggers, trigger_rumble_effect.left_trigger_magnitude, trigger_rumble_effect.right_trigger_magnitude, trigger_rumble_effect.duration);
        });
}

bool GamepadManager::stop_effects(Web::Gamepad::GamepadHandle handle)
{
    auto* device = device_for_handle(handle);
    if (!device)
        return false;

    device->effect_owner = nullptr;

    bool stopped_all = true;

    // https://wiki.libsdl.org/SDL3/SDL_RumbleGamepad
    // "Each call to this function cancels any previous rumble effect, and calling it with 0 intensity stops any
    // rumbling."
    if (device->description.supports_dual_rumble) {
        if (!SDL_RumbleGamepad(device->sdl_gamepad, 0, 0, 0))
            stopped_all = false;
    }

    // https://wiki.libsdl.org/SDL3/SDL_RumbleGamepadTriggers
    // "Each call to this function cancels any previous trigger rumble effect, and calling it with 0 intensity stops
    // any rumbling."
    if (device->description.supports_trigger_rumble) {
        if (!SDL_RumbleGamepadTriggers(device->sdl_gamepad, 0, 0, 0))
            stopped_all = false;
    }

    return stopped_all;
}

Optional<Web::Gamepad::VirtualGamepad> GamepadManager::create_virtual_gamepad(WebContentClient& client)
{
    // Virtual gamepads exist to let the Internals object drive the Gamepad API in tests. A compromised WebContent
    // process must not be able to fabricate gamepad input in a regular browsing session.
    if (Application::web_content_options().expose_internals_object != ExposeInternalsObject::Yes) {
        dbgln("WebContent requested a virtual gamepad, but the internals object is not exposed; ignoring");
        return {};
    }

    if (!ensure_sdl_initialized())
        return {};

    auto device = make<Device>();

    // The virtual device exposes the Standard Gamepad layout, so tests exercise the same layout as real devices.
    Vector<i32> buttons;
    Vector<i32> triggers;
    for (auto const& standard_gamepad_button : standard_gamepad_button_layout) {
        standard_gamepad_button.visit(
            [&](SDL_GamepadButton button) { buttons.append(button); },
            [&](SDL_GamepadAxis axis) { triggers.append(axis); });
    }

    Vector<i32> axes;
    for (auto const standard_gamepad_axis : standard_gamepad_axes_layout)
        axes.append(standard_gamepad_axis);

    SDL_VirtualJoystickDesc virtual_joystick_desc {};
    SDL_INIT_INTERFACE(&virtual_joystick_desc);

    virtual_joystick_desc.type = SDL_JOYSTICK_TYPE_GAMEPAD;
    virtual_joystick_desc.naxes = axes.size() + triggers.size();
    virtual_joystick_desc.nbuttons = buttons.size();

    u32 button_mask = 0;
    for (auto const button : buttons)
        button_mask |= 1 << button;

    virtual_joystick_desc.button_mask = button_mask;

    u32 axis_mask = 0;
    for (auto const axis : axes)
        axis_mask |= 1 << axis;

    for (auto const trigger : triggers)
        axis_mask |= 1 << trigger;

    virtual_joystick_desc.axis_mask = axis_mask;

    virtual_joystick_desc.name = VIRTUAL_GAMEPAD_NAME;
    virtual_joystick_desc.userdata = device.ptr();
    virtual_joystick_desc.Rumble = record_received_dual_rumble_effect;
    virtual_joystick_desc.RumbleTriggers = record_received_trigger_rumble_effect;

    auto sdl_joystick_id = SDL_AttachVirtualJoystick(&virtual_joystick_desc);
    if (sdl_joystick_id == 0) {
        dbgln("Failed to attach virtual joystick: {}", SDL_GetError());
        return {};
    }

    auto* sdl_joystick = SDL_OpenJoystick(sdl_joystick_id);
    auto* sdl_gamepad = SDL_OpenGamepad(sdl_joystick_id);
    if (!sdl_joystick || !sdl_gamepad) {
        dbgln("Failed to open virtual gamepad: {}", SDL_GetError());
        if (sdl_gamepad)
            SDL_CloseGamepad(sdl_gamepad);
        if (sdl_joystick)
            SDL_CloseJoystick(sdl_joystick);
        SDL_DetachVirtualJoystick(sdl_joystick_id);
        return {};
    }

    auto handle = m_next_handle++;
    device->sdl_joystick_id = sdl_joystick_id;
    device->sdl_gamepad = sdl_gamepad;
    device->description = build_description(handle, sdl_joystick_id, sdl_gamepad);
    device->virtual_device_owner = &client;
    device->virtual_sdl_joystick = sdl_joystick;
    m_devices.set(handle, move(device));

    return Web::Gamepad::VirtualGamepad { handle, move(buttons), move(axes), move(triggers) };
}

void GamepadManager::set_virtual_gamepad_button(WebContentClient& client, Web::Gamepad::GamepadHandle handle, i32 button, bool down)
{
    auto* device = virtual_device_owned_by_client(client, handle);
    if (!device)
        return;
    SDL_SetJoystickVirtualButton(device->virtual_sdl_joystick, button, down);
}

void GamepadManager::set_virtual_gamepad_axis(WebContentClient& client, Web::Gamepad::GamepadHandle handle, i32 axis, i16 value)
{
    auto* device = virtual_device_owned_by_client(client, handle);
    if (!device)
        return;
    SDL_SetJoystickVirtualAxis(device->virtual_sdl_joystick, axis, value);
}

void GamepadManager::disconnect_virtual_gamepad(WebContentClient& client, Web::Gamepad::GamepadHandle handle)
{
    auto* device = virtual_device_owned_by_client(client, handle);
    if (!device)
        return;
    remove_device(handle);
}

Vector<Web::Gamepad::GamepadChangeEvent> GamepadManager::pump_gamepad_events(WebContentClient& client)
{
    if (m_sdl_initialized) {
        drain_sdl_events();
        publish_changed_input_states();
    }

    if (auto events = m_buffered_virtual_gamepad_events.take(&client); events.has_value())
        return events.release_value();
    return {};
}

Vector<Web::Gamepad::ReceivedDualRumbleEffect> GamepadManager::virtual_gamepad_received_rumble_effects(WebContentClient& client, Web::Gamepad::GamepadHandle handle)
{
    auto* device = virtual_device_owned_by_client(client, handle);
    if (!device)
        return {};
    return device->received_dual_rumble_effects;
}

Vector<Web::Gamepad::ReceivedTriggerRumbleEffect> GamepadManager::virtual_gamepad_received_trigger_rumble_effects(WebContentClient& client, Web::Gamepad::GamepadHandle handle)
{
    auto* device = virtual_device_owned_by_client(client, handle);
    if (!device)
        return {};
    return device->received_trigger_rumble_effects;
}

void GamepadManager::drain_sdl_events()
{
    SDL_Event event;
    while (SDL_PollEvent(&event))
        handle_sdl_event(event);
}

void GamepadManager::handle_sdl_event(SDL_Event const& event)
{
    switch (event.type) {
    case SDL_EVENT_GAMEPAD_ADDED:
        device_connected(event.gdevice.which);
        break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        device_disconnected(event.gdevice.which);
        break;
    default:
        break;
    }
}

void GamepadManager::publish_changed_input_states()
{
    for (auto& entry : m_devices) {
        auto& device = *entry.value;
        if (!device.announced)
            continue;
        read_current_state(device, m_scratch_state);
        if (m_scratch_state == device.last_published_state)
            continue;
        device.last_published_state = m_scratch_state;
        if (device.virtual_device_owner)
            emit_event_for_device(device, Web::Gamepad::GamepadStateUpdatedEvent { device.last_published_state });
        else
            Web::Gamepad::publish_gamepad_state_to_slot(shared_state_slot(device.shared_state_slot_index.value()), device.last_published_state);
    }
}

void GamepadManager::device_connected(u32 sdl_joystick_id)
{
    auto* device = device_for_sdl_joystick_id(sdl_joystick_id);

    if (!device) {
        auto* sdl_gamepad = SDL_OpenGamepad(sdl_joystick_id);
        if (!sdl_gamepad) {
            dbgln("Failed to open gamepad: {}", SDL_GetError());
            return;
        }

        auto handle = m_next_handle++;
        auto new_device = make<Device>();
        new_device->sdl_joystick_id = sdl_joystick_id;
        new_device->sdl_gamepad = sdl_gamepad;
        new_device->description = build_description(handle, sdl_joystick_id, sdl_gamepad);
        device = new_device.ptr();
        m_devices.set(handle, move(new_device));
    }

    announce_device(*device);
}

void GamepadManager::device_disconnected(u32 sdl_joystick_id)
{
    auto* device = device_for_sdl_joystick_id(sdl_joystick_id);
    if (!device)
        return;
    remove_device(device->description.handle);
}

void GamepadManager::announce_device(Device& device)
{
    if (device.announced)
        return;

    read_current_state(device, device.last_published_state);

    // Real devices deliver input through a slot in the shared state buffer; a device that cannot get a slot would be
    // permanently inert, so leave it unannounced.
    if (!device.virtual_device_owner) {
        device.shared_state_slot_index = allocate_shared_state_slot();
        if (!device.shared_state_slot_index.has_value()) {
            dbgln("All {} shared gamepad state slots are occupied; ignoring '{}'", Web::Gamepad::MAX_SHARED_GAMEPADS, device.description.id);
            return;
        }
        Web::Gamepad::publish_gamepad_state_to_slot(shared_state_slot(device.shared_state_slot_index.value()), device.last_published_state);
    }

    device.announced = true;
    emit_event_for_device(device, Web::Gamepad::GamepadConnectedEvent { device.description });

    // Virtual devices deliver their input state as buffered events instead of through the shared state buffer, so
    // the initial state must be delivered explicitly.
    if (device.virtual_device_owner)
        emit_event_for_device(device, Web::Gamepad::GamepadStateUpdatedEvent { device.last_published_state });
}

void GamepadManager::emit_event_for_device(Device const& device, Web::Gamepad::GamepadChangeEvent event)
{
    // Events for virtual test devices are buffered for the owning WebContent process to collect with its next
    // synchronous pump; events for real devices are pushed to every process that uses the Gamepad API.
    if (device.virtual_device_owner) {
        m_buffered_virtual_gamepad_events.ensure(device.virtual_device_owner).append(move(event));
        return;
    }

    for (auto* client : m_clients_using_gamepads) {
        event.visit(
            [&](Web::Gamepad::GamepadConnectedEvent const& connected_event) {
                client->async_gamepad_connected(connected_event.description);
            },
            [&](Web::Gamepad::GamepadStateUpdatedEvent const&) {
                // Real devices publish input state through the shared state buffer, never as an IPC message.
                VERIFY_NOT_REACHED();
            },
            [&](Web::Gamepad::GamepadDisconnectedEvent const& disconnected_event) {
                client->async_gamepad_disconnected(disconnected_event.handle);
            });
    }
}

void GamepadManager::remove_device(Web::Gamepad::GamepadHandle handle)
{
    auto device = m_devices.take(handle);
    if (!device.has_value())
        return;

    if ((*device)->shared_state_slot_index.has_value())
        Web::Gamepad::clear_gamepad_state_slot(shared_state_slot((*device)->shared_state_slot_index.value()));

    if ((*device)->announced)
        emit_event_for_device(**device, Web::Gamepad::GamepadDisconnectedEvent { handle });

    SDL_CloseGamepad((*device)->sdl_gamepad);
    if ((*device)->virtual_sdl_joystick) {
        SDL_CloseJoystick((*device)->virtual_sdl_joystick);
        SDL_DetachVirtualJoystick((*device)->sdl_joystick_id);
    }
}

Web::Gamepad::GamepadDescription GamepadManager::build_description(Web::Gamepad::GamepadHandle handle, u32 sdl_joystick_id, SDL_Gamepad* sdl_gamepad) const
{
    Web::Gamepad::GamepadDescription description;
    description.handle = handle;

    if (auto const* name = SDL_GetGamepadNameForID(sdl_joystick_id))
        description.id = String::from_utf8_with_replacement_character(StringView { name, strlen(name) });

    description.is_virtual = SDL_IsJoystickVirtual(sdl_joystick_id);

    SDL_PropertiesID sdl_gamepad_properties = SDL_GetGamepadProperties(sdl_gamepad);
    description.supports_dual_rumble = SDL_GetBooleanProperty(sdl_gamepad_properties, SDL_PROP_GAMEPAD_CAP_RUMBLE_BOOLEAN, /* default_value= */ false);
    description.supports_trigger_rumble = SDL_GetBooleanProperty(sdl_gamepad_properties, SDL_PROP_GAMEPAD_CAP_TRIGGER_RUMBLE_BOOLEAN, /* default_value= */ false);

    // https://w3c.github.io/gamepad/#dfn-selecting-a-mapping
    // The device's layout corresponds with the Standard Gamepad layout if it has every standard button and axis.
    bool has_standard_mapping = true;

    for (size_t canonical_index = 0; canonical_index < standard_gamepad_button_layout.size(); ++canonical_index) {
        bool has_input = standard_gamepad_button_layout[canonical_index].visit(
            [&](SDL_GamepadButton button) {
                return SDL_GamepadHasButton(sdl_gamepad, button);
            },
            [&](SDL_GamepadAxis axis) {
                return SDL_GamepadHasAxis(sdl_gamepad, axis);
            });

        if (!has_input) {
            has_standard_mapping = false;
            continue;
        }

        bool is_digital = standard_gamepad_button_layout[canonical_index].has<SDL_GamepadButton>();
        description.buttons.append({
            .canonical_index = static_cast<u32>(canonical_index),
            .logical_minimum = 0,
            // Buttons are binary inputs with SDL; trigger axis values range from 0 (released) to
            // SDL_JOYSTICK_AXIS_MAX (fully pressed) when reported by SDL_GetGamepadAxis().
            .logical_maximum = is_digital ? i16(1) : i16(SDL_JOYSTICK_AXIS_MAX),
        });
    }

    for (auto const non_standard_gamepad_button : non_standard_gamepad_button_layout) {
        if (!SDL_GamepadHasButton(sdl_gamepad, non_standard_gamepad_button))
            continue;
        description.buttons.append({
            .canonical_index = {},
            .logical_minimum = 0,
            .logical_maximum = 1,
        });
    }

    for (size_t canonical_index = 0; canonical_index < standard_gamepad_axes_layout.size(); ++canonical_index) {
        if (!SDL_GamepadHasAxis(sdl_gamepad, standard_gamepad_axes_layout[canonical_index])) {
            has_standard_mapping = false;
            continue;
        }
        description.axes.append({
            .canonical_index = static_cast<u32>(canonical_index),
            .logical_minimum = SDL_JOYSTICK_AXIS_MIN,
            .logical_maximum = SDL_JOYSTICK_AXIS_MAX,
        });
    }

    description.has_standard_mapping = has_standard_mapping;
    return description;
}

void GamepadManager::read_current_state(Device const& device, Web::Gamepad::GamepadState& state) const
{
    state.handle = device.description.handle;
    state.axis_values.clear_with_capacity();
    state.button_values.clear_with_capacity();

    for (auto const standard_gamepad_axis : standard_gamepad_axes_layout) {
        if (SDL_GamepadHasAxis(device.sdl_gamepad, standard_gamepad_axis))
            state.axis_values.append(SDL_GetGamepadAxis(device.sdl_gamepad, standard_gamepad_axis));
    }

    for (auto const& standard_gamepad_button : standard_gamepad_button_layout) {
        standard_gamepad_button.visit(
            [&](SDL_GamepadButton button) {
                if (SDL_GamepadHasButton(device.sdl_gamepad, button))
                    state.button_values.append(SDL_GetGamepadButton(device.sdl_gamepad, button) ? 1 : 0);
            },
            [&](SDL_GamepadAxis axis) {
                if (SDL_GamepadHasAxis(device.sdl_gamepad, axis))
                    state.button_values.append(SDL_GetGamepadAxis(device.sdl_gamepad, axis));
            });
    }

    for (auto const non_standard_gamepad_button : non_standard_gamepad_button_layout) {
        if (SDL_GamepadHasButton(device.sdl_gamepad, non_standard_gamepad_button))
            state.button_values.append(SDL_GetGamepadButton(device.sdl_gamepad, non_standard_gamepad_button) ? 1 : 0);
    }
}

Optional<size_t> GamepadManager::allocate_shared_state_slot() const
{
    for (size_t slot_index = 0; slot_index < Web::Gamepad::MAX_SHARED_GAMEPADS; ++slot_index) {
        bool slot_in_use = any_of(m_devices, [&](auto const& entry) {
            return entry.value->shared_state_slot_index == slot_index;
        });
        if (!slot_in_use)
            return slot_index;
    }
    return {};
}

Web::Gamepad::SharedGamepadStateSlot& GamepadManager::shared_state_slot(size_t slot_index)
{
    return m_shared_state_buffer.data<Web::Gamepad::SharedGamepadStateBuffer>()->slots[slot_index];
}

GamepadManager::Device* GamepadManager::device_for_handle(Web::Gamepad::GamepadHandle handle)
{
    if (auto device = m_devices.get(handle); device.has_value())
        return *device;
    return nullptr;
}

GamepadManager::Device* GamepadManager::virtual_device_owned_by_client(WebContentClient& client, Web::Gamepad::GamepadHandle handle)
{
    auto* device = device_for_handle(handle);
    if (!device || device->virtual_device_owner != &client)
        return nullptr;
    return device;
}

GamepadManager::Device* GamepadManager::device_for_sdl_joystick_id(u32 sdl_joystick_id)
{
    for (auto& entry : m_devices) {
        if (entry.value->sdl_joystick_id == sdl_joystick_id)
            return entry.value.ptr();
    }
    return nullptr;
}

}
