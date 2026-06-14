# dfps tests

## Config validator regression test (host-runnable)

`test_config_parser.cpp` guards the config hot-reload fix.

**The bug:** on a config write, the daemon (`source/main.cpp`) used to `KillOldApp()` and
only then start a replacement that validated the config in `DynamicFps`'s constructor. A bad
config — missing default (`*`) or offscreen (`-`) rule, an inconsistent rule, or a parse error —
made the replacement exit immediately, so the running instance was gone and nothing replaced it:
the whole dynamic-refresh-rate service went down.

**The fix:** the validation logic now lives in `source/modules/config_parser.h/.cpp`
(`ConfigParser`), which depends only on the C++ standard library. The daemon calls
`ConfigParser::Validate()` **before** touching the running instance and, on any error, logs a
warning and keeps the current instance alive (rollback / keep-alive). `DynamicFps` reuses the
same `ConfigParser`, so runtime parsing and the reload gate can never drift apart.

Because `ConfigParser` is platform-free, the predicate the daemon branches on is unit-testable on
any host — no Android NDK, no spdlog, no device.

### Run

```sh
bash tests/run.sh
```

This compiles `tests/test_config_parser.cpp` + `source/modules/config_parser.cpp` with the host
compiler under `-fsanitize=address` and runs the suite. Exit code is non-zero if any case fails.
The over-long-line cases rely on ASAN to catch a buffer-overflow regression in the token parsing.

## On-device integration check (manual)

The full daemon is Android-only (cross-compiled via `build.sh` with the NDK) and cannot run on a
desktop host, so the kill-vs-keep wiring is verified on a device:

1. Build & install: `./build.sh Release "make pack install"` (requires `ANDROID_NDK` + `adb`).
2. Tail logs: `adb shell su -c logcat -s dfps` (or watch the configured `-o` log file).
3. With dfps running, push a **broken** config (e.g. delete the `*` line) to the watched path.
   Expected: a `New config rejected (...), keep current dfps running` warning, and the existing
   dfps process is **still alive** (`adb shell pgrep -f dfps` unchanged) — the refresh rate keeps
   working.
4. Push a **valid** config. Expected: `New config is valid, reload dfps`, and dfps restarts.

Before the fix, step 3 killed dfps and left nothing running.
