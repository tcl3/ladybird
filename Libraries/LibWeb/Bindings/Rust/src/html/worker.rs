/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! https://html.spec.whatwg.org/multipage/workers.html#workertype

/// In the order of Web::Bindings::WorkerType, which C++ passes as its underlying value.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum WorkerType {
    #[default]
    Classic,
    Module,
}
