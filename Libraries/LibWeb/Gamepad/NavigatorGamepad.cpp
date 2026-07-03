/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/TypeCasts.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/Gamepad/EventNames.h>
#include <LibWeb/Gamepad/Gamepad.h>
#include <LibWeb/Gamepad/GamepadEvent.h>
#include <LibWeb/Gamepad/GamepadRegistry.h>
#include <LibWeb/Gamepad/NavigatorGamepad.h>
#include <LibWeb/HTML/Navigator.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HighResolutionTime/TimeOrigin.h>
#include <LibWeb/Page/Page.h>

namespace Web::Gamepad {

// https://w3c.github.io/gamepad/#dom-navigator-getgamepads
WebIDL::ExceptionOr<GC::RootVector<GC::Ptr<Gamepad>>> NavigatorGamepadPartial::get_gamepads()
{
    auto& navigator = as<HTML::Navigator>(*this);
    auto& realm = navigator.realm();

    // 1. Let doc be the current global object's associated Document.
    auto& window = as<HTML::Window>(HTML::current_global_object());
    auto& document = window.associated_document();

    // 2. If doc is null or doc is not fully active, then return an empty list.
    GC::RootVector<GC::Ptr<Gamepad>> gamepads;
    if (!document.is_fully_active())
        return gamepads;

    // AD-HOC: Tell the UI process that this process consumes gamepad input. The UI process does not monitor gamepads
    //         or send gamepad state to a WebContent process until one of its pages uses the Gamepad API.
    window.page().client().page_did_start_using_gamepads();

    // 3. If doc is not allowed to use the "gamepad" permission, then throw a "SecurityError" DOMException and abort these steps.
    if (!document.is_allowed_to_use_feature(DOM::PolicyControlledFeature::Gamepad))
        return WebIDL::SecurityError::create(realm, "Not allowed to use gamepads"_utf16);

    // 4. If this.[[hasGamepadGesture]] is false, then return an empty list.
    if (!m_has_gamepad_gesture)
        return gamepads;

    // 5. Let now be the current high resolution time given the current global object.
    auto now = HighResolutionTime::current_high_resolution_time(window);

    // 6. Let gamepads be an empty list.
    // NOTE: Already done.

    // 7. For each gamepad of this.[[gamepads]]:
    for (auto gamepad : m_gamepads) {
        // 1. If gamepad is not null and gamepad.[[exposed]] is false:
        if (gamepad && !gamepad->exposed()) {
            // 1. Set gamepad.[[exposed]] to true.
            gamepad->set_exposed({}, true);

            // 2. Set gamepad.[[timestamp]] to now.
            gamepad->set_timestamp({}, now);
        }

        // 2. Append gamepad to gamepads.
        gamepads.append(gamepad);
    }

    // 8. Return gamepads.
    return gamepads;
}

void NavigatorGamepadPartial::visit_edges(GC::Cell::Visitor& visitor)
{
    visitor.visit(m_gamepads);
}

// https://w3c.github.io/gamepad/#dfn-selecting-an-unused-gamepad-index
size_t NavigatorGamepadPartial::select_an_unused_gamepad_index(Badge<Gamepad>)
{
    // 2. Let maxGamepadIndex be the size of navigator.[[gamepads]] − 1.
    // 3. For each gamepadIndex of the range from 0 to maxGamepadIndex:
    for (size_t gamepad_index = 0; gamepad_index < m_gamepads.size(); ++gamepad_index) {
        // 1. If navigator.[[gamepads]][gamepadIndex] is null, then return gamepadIndex.
        if (!m_gamepads[gamepad_index])
            return gamepad_index;
    }

    // 4. Append null to navigator.[[gamepads]].
    m_gamepads.append(nullptr);

    // 5. Return the size of navigator.[[gamepads]] − 1.
    return m_gamepads.size() - 1;
}

// https://w3c.github.io/gamepad/#event-gamepadconnected
void NavigatorGamepadPartial::handle_gamepad_connected(GamepadDescription const& description)
{
    // When a gamepad becomes available on the system, run the following steps:
    if (m_available_gamepads.contains_slow(description.handle))
        return;

    // 1. Let document be the current global object's associated Document; otherwise null.
    // FIXME: We can't use the current global object here, since it's not executing in a scripting context.
    // NOTE: NavigatorGamepad is only available on Window.
    // NOTE: document is never null.
    auto& navigator = as<HTML::Navigator>(*this);
    auto& realm = navigator.realm();
    auto& window = as<HTML::Window>(HTML::relevant_global_object(navigator));
    auto& document = window.associated_document();

    // 2. If document is not null and is not allowed to use the "gamepad" permission, then abort these steps.
    if (!document.is_allowed_to_use_feature(DOM::PolicyControlledFeature::Gamepad))
        return;

    // AD-HOC: In test mode, ignore any non-virtual gamepads.
    //         All fake gamepads added by Internals are always virtual, and no other ones are.
    if (HTML::Window::in_test_mode() && !description.is_virtual)
        return;

    m_available_gamepads.append(description.handle);

    // 3. Queue a global task on the gamepad task source with the current global object to perform the following steps:
    HTML::queue_global_task(HTML::Task::Source::Gamepad, window, GC::create_function(realm.heap(), [&realm, &document, description = description] mutable {
        // 1. Let gamepad be a new Gamepad representing the gamepad.
        auto gamepad = Gamepad::create(realm, description);

        // 2. Let navigator be gamepad's relevant global object's Navigator object.
        auto& gamepad_window = as<HTML::Window>(HTML::relevant_global_object(gamepad));
        auto navigator = gamepad_window.navigator();

        // 3. Set navigator.[[gamepads]][gamepad.index] to gamepad.
        navigator->m_gamepads[gamepad->index()] = gamepad;

        // 4. If navigator.[[hasGamepadGesture]] is true:
        if (navigator->m_has_gamepad_gesture) {
            // 1. Set gamepad.[[exposed]] to true.
            gamepad->set_exposed({}, true);

            // 2. If document is not null and is fully active, then fire an event named gamepadconnected at gamepad's
            //    relevant global object using GamepadEvent with its gamepad attribute initialized to gamepad.
            if (document.is_fully_active()) {
                auto gamepad_connected_event_init = Bindings::GamepadEventInit {
                    {
                        .bubbles = false,
                        .cancelable = false,
                        .composed = false,
                    },
                    gamepad,
                };
                auto gamepad_connected_event = MUST(GamepadEvent::construct_impl(realm, EventNames::gamepadconnected, gamepad_connected_event_init));
                gamepad_window.dispatch_event(gamepad_connected_event);
            }
        }
    }));
}

// https://w3c.github.io/gamepad/#dfn-receives-new-button-or-axis-input-values
void NavigatorGamepadPartial::handle_gamepad_updated(Badge<EventHandler>, GamepadState const& state)
{
    // When the system receives new button or axis input values, run the following steps:
    // 1. Let gamepad be the Gamepad object representing the device that received new button or axis input values.
    auto gamepad = m_gamepads.find_if([&state](GC::Ptr<Gamepad> gamepad) {
        return gamepad && gamepad->handle() == state.handle;
    });

    if (gamepad.is_end())
        return;

    (*gamepad)->set_latest_state({}, state);

    // 2. Queue a global task on the gamepad task source with gamepad's relevant global object to update gamepad state
    //    for gamepad.
    auto& global = HTML::relevant_global_object(**gamepad);
    HTML::queue_global_task(HTML::Task::Source::Gamepad, global, GC::create_function(global.heap(), [gamepad = GC::Ref { **gamepad }] {
        gamepad->update_gamepad_state({});
    }));
}

void NavigatorGamepadPartial::handle_gamepad_disconnected(Badge<EventHandler>, GamepadHandle handle)
{
    // When a gamepad becomes unavailable on the system, run the following steps:
    m_available_gamepads.remove_first_matching([&handle](GamepadHandle available_gamepad) {
        return handle == available_gamepad;
    });

    // 1. Let gamepad be the Gamepad representing the unavailable device.
    auto gamepad = m_gamepads.find_if([&handle](GC::Ptr<Gamepad> gamepad) {
        return gamepad && gamepad->handle() == handle;
    });

    if (gamepad.is_end())
        return;

    // 2. Queue a global task on the gamepad task source with gamepad's relevant global object to perform the
    //    following steps:
    auto& window = as<HTML::Window>(HTML::relevant_global_object(**gamepad));
    HTML::queue_global_task(HTML::Task::Source::Gamepad, window, GC::create_function(window.heap(), [gamepad = GC::Ref { **gamepad }, &window] {
        // 1. Set gamepad.[[connected]] to false.
        gamepad->set_connected({}, false);

        // 2. Let document be gamepad's relevant global object's associated Document; otherwise null.
        auto& document = window.associated_document();

        // 3. If gamepad.[[exposed]] is true and document is not null and is fully active, then fire an event named
        //    gamepaddisconnected at gamepad's relevant global object using GamepadEvent with its gamepad attribute
        //    initialized to gamepad.
        if (gamepad->exposed() && document.is_fully_active()) {
            auto gamepad_disconnected_event_init = Bindings::GamepadEventInit {
                {
                    .bubbles = false,
                    .cancelable = false,
                    .composed = false,
                },
                gamepad,
            };
            auto gamepad_disconnected_event = MUST(GamepadEvent::construct_impl(window.realm(), EventNames::gamepaddisconnected, gamepad_disconnected_event_init));
            window.dispatch_event(gamepad_disconnected_event);
        }

        // 4. Let navigator be gamepad's relevant global object's Navigator object.
        auto navigator = window.navigator();

        // 5. Set navigator.[[gamepads]][gamepad.index] to null.
        navigator->m_gamepads[gamepad->index()] = nullptr;

        // 6. While navigator.[[gamepads]] is not empty and the last item of navigator.[[gamepads]] is null, remove the
        //    last item of navigator.[[gamepads]].
        while (!navigator->m_gamepads.is_empty() && navigator->m_gamepads.last() == nullptr) {
            (void)navigator->m_gamepads.take_last();
        }
    }));
}

void NavigatorGamepadPartial::check_for_connected_gamepads()
{
    // The UI process informs each WebContent process about connected gamepads, and the process-wide registry
    // remembers them. This lets documents created after a gamepad connected (for example, a new iframe) pick it up.
    GamepadRegistry::the().for_each_connected_gamepad([this](GamepadDescription const& description) {
        handle_gamepad_connected(description);
    });
}

void NavigatorGamepadPartial::set_has_gamepad_gesture(Badge<Gamepad>, bool value)
{
    m_has_gamepad_gesture = value;
}

GC::RootVector<GC::Ptr<Gamepad>> NavigatorGamepadPartial::gamepads(Badge<Gamepad>) const
{
    return GC::RootVector<GC::Ptr<Gamepad>> { as<HTML::Navigator>(*this).m_gamepads };
}

}
