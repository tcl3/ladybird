/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Clients, https://w3c.github.io/ServiceWorker/#clients-interface

use libjs_rust::api::Trace;

use crate::bindings::interface::Wrappable;

// https://w3c.github.io/ServiceWorker/#enumdef-clienttype
/// In the order of Web::Bindings::ClientType, which C++ passes as its underlying value.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum ClientType {
    Window,
    Worker,
    Sharedworker,
    All,
}

impl ClientType {
    pub(crate) fn from_u8(value: u8) -> Option<Self> {
        match value {
            0 => Some(Self::Window),
            1 => Some(Self::Worker),
            2 => Some(Self::Sharedworker),
            3 => Some(Self::All),
            _ => None,
        }
    }
}

// https://w3c.github.io/ServiceWorker/#clients-interface
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Clients {}

impl Wrappable for Clients {}
