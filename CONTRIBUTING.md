# Contributing to Haul

Thanks for helping. Haul is also a teaching project, so readable code matters as much as clever code.

## Workflow

* `main` is the stable baseline. Never commit to it directly.
* Shared code (engine, UI, tests, docs) goes to `feature/shared-core`.
* Linux-only packaging goes to `feature/linux-desktop`; Windows-only packaging and CI go to `feature/windows-desktop`. Shared-core changes are **merged** into both platform branches; they are never duplicated.
* Use small commits with conventional messages: `feat(network): …`, `fix(ui): …`, `test(queue): …`, `docs: …`, `build(linux): …`.

## Before opening a pull request

```bash
cmake --preset linux-debug && cmake --build --preset linux-debug && ctest --preset linux-debug
```

* The build must have no warnings.
* New behaviour needs a test. Network behaviour is tested against `tests/server/test_server.py`, never the real Internet.
* Never weaken TLS verification, the protocol allow-list or file-name sanitisation (see `docs/SECURITY.md`).
* Do not add code, text or artwork taken from proprietary download managers (see `docs/FEATURE_RESEARCH.md`).

## Code style

* C++17, Qt 6. Match the surrounding code: 4-space indent, `m_` members, `camelCase` functions.
* Keep the engine (`src/core`, `network`, `scheduler`, `storage`, `app`) free of widget code.
* Worker threads communicate only through signals; never touch a widget off the GUI thread.
* Comments explain *why*, not *what*.
