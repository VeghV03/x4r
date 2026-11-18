# x4r

The Xteink X4 as a newspaper and a book: CrossPoint Reader's firmware, forked, plus a Mac-side
companion. What the device does is in README.md; the contracts are docs/DATA.md (files on the SD
card and in `~/x4`) and docs/PROTOCOL.md (the USB protocol).

## Layout

- `firmware/` CrossPoint, vendored with `git subtree`. `firmware/freeink-sdk/` the same way.
- `companion/` Python package: the `x4` CLI, the MCP server, the dashboard, converters.
- `.claude/skills/x4-sync/` the skill Claude runs to write an issue and sync the device.
- `backups/` flash dumps. Never committed, never deleted.

## Rules

- Design: one purpose, no taglines, helper text or secondary labels. If a screen needs them, fix the layout.
- `firmware/AGENTS.md` and `firmware/.agents/` apply for technical guidance (heap discipline, HAL
  boundaries, coding style, build steps).
- Never flash the X4 without a verified backup in `backups/` (`x4 backup --out backups`).
- The `x4` tool never calls a language model. Claude Code does the writing through `/x4-sync`.

## Build and flash

```bash
firmware/.venv/bin/pio run -d firmware -e x4r
```

```bash
firmware/.venv/bin/pio run -d firmware -e x4r -t upload
```

The first install must go through `-t upload` (or the factory image at 0x0) so the bootloader with
app rollback is on the device; after that `x4 flash` installs new images. Host tests (CMake comes
with PlatformIO):

```bash
CMAKE=~/.platformio/packages/tool-cmake/bin/cmake
$CMAKE -S firmware/test -B build/test && $CMAKE --build build/test --target LearnTest x4play x4paper && build/test/learn/LearnTest
```

Companion tests: `uv run --directory companion pytest`.

## Verify on the simulator

`x4 sim <dir> --play` runs the paper's key logic (`x4play`) and the firmware's drawing (`x4paper`)
on the Mac. Check behaviour there, not by pressing buttons on someone's device: presses write
reading records into their log.
