# Handoff — finish CSS Phase 6

You are picking up a half-finished change. Read this whole file before touching code.

## Situation

`docs/css-support-roadmap.md` Phases 1-5 are **done and committed**. Phase 6 (the
long tail) was started and abandoned mid-way when the agent doing it was killed
twice by server-side API errors. Its **parse side is committed** as `bd110d47`
(`wip:` prefix, deliberately). Nothing has been flashed or device-tested — not
Phase 6, and not Phases 1-5 either.

Read first, in this order:
1. `docs/css-support-roadmap.md` — the plan. Phases 1-5 each updated it; trust its current state.
2. `CLAUDE.md` — 380KB RAM hard ceiling, no exceptions/RTTI, `makeUniqueNoThrow` not bare `new`, `constexpr` tables, no `std::string` in hot paths, `std::string_view` is not null-terminated.
3. `git log --oneline -8` and `git show bd110d47` — what exists.

## What is already done (committed, `bd110d47`)

In `lib/Epub/Epub/css/CssStyle.h`:
- `enum class CssTextTransform : uint8_t { None, Uppercase, Lowercase, Capitalize }`
- One packed byte holding the transform (bits 0-1) and a `hyphens: none` bit
  (bit 2) — see `CSS_TEXT_TRANSFORM_MASK`, `CSS_HYPHENS_NONE_BIT`
- `CssLength letterSpacing`
- `defined` bits: `textTransform`, `hyphens`, `letterSpacing`

In `lib/Epub/Epub/css/CssParser.{h,cpp}`: the declaration branches, the
serialiser and all readers, framing constants updated, and
**`CSS_CACHE_VERSION` bumped 17 → 18**.

In `lib/Epub/Epub/blocks/BlockStyle.h` and `ReaderFontScale.{h,cpp}`: block-level
plumbing and a `cssLetterSpacingPx()` helper.

**It compiles (`-e default` verified) and is inert** — values are parsed, stored,
and never read by layout or rendering.

## What is NOT done

These three files are **untouched** and are where the remaining work lives:

| file | what is missing |
|---|---|
| `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp` | apply `text-transform` to text (the committed comment points at `flushPartWordBuffer`); per-block `hyphens: none` suppression |
| `lib/GfxRenderer/GfxRenderer.cpp` | the `letter-spacing` per-glyph advance, in **both** measurement and draw |
| `lib/Epub/Epub/Section.cpp` | `SECTION_FILE_VERSION` bump (62 → 63) — **only once laid-out output actually changes** |

Also not done: `max-width` (item 4, trivial — reuse the existing `width` path),
`clang-format`, the `-e private` build, and the host-side tests.

## Do this

### 1. `text-transform`
Apply at parse/layout time, not render time. **UTF-8 safety is mandatory**: the
text is UTF-8 and `std::toupper` on bytes corrupts multi-byte sequences. Restrict
the transform to ASCII `A-Z`/`a-z` and pass all bytes ≥ 0x80 through untouched,
or implement a correct minimal Latin mapping. State which you chose. **Do not
corrupt Japanese text** — verify with Japanese input in a host test.

### 2. `hyphens: none`
Map onto the existing hyphenation mechanism (`SETTINGS.hyphenationEnabled`,
`lib/Epub/Epub/hyphenation/Hyphenator.h`) as a per-block suppression. The user's
global setting must still be able to disable hyphenation entirely; the book may
only **suppress**, never force it on. Accept-and-ignore the
`hyphenate-limit-*` family (and `-moz-`/`-webkit-`/`-ms-` variants) so they don't
fall through as unknown.

### 3. `letter-spacing` — highest risk, skip if unsure
The layout measurement and the draw **must** use the same per-glyph delta, or
text overlaps or drifts. Relevant functions: `getTextAdvanceX`, `getSpaceAdvance`,
`getSpaceWidth`, `drawText` in `lib/GfxRenderer/GfxRenderer.cpp`; line layout in
`lib/Epub/Epub/ParsedText.cpp`.

> This exact class of bug — layout measuring with one font while drawing used
> another — cost hours of debugging on 2026-07-29. See the memory note
> `reader-font-substitution` and `effectiveReaderFontId()` in
> `src/activities/reader/EpubReaderActivity.cpp`.

**If you cannot prove they agree, do not implement it.** Remove the parse-side
`letterSpacing` field, drop `CSS_CACHE_VERSION` back appropriately, and say so.
8 declarations of heading tracking is not worth a drift bug.

### 4. `max-width`
Reuse the `width` path. Trivial.

### Explicitly do NOT implement
`float` (needs real block layout — out of scope per SCOPE.md), `border-radius`
(meaningless on 1-bit), `@font-face`/`font-family` (fonts must be pre-converted
`.cpfont`). `widows`/`orphans` only if genuinely cheap against the Phase 3
buffering paginator — otherwise skip and say so. **Do not destabilise pagination
for 6 declarations.**

## Invariants you must not break

1. **Framing constants.** `CSS_LEADING_ENUM_BYTES` / `CSS_LENGTH_FIELD_COUNT` /
   `CSS_TRAILING_ENUM_BYTES` / `RULE_FIXED_BYTES` are defined once at file scope
   in `CssParser.cpp` (~line 53) and shared by `writeRuleRecord`,
   `validateCache`, `loadFromCache` and `collectVerticalStyles`. They must match
   the serialiser exactly. **State the verified fixed-byte count in your report.**
   (It was 82 after Phase 5; Phase 6's parse side may have changed it — verify,
   don't assume.)
2. **Version bumps.** `CSS_CACHE_VERSION` when the rule record layout changes;
   `SECTION_FILE_VERSION` when laid-out output changes. Each needs a one-line
   comment in its existing per-version block. Missing a bump means stale caches
   render the old layout and never self-correct.
3. **Every new field needs a `defined` bit**, or cascade cannot tell "unset" from
   "set to the default".
4. **Heap.** 415 CSS rules were observed on one real book, so per-rule growth
   multiplies. Byte enums and packed `CssLength` only — never a `std::string`
   per rule.
5. **Do not regress**: Japanese vertical text, ruby/furigana leading (fixed in
   `6ba186d8` — do not undo), Phase 3 pagination forward-progress (no empty
   pages, no infinite loops), Phase 4 selector cascade.

## Build and verify

```
~/.platformio/penv/bin/pio run -e default    # must succeed, zero new warnings
~/.platformio/penv/bin/pio run -e private    # must also succeed
~/.platformio/penv/bin/clang-format -i <every file you changed>
```

Then rebuild both after formatting.

### About `-e private`
`platformio.local.ini` (gitignored) defines an `env:private` that extends
`env:default` and swaps in a private overlay from `~/Projects/libby-crosspoint`
via `build_src_filter`. **You are not expected to modify anything in the
overlay** — CSS work is entirely public-tree. `-e private` must simply keep
building. If it fails only inside overlay files, stop and report rather than
editing them; if `platformio.local.ini` is missing on this machine, say so and
verify `-e default` only.

### Host-side tests
Phases 1-5 each compiled the relevant code standalone with
`clang++ -std=c++20` in the scratchpad (not committed) and reported pass counts.
Follow that precedent. Cover at minimum:
- `text-transform` with ASCII, accented Latin, **and Japanese** input — assert
  no byte corruption
- if you implement letter-spacing: measured width vs summed drawn advances for
  the same string must be equal

## Rules of engagement

- **Do NOT flash the device.** The user tests on hardware.
- **Do NOT `git push`.** Local commits only.
- The `freeink-sdk` submodule has unrelated local edits and a locally-moved
  checkout. **Never `git add -A`**; stage only files you changed by name, and
  never stage `freeink-sdk`.
- Commit once, message reflecting what you actually landed (drop the `wip:`
  prefix if it is complete). End with:
  `Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>`
- If something in the roadmap or this file is wrong once you read the real code,
  **say so in your report** rather than silently working around it. Every prior
  phase found at least one such error and correcting the doc was part of the work.

## Report back

- Files changed, with `file:line`
- What you landed, what you skipped, and **why** for each skip
- New `CSS_CACHE_VERSION` / `SECTION_FILE_VERSION` + verified fixed-byte count
- The UTF-8 strategy for `text-transform`
- If letter-spacing landed: the proof that measurement and draw agree
- Host-test results (pass counts)
- What needs device verification
- Any roadmap/handoff errors you found

## Device testing (the user does this, not you)

Six cache-version bumps have accumulated across Phases 1-6. **`.crosspoint/`
must be cleared** before judging anything, or a stale section cache renders the
old layout and it looks like nothing changed. Confirm `startBuild` appears in the
serial log rather than `Cache found, skipping build`. Then: a heading-heavy Latin
book, a Japanese vertical book (regression), and `maxAlloc` during chapter build
staying above ~40 KB.
