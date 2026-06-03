# Contributing

Thanks for your interest in `hft_framework`. This project is small and
opinionated — read this once before opening your first PR.

## Build & test

```powershell
# Windows
cmake -S . -B build_vs2022 -G "Visual Studio 17 2022" -A x64
cmake --build build_vs2022 --config Release --target hft_framework
cmake --build build_vs2022 --config Release --target hft_tests
./dist/hft_tests.exe
```

```bash
# Linux
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j --target hft_framework hft_tests
./dist/hft_tests
```

Both targets must build clean and tests must pass before you open a PR.

## Scope

This is a **live-trading** framework. PRs that add the following are
out of scope and will be closed:

- Backtest engines or historical replay systems
- Desktop GUIs (Qt, ImGui, etc.)
- HTTP / WebSocket control panels
- Web dashboards or browser front-ends

If you want any of these, fork or build a separate companion project
that drives `hft_framework` via its public headers.

In-scope additions we welcome:

- Additional broker gateways (CTP-mini, REM, YD, OES, ...)
- Strategy templates / examples (especially in Python)
- Performance work on the consumer hot path
- Tests, especially integration tests against fake gateways
- Documentation, especially translations

## Code style

- C++17, must compile on MSVC (`/W3` is OK) and recent GCC/Clang.
- 4-space indentation, no tabs.
- `snake_case` for functions and variables, `PascalCase` for types,
  `kCamelCase` for constants.
- Header guards via `#pragma once`.
- Prefer `std::filesystem`, `std::chrono`, `std::optional`.
- **Do not** introduce exceptions in the hot path (`TradingEngine`
  consumer loop, gateway callbacks). Use return codes / `bool` + error
  strings.
- **Do not** add hot-path allocations in the order send path. Reuse
  fixed-size buffers.
- **Do not** add new mutexes on the consumer thread. If you need to
  share data with another thread, use an SPSC queue or atomics.

## Commit messages

Short imperative subject (under 70 chars), optional body explaining
the *why*. Reference an issue if applicable.

```
risk: cap MaxOrdersPerMinute at 1000

The previous unbounded value let mis-configured strategies hammer the
broker on startup. Closes #42.
```

## Before opening a PR

- [ ] `--target hft_framework` builds clean (Windows + Linux if you can)
- [ ] `--target hft_tests` builds and the binary returns 0
- [ ] No new `src/qt/`, `src/web/`, backtest, or GUI code
- [ ] No `config.ini`, `*.db`, `dist/`, or build artifacts staged
- [ ] No real broker credentials in commits or test data
- [ ] New public APIs documented in code comments or in `docs/`
- [ ] If touching hot-path code (`src/engine/`, `src/order/`, `src/position/`,
      `src/risk/`, `src/common/spsc_queue.h`), run `hft_bench` before and after
      and paste the p99 numbers in the PR description. Regressions > 20% will
      be flagged.

## Security

Found a vulnerability? Do **not** open a public issue. Email the
maintainer (see commit history for contact) with a description and
a reproducer. We aim to respond within 7 days.
