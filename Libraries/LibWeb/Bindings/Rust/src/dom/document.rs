/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The parts of Document, which is implemented in C++, that the Rust interfaces use.

// https://html.spec.whatwg.org/multipage/interaction.html#visibility-state
/// In the order of Web::Bindings::DocumentVisibilityState, which C++ passes as its underlying value.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub(crate) enum DocumentVisibilityState {
    Visible,
    Hidden,
}

impl DocumentVisibilityState {
    pub(crate) fn from_u8(value: u8) -> Option<Self> {
        match value {
            0 => Some(Self::Visible),
            1 => Some(Self::Hidden),
            _ => None,
        }
    }
}
