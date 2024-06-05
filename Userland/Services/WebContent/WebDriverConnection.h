/*
 * Copyright (c) 2022, Florent Castelli <florent.castelli@gmail.com>
 * Copyright (c) 2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022-2023, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/String.h>
#include <LibGfx/Rect.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibJS/Forward.h>
#include <LibJS/Heap/MarkedVector.h>
#include <LibWeb/Forward.h>
#include <LibWeb/UIEvents/KeyCode.h>
#include <LibWeb/WebDriver/ElementLocationStrategies.h>
#include <LibWeb/WebDriver/Response.h>
#include <LibWeb/WebDriver/TimeoutsConfiguration.h>
#include <WebContent/Forward.h>
#include <WebContent/WebDriverClientEndpoint.h>
#include <WebContent/WebDriverServerEndpoint.h>

namespace WebContent {

// https://w3c.github.io/webdriver/#dfn-global-key-state
struct GlobalKeyState {
    Vector<Web::UIEvents::KeyCode> pressed {};
    bool alt_key { false };
    bool ctrl_key { false };
    bool meta_key { false };
    bool shift_key { false };
};

// https://www.w3.org/TR/webdriver/#dfn-actions-options
struct ActionsOptions {
    Function<bool(JsonValue const&)> is_element_origin;
    Function<ErrorOr<Web::DOM::Element*, Web::WebDriver::Error>(JsonValue const&)> get_element_origin;
};

enum class ActionType {
    None,
    Pointer,
    Key,
    Wheel
};

enum class ActionSubtype {
    None,
    Pause,
    PointerDown,
    PointerUp,
    PointerMove,
    PointerCancel,
    KeyDown,
    KeyUp,
    Scroll
};

// https://w3c.github.io/webdriver/#dfn-action-object
struct ActionObject {
    ActionObject(String id, ActionType type, ActionSubtype subtype)
        : id(move(id))
        , type(type)
        , subtype(subtype)
    {
    }

    virtual ~ActionObject() = default;

    String id;
    ActionType type;
    ActionSubtype subtype;
    // FIXME: This shouldn't be necessary on all ActionObjects
    Optional<u32> duration {};
};

enum class PointerType {
    Mouse
};

enum class PointerSubtype {
    Mouse,
    Pen,
    Touch,
};

struct PointerActionObject : ActionObject {
    PointerActionObject(String id, ActionSubtype subtype)
        : ActionObject(move(id), ActionType::Pointer, subtype)
    {
    }

    PointerActionObject(ActionObject& action_object)
        : ActionObject(action_object)
    {
    }

    PointerType pointer_type { PointerType::Mouse };
    u32 button { 0 };
};

struct PointerUpActionObject : PointerActionObject {
    PointerUpActionObject(String id)
        : PointerActionObject(move(id), ActionSubtype::PointerUp)
    {
    }

    PointerUpActionObject(ActionObject& action_object)
        : PointerActionObject(action_object)
    {
    }
};

struct PointerDownActionObject : PointerActionObject {
    PointerDownActionObject(String id)
        : PointerActionObject(move(id), ActionSubtype::PointerDown)
    {
    }

    u32 width { 0 };
    u32 height { 0 };
    u32 pressure { 0 };
    u32 tangential_pressure { 0 };
    u32 tilt_x { 0 };
    u32 tilt_y { 0 };
    u32 twist { 0 };
    u32 altitude_angle { 0 };
    u32 azimuth_angle { 0 };
};

struct PointerMoveActionObject : ActionObject {
    PointerMoveActionObject(String id)
        : ActionObject(move(id), ActionType::Pointer, ActionSubtype::PointerMove)
    {
    }

    u32 x { 0 };
    u32 y { 0 };
    Web::DOM::Element* origin { nullptr };
    u32 width { 0 };
    u32 height { 0 };
    u32 pressure { 0 };
    u32 tangential_pressure { 0 };
    u32 tilt_x { 0 };
    u32 tilt_y { 0 };
    u32 twist { 0 };
    u32 altitude_angle { 0 };
    u32 azimuth_angle { 0 };
};

// https://w3c.github.io/webdriver/#dfn-input-source
struct InputSource {
    String input_id;

    virtual ~InputSource() = default;
};

// https://www.w3.org/TR/webdriver/#dfn-null-input-source
struct NullInputSource : InputSource {
    void pause(int tick_duration);
};

// https://www.w3.org/TR/webdriver/#dfn-key-input-source
struct KeyInputSource : NullInputSource {
    Vector<Web::UIEvents::KeyCode> pressed {};
    bool alt { false };
    bool ctrl { false };
    bool meta { false };
    bool shift { false };

    void key_down(Web::UIEvents::KeyCode);
    void key_up(Web::UIEvents::KeyCode);
};

// https://www.w3.org/TR/webdriver/#pointer-input-source
struct PointerInputSource : NullInputSource {
    PointerSubtype subtype;
    u32 pointer_id { 0 };
    Vector<u32> pressed {};
    u32 x { 0 };
    u32 y { 0 };

    void pointer_down();
    void pointer_up();
    void pointer_move();
    void pointer_cancel();
};

// https://w3c.github.io/webdriver/#dfn-input-state
struct InputState {
    HashMap<String, InputSource> input_state_map;
    Vector<ActionObject> input_cancel_list;
    Vector<u32> actions_queue;
};

class WebDriverConnection final
    : public IPC::ConnectionToServer<WebDriverClientEndpoint, WebDriverServerEndpoint> {
    C_OBJECT_ABSTRACT(WebDriverConnection)

public:
    static ErrorOr<NonnullRefPtr<WebDriverConnection>> connect(Web::PageClient& page_client, ByteString const& webdriver_ipc_path);
    virtual ~WebDriverConnection() = default;

private:
    WebDriverConnection(NonnullOwnPtr<Core::LocalSocket> socket, Web::PageClient& page_client);

    virtual void die() override { }

    virtual void close_session() override;
    virtual void set_page_load_strategy(Web::WebDriver::PageLoadStrategy const& page_load_strategy) override;
    virtual void set_unhandled_prompt_behavior(Web::WebDriver::UnhandledPromptBehavior const& unhandled_prompt_behavior) override;
    virtual void set_strict_file_interactability(bool strict_file_interactability) override;
    virtual void set_is_webdriver_active(bool) override;
    virtual Messages::WebDriverClient::GetTimeoutsResponse get_timeouts() override;
    virtual Messages::WebDriverClient::SetTimeoutsResponse set_timeouts(JsonValue const& payload) override;
    virtual Messages::WebDriverClient::NavigateToResponse navigate_to(JsonValue const& payload) override;
    virtual Messages::WebDriverClient::GetCurrentUrlResponse get_current_url() override;
    virtual Messages::WebDriverClient::BackResponse back() override;
    virtual Messages::WebDriverClient::ForwardResponse forward() override;
    virtual Messages::WebDriverClient::RefreshResponse refresh() override;
    virtual Messages::WebDriverClient::GetTitleResponse get_title() override;
    virtual Messages::WebDriverClient::GetWindowHandleResponse get_window_handle() override;
    virtual Messages::WebDriverClient::CloseWindowResponse close_window() override;
    virtual Messages::WebDriverClient::SwitchToWindowResponse switch_to_window() override;
    virtual Messages::WebDriverClient::NewWindowResponse new_window(JsonValue const& payload) override;
    virtual Messages::WebDriverClient::GetWindowRectResponse get_window_rect() override;
    virtual Messages::WebDriverClient::SetWindowRectResponse set_window_rect(JsonValue const& payload) override;
    virtual Messages::WebDriverClient::MaximizeWindowResponse maximize_window() override;
    virtual Messages::WebDriverClient::MinimizeWindowResponse minimize_window() override;
    virtual Messages::WebDriverClient::FullscreenWindowResponse fullscreen_window() override;
    virtual Messages::WebDriverClient::ConsumeUserActivationResponse consume_user_activation() override;
    virtual Messages::WebDriverClient::FindElementResponse find_element(JsonValue const& payload) override;
    virtual Messages::WebDriverClient::FindElementsResponse find_elements(JsonValue const& payload) override;
    virtual Messages::WebDriverClient::FindElementFromElementResponse find_element_from_element(JsonValue const& payload, String const& element_id) override;
    virtual Messages::WebDriverClient::FindElementsFromElementResponse find_elements_from_element(JsonValue const& payload, String const& element_id) override;
    virtual Messages::WebDriverClient::FindElementFromShadowRootResponse find_element_from_shadow_root(JsonValue const& payload, String const& shadow_id) override;
    virtual Messages::WebDriverClient::FindElementsFromShadowRootResponse find_elements_from_shadow_root(JsonValue const& payload, String const& shadow_id) override;
    virtual Messages::WebDriverClient::GetActiveElementResponse get_active_element() override;
    virtual Messages::WebDriverClient::GetElementShadowRootResponse get_element_shadow_root(String const& element_id) override;
    virtual Messages::WebDriverClient::IsElementSelectedResponse is_element_selected(String const& element_id) override;
    virtual Messages::WebDriverClient::GetElementAttributeResponse get_element_attribute(String const& element_id, String const& name) override;
    virtual Messages::WebDriverClient::GetElementPropertyResponse get_element_property(String const& element_id, String const& name) override;
    virtual Messages::WebDriverClient::GetElementCssValueResponse get_element_css_value(String const& element_id, String const& name) override;
    virtual Messages::WebDriverClient::GetElementTextResponse get_element_text(String const& element_id) override;
    virtual Messages::WebDriverClient::GetElementTagNameResponse get_element_tag_name(String const& element_id) override;
    virtual Messages::WebDriverClient::GetElementRectResponse get_element_rect(String const& element_id) override;
    virtual Messages::WebDriverClient::IsElementEnabledResponse is_element_enabled(String const& element_id) override;
    virtual Messages::WebDriverClient::GetComputedRoleResponse get_computed_role(String const& element_id) override;
    virtual Messages::WebDriverClient::GetComputedLabelResponse get_computed_label(String const& element_id) override;
    virtual Messages::WebDriverClient::ElementClickResponse element_click(String const& element_id) override;
    virtual Messages::WebDriverClient::GetSourceResponse get_source() override;
    virtual Messages::WebDriverClient::ExecuteScriptResponse execute_script(JsonValue const& payload) override;
    virtual Messages::WebDriverClient::ExecuteAsyncScriptResponse execute_async_script(JsonValue const& payload) override;
    virtual Messages::WebDriverClient::GetAllCookiesResponse get_all_cookies() override;
    virtual Messages::WebDriverClient::GetNamedCookieResponse get_named_cookie(String const& name) override;
    virtual Messages::WebDriverClient::AddCookieResponse add_cookie(JsonValue const& payload) override;
    virtual Messages::WebDriverClient::DeleteCookieResponse delete_cookie(String const& name) override;
    virtual Messages::WebDriverClient::DeleteAllCookiesResponse delete_all_cookies() override;
    virtual Messages::WebDriverClient::PerformActionsResponse perform_actions(JsonValue const& payload) override;
    virtual Messages::WebDriverClient::ReleaseActionsResponse release_actions() override;
    virtual Messages::WebDriverClient::DismissAlertResponse dismiss_alert() override;
    virtual Messages::WebDriverClient::AcceptAlertResponse accept_alert() override;
    virtual Messages::WebDriverClient::GetAlertTextResponse get_alert_text() override;
    virtual Messages::WebDriverClient::SendAlertTextResponse send_alert_text(JsonValue const& payload) override;
    virtual Messages::WebDriverClient::TakeScreenshotResponse take_screenshot() override;
    virtual Messages::WebDriverClient::TakeElementScreenshotResponse take_element_screenshot(String const& element_id) override;
    virtual Messages::WebDriverClient::PrintPageResponse print_page() override;

    virtual Messages::WebDriverClient::EnsureTopLevelBrowsingContextIsOpenResponse ensure_top_level_browsing_context_is_open() override;
    ErrorOr<void, Web::WebDriver::Error> ensure_open_top_level_browsing_context();

    ErrorOr<void, Web::WebDriver::Error> handle_any_user_prompts();
    void restore_the_window();
    Gfx::IntRect maximize_the_window();
    Gfx::IntRect iconify_the_window();

    ErrorOr<void, Web::WebDriver::Error> wait_for_navigation_to_complete();

    using StartNodeGetter = Function<ErrorOr<Web::DOM::ParentNode*, Web::WebDriver::Error>()>;
    ErrorOr<JsonArray, Web::WebDriver::Error> find(StartNodeGetter&& start_node_getter, Web::WebDriver::LocationStrategy using_, StringView value);

    struct ScriptArguments {
        ByteString script;
        JS::MarkedVector<JS::Value> arguments;
    };
    ErrorOr<ScriptArguments, Web::WebDriver::Error> extract_the_script_arguments_from_a_request(JsonValue const& payload);
    void delete_cookies(Optional<StringView> const& name = {});

    InputState& get_the_input_state(Web::HTML::BrowsingContext&);

    JS::NonnullGCPtr<Web::PageClient> m_page_client;

    // https://w3c.github.io/webdriver/#dfn-page-load-strategy
    Web::WebDriver::PageLoadStrategy m_page_load_strategy { Web::WebDriver::PageLoadStrategy::Normal };

    // https://w3c.github.io/webdriver/#dfn-unhandled-prompt-behavior
    Web::WebDriver::UnhandledPromptBehavior m_unhandled_prompt_behavior { Web::WebDriver::UnhandledPromptBehavior::DismissAndNotify };

    // https://w3c.github.io/webdriver/#dfn-strict-file-interactability
    bool m_strict_file_interactability { false };

    // https://w3c.github.io/webdriver/#dfn-session-script-timeout
    Web::WebDriver::TimeoutsConfiguration m_timeouts_configuration;

    // https://w3c.github.io/webdriver/#dfn-browsing-context-input-state-map
    HashMap<JS::RawGCPtr<Web::HTML::BrowsingContext>, InputState> m_browsing_context_input_state_map;
};

}
