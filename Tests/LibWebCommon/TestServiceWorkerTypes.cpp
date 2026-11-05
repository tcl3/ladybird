/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/MemoryStream.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibTest/TestCase.h>
#include <LibWebCommon/ServiceWorker/CacheStorageTypes.h>
#include <LibWebCommon/ServiceWorker/ServiceWorkerTypes.h>

namespace Bindings = Web::Bindings;
namespace ServiceWorker = Web::ServiceWorker;

template<typename... Values>
static IPC::MessageBuffer encode_values(Values const&... values)
{
    IPC::MessageBuffer message;
    IPC::Encoder encoder { message };
    auto encode = [&](auto const& value) { MUST(encoder.encode(value)); };
    (encode(values), ...);
    return message;
}

template<typename T>
static ErrorOr<T> decode_message(IPC::MessageBuffer& message)
{
    Queue<IPC::Attachment> attachments;
    FixedMemoryStream stream { message.data().span() };
    IPC::Decoder decoder { stream, attachments };
    return decoder.decode<T>();
}

// Each value that an enum declares survives IPC, and a value it does not declare is refused.
template<typename Enum, size_t value_count>
static void expect_only_declared_values_decode(Array<Enum, value_count> const& values)
{
    for (auto value : values) {
        auto message = encode_values(value);
        EXPECT_EQ(MUST(decode_message<Enum>(message)), value);
    }
    for (u8 value : { static_cast<u8>(value_count), static_cast<u8>(255) }) {
        auto message = encode_values(value);
        EXPECT(decode_message<Enum>(message).is_error());
    }
}

TEST_CASE(binding_enums_refuse_undeclared_values)
{
    expect_only_declared_values_decode(Array { Bindings::ClientType::Window, Bindings::ClientType::Worker, Bindings::ClientType::Sharedworker, Bindings::ClientType::All });
    expect_only_declared_values_decode(Array { Bindings::FrameType::Auxiliary, Bindings::FrameType::TopLevel, Bindings::FrameType::Nested, Bindings::FrameType::None });
    expect_only_declared_values_decode(Array { Bindings::DocumentVisibilityState::Visible, Bindings::DocumentVisibilityState::Hidden });
    expect_only_declared_values_decode(Array { Bindings::ServiceWorkerState::Parsed, Bindings::ServiceWorkerState::Installing, Bindings::ServiceWorkerState::Installed, Bindings::ServiceWorkerState::Activating, Bindings::ServiceWorkerState::Activated, Bindings::ServiceWorkerState::Redundant });
    expect_only_declared_values_decode(Array { Bindings::ServiceWorkerUpdateViaCache::Imports, Bindings::ServiceWorkerUpdateViaCache::All, Bindings::ServiceWorkerUpdateViaCache::None });
}

TEST_CASE(service_worker_enums_refuse_undeclared_values)
{
    expect_only_declared_values_decode(Array { ServiceWorker::ServiceWorkerJobType::Register, ServiceWorker::ServiceWorkerJobType::Update, ServiceWorker::ServiceWorkerJobType::Unregister });
    expect_only_declared_values_decode(Array { ServiceWorker::ServiceWorkerJobError::TypeError, ServiceWorker::ServiceWorkerJobError::SecurityError });
    expect_only_declared_values_decode(Array { ServiceWorker::ServiceWorkerRegistrationSlot::Installing, ServiceWorker::ServiceWorkerRegistrationSlot::Waiting, ServiceWorker::ServiceWorkerRegistrationSlot::Active });
    expect_only_declared_values_decode(Array { ServiceWorker::ServiceWorkerLifecycleEvent::Install, ServiceWorker::ServiceWorkerLifecycleEvent::Activate });
    expect_only_declared_values_decode(Array { ServiceWorker::ServiceWorkerNavigationPreloadOperation::Enable, ServiceWorker::ServiceWorkerNavigationPreloadOperation::Disable, ServiceWorker::ServiceWorkerNavigationPreloadOperation::SetHeaderValue, ServiceWorker::ServiceWorkerNavigationPreloadOperation::GetState });
}

TEST_CASE(cache_storage_enums_refuse_undeclared_values)
{
    expect_only_declared_values_decode(Array { ServiceWorker::CacheBatchOperationType::Delete, ServiceWorker::CacheBatchOperationType::Put });
    expect_only_declared_values_decode(Array { ServiceWorker::CacheStorageNameOperation::Has, ServiceWorker::CacheStorageNameOperation::Open, ServiceWorker::CacheStorageNameOperation::Delete, ServiceWorker::CacheStorageNameOperation::Keys });
    expect_only_declared_values_decode(Array { ServiceWorker::CacheQueryKind::MatchAll, ServiceWorker::CacheQueryKind::Keys });
    expect_only_declared_values_decode(Array { ServiceWorker::CacheStorageError::TypeError, ServiceWorker::CacheStorageError::InvalidStateError, ServiceWorker::CacheStorageError::SecurityError });
}

TEST_CASE(a_request_that_names_an_undeclared_operation_is_refused)
{
    auto valid_message = encode_values(ServiceWorker::CacheStorageNameOperation::Open, "cache"_string);
    auto request = MUST(decode_message<ServiceWorker::CacheStorageNameRequest>(valid_message));
    EXPECT_EQ(request.operation, ServiceWorker::CacheStorageNameOperation::Open);
    EXPECT_EQ(request.cache_name, "cache"sv);

    auto invalid_message = encode_values(static_cast<u8>(9), "cache"_string);
    EXPECT(decode_message<ServiceWorker::CacheStorageNameRequest>(invalid_message).is_error());
}
