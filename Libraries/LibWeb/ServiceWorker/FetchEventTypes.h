/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/ByteString.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Variant.h>
#include <AK/Vector.h>
#include <LibHTTP/Cache/CacheMode.h>
#include <LibHTTP/Header.h>
#include <LibURL/URL.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Requests.h>
#include <LibWeb/Fetch/Infrastructure/HTTP/Responses.h>
#include <LibWebCommon/Fetch/Infrastructure/HTTP/RequestReferrer.h>
#include <LibWebCommon/Fetch/Infrastructure/RedirectTaint.h>
#include <LibWebCommon/ReferrerPolicy/ReferrerPolicy.h>

namespace Web::ServiceWorker {

using FetchEventId = u64;

// https://w3c.github.io/ServiceWorker/#create-fetch-event-and-dispatch
// What a fetch event's request object is created from, in the service worker's realm.
struct FetchEventRequest {
    ByteString method;
    Vector<URL::URL> url_list;
    Vector<HTTP::Header> header_list;
    Optional<ByteBuffer> body;
    Optional<Fetch::Infrastructure::Request::Destination> destination;
    Fetch::Infrastructure::Request::Mode mode { Fetch::Infrastructure::Request::Mode::NoCORS };
    Fetch::Infrastructure::Request::CredentialsMode credentials_mode { Fetch::Infrastructure::Request::CredentialsMode::SameOrigin };
    HTTP::CacheMode cache_mode { HTTP::CacheMode::Default };
    Fetch::Infrastructure::Request::RedirectMode redirect_mode { Fetch::Infrastructure::Request::RedirectMode::Follow };
    Fetch::Infrastructure::RequestReferrerType referrer;
    ReferrerPolicy::ReferrerPolicy referrer_policy { ReferrerPolicy::ReferrerPolicy::EmptyString };
    String integrity_metadata;
    bool keepalive { false };
    bool reload_navigation { false };
    bool history_navigation { false };

    // https://w3c.github.io/ServiceWorker/#create-fetch-event-and-dispatch
    // 17.4.8. If client is not null, initialize e's clientId attribute to client's id.
    // NB: The channel names the reserved client, and so the client of a subresource request.
    Optional<String> client_id;

    // 17.4.10. If request is a navigation request, initialize e's replacesClientId attribute to request's replaces
    //          client id.
    String replaces_client_id;
};

// https://fetch.spec.whatwg.org/#concept-response
// A response that a service worker gave a fetch event, with its type and the fields of its internal response.
struct FetchEventResponse {
    Fetch::Infrastructure::Response::Type type { Fetch::Infrastructure::Response::Type::Default };
    u16 status { 200 };
    ByteString status_message;
    Vector<HTTP::Header> header_list;
    Vector<URL::URL> url_list;
    Vector<ByteString> cors_exposed_header_name_list;
    bool range_requested { false };
    bool request_includes_credentials { true };
    bool timing_allow_passed { false };
    Fetch::Infrastructure::RedirectTaint redirect_taint { Fetch::Infrastructure::RedirectTaint::SameOrigin };

    // NB: The body's bytes follow as the worker reads them.
    bool has_body { false };
};

// https://w3c.github.io/ServiceWorker/#create-fetch-event-and-dispatch
// What Create Fetch Event and Dispatch returns: null, for the request to go to the network, a network error, or a
// response.
enum class FetchEventOutcome : u8 {
    Fallback,
    NetworkError,
    Response,
};

}

#include <LibWeb/ServiceWorker/FetchEventTypesCodecs.h>
