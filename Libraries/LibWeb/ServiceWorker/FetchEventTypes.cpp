/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/ImmutableBytes.h>
#include <LibHTTP/HeaderList.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Bodies.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/ResponseView.h>
#include <LibWeb/ServiceWorker/FetchEventTypes.h>

namespace Web::ServiceWorker {

NonnullRefPtr<Fetch::Infrastructure::Request> request_from(FetchEventRequest&& descriptor)
{
    auto request = Fetch::Infrastructure::Request::create();
    request->set_method(move(descriptor.method));
    request->set_url_list(move(descriptor.url_list));
    if (descriptor.origin.has_value())
        request->set_origin(descriptor.origin.release_value());
    request->set_header_list(HTTP::HeaderList::create(move(descriptor.header_list)));
    if (descriptor.body.has_value())
        request->set_body(Fetch::Infrastructure::FetchBody::from_bytes(Core::ImmutableBytes::adopt(descriptor.body.release_value())));
    request->set_destination(descriptor.destination);
    request->set_mode(descriptor.mode);
    request->set_credentials_mode(descriptor.credentials_mode);
    request->set_cache_mode(descriptor.cache_mode);
    request->set_redirect_mode(descriptor.redirect_mode);
    request->set_referrer(move(descriptor.referrer));
    request->set_referrer_policy(descriptor.referrer_policy);
    request->set_integrity_metadata(Utf16String::from_utf8(descriptor.integrity_metadata));
    request->set_keepalive(descriptor.keepalive);
    request->set_reload_navigation(descriptor.reload_navigation);
    request->set_history_navigation(descriptor.history_navigation);
    return request;
}

FetchEventResponse describe_response(Fetch::Infrastructure::ResponseView const& response)
{
    auto const& record = *response.record();
    auto const* filtered_response = as_if<Fetch::Infrastructure::FilteredResponse>(record);
    auto const& internal_response = filtered_response ? *filtered_response->internal_response() : record;
    return FetchEventResponse {
        .type = record.type(),
        .status = internal_response.status(),
        .status_message = internal_response.status_message(),
        .header_list = internal_response.header_list()->headers(),
        .url_list = internal_response.url_list(),
        .cors_exposed_header_name_list = internal_response.cors_exposed_header_name_list(),
        .range_requested = internal_response.range_requested(),
        .request_includes_credentials = internal_response.request_includes_credentials(),
        .timing_allow_passed = internal_response.timing_allow_passed(),
        .redirect_taint = internal_response.redirect_taint(),
        // NB: An opaque filtered response has no body, but the client reads the body of its internal response, as
        //     importScripts() does.
        .has_body = internal_response.body().has_value(),
    };
}

// https://w3c.github.io/ServiceWorker/#fetch-event-respondwith
// 10.2.4. Let potentialResponse be a copy of response's associated response, except for its body.
NonnullRefPtr<Fetch::Infrastructure::Response> internal_response_from(FetchEventResponse const& descriptor)
{
    auto internal_response = Fetch::Infrastructure::Response::create();
    internal_response->set_status(descriptor.status);
    internal_response->set_status_message(descriptor.status_message);
    internal_response->set_header_list(HTTP::HeaderList::create(descriptor.header_list));
    internal_response->set_url_list(descriptor.url_list);
    internal_response->set_cors_exposed_header_name_list(descriptor.cors_exposed_header_name_list);
    internal_response->set_range_requested(descriptor.range_requested);
    internal_response->set_request_includes_credentials(descriptor.request_includes_credentials);
    internal_response->set_timing_allow_passed(descriptor.timing_allow_passed);
    internal_response->set_redirect_taint(descriptor.redirect_taint);
    return internal_response;
}

NonnullRefPtr<Fetch::Infrastructure::Response> response_of_type(Fetch::Infrastructure::Response::Type type, NonnullRefPtr<Fetch::Infrastructure::Response> internal_response)
{
    using Type = Fetch::Infrastructure::Response::Type;
    switch (type) {
    case Type::Basic:
        return Fetch::Infrastructure::BasicFilteredResponse::create(move(internal_response));
    case Type::CORS:
        return Fetch::Infrastructure::CORSFilteredResponse::create(move(internal_response));
    case Type::Opaque:
        return Fetch::Infrastructure::OpaqueFilteredResponse::create(move(internal_response));
    case Type::OpaqueRedirect:
        return Fetch::Infrastructure::OpaqueRedirectFilteredResponse::create(move(internal_response));
    case Type::Default:
        return internal_response;
    case Type::Error:
        break;
    }
    VERIFY_NOT_REACHED();
}

}
