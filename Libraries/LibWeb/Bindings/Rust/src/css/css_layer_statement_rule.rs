/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! CSSLayerStatementRule, https://drafts.csswg.org/css-cascade-5/#the-csslayerstatementrule-interface

use super::css_rule::CSSRule;
use super::css_rule::RuleText;
use super::css_rule::impl_rule_wrappable;
use crate::bindings::conversions::{ToJs, frozen_array_to_js, sequence_to_js};
use crate::bindings::interface::{Inherits, Ref, allocate};
use crate::bindings::{CallContext, WebIdlResult};
use ak::Utf16String;
use core::cell::Cell;
use libjs_rust::api::{Gc, Object, Trace, Vm};
use libweb_rust::NativeRule;
use std::rc::Rc;

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct CSSLayerStatementRule {
    rule: CSSRule,
    name_list: Cell<Option<Gc<Object>>>,
}

impl_rule_wrappable!(CSSLayerStatementRule);

impl Inherits<CSSRule> for CSSLayerStatementRule {
    fn parent(&self) -> &CSSRule {
        &self.rule
    }
}

impl CSSLayerStatementRule {
    pub(crate) fn create(vm: &Vm, native_rule: Rc<NativeRule>) -> Ref<Self> {
        allocate(vm, |_| Self {
            rule: CSSRule::new(native_rule),
            name_list: Cell::new(None),
        })
    }

    pub(crate) fn rule(&self) -> &CSSRule {
        &self.rule
    }

    // https://drafts.csswg.org/css-cascade-5/#dom-csslayerstatementrule-namelist
    pub(crate) fn name_list(&self, cx: &mut CallContext) -> WebIdlResult<Gc<Object>> {
        // NB: The layer names of a rule never change, so the frozen array of them is created once.
        if let Some(name_list) = self.name_list.get() {
            return Ok(name_list);
        }
        let names = self.rule.native_rule().layer_names();
        let name_list = frozen_array_to_js(cx, sequence_to_js(cx, names, |name| name.to_js(cx)))?.as_object();
        self.name_list.set(Some(name_list));
        Ok(name_list)
    }

    pub(crate) fn serialized(&self, _: &Vm) -> Utf16String {
        // AD-HOC: No spec yet.
        let mut text = RuleText::new("@layer ");
        for (index, name) in self.rule.native_rule().layer_names().iter().enumerate() {
            if index != 0 {
                text.push_str(", ");
            }
            text.push(name);
        }
        text.push_str(";");
        text.finish()
    }
}
