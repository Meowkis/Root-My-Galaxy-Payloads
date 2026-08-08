# RT-mutex RB-tree crash diagnostics

This note documents a safe debugging path for crashes that reach `rb_erase()` from
`rt_mutex_adjust_prio_chain()`.

## What the crash means

`rb_erase()` expects the supplied `struct rb_node` to be a valid member of the
RB-tree represented by the supplied root. A crash immediately after entering
`rb_erase()` is therefore consistent with an invalid, stale, or corrupted node
linkage.

A matching object address is not sufficient evidence of tree membership.

## Diagnostics

For a kernel under test, collect:

- the exact kernel commit and configuration;
- the complete backtrace from `rt_mutex_adjust_prio_chain()` into `rb_erase()`;
- the address of the waiter object and the address of its containing lock;
- the RB-node parent/left/right values immediately before the erase operation;
- whether the node is marked empty before the operation;
- the lock's waiter-tree root and any available RB-tree validation output.

Do not treat a synthetic object as a valid RB-tree member merely because its
address is accepted by the call site.

## Suggested validation

When modifying a kernel you control, add temporary assertions around the
normal rt-mutex tree lifecycle and verify that insertion and removal occur on
the same node exactly once. Enable the kernel's available debug facilities for
locking and RB trees where supported by the target kernel configuration.

The objective is to identify whether corruption occurs during object lifetime,
during tree insertion, or before removal. This document intentionally does not
specify fake-object layouts, exploit primitives, register manipulation, or
privilege-escalation steps.
