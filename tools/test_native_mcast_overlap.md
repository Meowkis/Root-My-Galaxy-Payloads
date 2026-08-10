# Native MCAST overlap test

AI disclosure: OpenAI Codex prepared this isolated test at `@johnny-salz`'s request.

Target: `SM-S918B`, `S918BXXSAFZF5`, kernel `5.15.189-android13-8-33413713-abS918BXXSAFZF5`.

Build:

```sh
ANDROID_NDK_HOME=/path/to/android-ndk make native-mcast-test
```

A ready-to-run binary is included at:

```text
tools/prebuilt/SM-S918B_FZF5/test-native-mcast-overlap
```

SHA-256:

```text
44ccb452e12dcbc99aea12173d4ebef3028b5e18b37c304b17537c5f1d6fdaa1
```

Safe syscall check:

```sh
./test-native-mcast-overlap --probe
```

Expected: `setsockopt` returns `-1` after copying the 264-byte native `group_source_req` and rejecting its invalid address family.

Destructive overlap test:

```sh
./test-native-mcast-overlap --trigger
```

This may intentionally panic the kernel. A positive stack-overlap result is:

```text
pc = rt_mutex_adjust_prio_chain+0x1ac
x27 = 0x4d434153544c4f43
```

`x27` is loaded from stale `waiter->lock`. The marker proves that the native arm64 MCAST copy placed the waiter tail at the calculated location. A different `x27` means the runtime stack placement did not match the static `buffer+0x68` calculation or the waiter was overwritten before the consumer ran.

This test does not run KernelSnitch, reclaim pages, replace fops, or attempt root.
