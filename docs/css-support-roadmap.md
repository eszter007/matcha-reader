# CSS support roadmap

What the reader's CSS engine currently ignores, why it matters, and how to add it.

## How this was measured

The stylesheet of a real trade book (*The Coaching Habit*, Page Two, 2016 —
14,875 bytes, 931 lines) was dumped after decryption and every declaration
classified against `CssParser::applyDeclaration`:

| | declarations |
|---|---|
| supported | 297 |
| **ignored** | **212** |

**~42% of the book's CSS is silently dropped.** The pattern generalises: the
ignored properties are the ones trade publishers use for visual hierarchy, so
most professionally-typeset EPUBs lose their headings, spacing and layout the
same way.

## Currently supported

`text-align` · `text-indent` · `text-decoration(-line)` · `text-emphasis(-style)`
(+`-epub-`/`-webkit-`) · `font-style` · `font-weight` · `font-variant(-caps)` ·
`font` · `display` · `direction` · `margin{,-top,-right,-bottom,-left}` ·
`padding{,-top,-right,-bottom,-left}` · `border{,-top,-right,-bottom,-left,-style}` ·
`list-style(-type)` · `width` · `height` · `vertical-align`

## Ignored, ranked by measured impact

| n | property | effect of ignoring |
|---|---|---|
| 65 | `font-family` | 12 `@font-face` faces ignored; everything uses the reader font |
| **45** | **`font-size`** | `h1{4em}`, `h2{2.2em}`, `h3{1.2em}` all render at body size |
| 15 | `line-height` | book leading lost; only the global Line Spacing applies |
| 12 | `color` | **can make text invisible** — see the `h1` trap below |
| 13 | `page-break-{inside,before,after}` | sections run together |
| 8 | `letter-spacing` | tracking on headings lost |
| 8 | `float` | pull-quotes and figures fall inline |
| 5 | `background-color` | heading bars / callout panels vanish |
| 3+18 | `hyphens`, `hyphenate-limit-*` (+vendor) | book's hyphenation prefs ignored |
| 3 | `border-radius` | rounded panels square (harmless on 1-bit) |
| 3 | `text-transform` | `uppercase` headings render as authored |
| 3+3 | `widows` / `orphans` | no paragraph-break control |
| 1 | `max-width` | over-wide blocks |
| 1 | `border-collapse` | table borders doubled |

### The `h1` trap — fix before or with `font-size`

```css
h1 { color:#fff; background-color:#a7a9ac; padding:.5em 5%; border-radius:20px; }
```

`padding` **is** supported; `color` and `background-color` are **not**. The
indent is reserved while the white text and grey panel are dropped → **white
text on a white page: invisible chapter titles**, with padding still consuming
space. Ignoring the whole rule would be better than what happens today, so
`color` must not lag far behind `font-size`.

---

## Where things live

| concern | file |
|---|---|
| property dispatch | `lib/Epub/Epub/css/CssParser.cpp` — `applyDeclaration()`, ~line 400, `iequalsAscii(name, ...)` chain |
| style struct + enums | `lib/Epub/Epub/css/CssStyle.h` — `CssStyle`, `CssLength`, `CssPropertyFlags` |
| cache format | `CssParser.h` — `CSS_CACHE_VERSION` (currently **12**); serialiser ~line 944, reader ~line 1031, `RULE_FIXED_BYTES` ~line 1047 |
| style → layout | `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp` — `currentCssStyle`, ~lines 200-310 |
| line breaking / gaps | `lib/Epub/Epub/ParsedText.cpp` |
| section cache format | `lib/Epub/Epub/Section.cpp` — `SECTION_FILE_VERSION` (currently **57**) |

### Rules that apply to every item below

1. **Bump `CSS_CACHE_VERSION`** when the serialised rule layout changes, and
   **`SECTION_FILE_VERSION`** when laid-out output changes. Both are documented
   with a per-version comment — add one. Miss this and stale caches render with
   the old layout and never self-correct (see `reader-font-substitution`).
2. **`RULE_FIXED_BYTES` must match the serialiser exactly.** It is
   `5 + 11*(sizeof(float)+1) + 6 + sizeof(uint32_t)` — the `11` is the
   `CssLength` count. Adding a `CssLength` means +1 there; adding a byte enum
   means +1 to the `6`.
3. **Every new field needs a `defined` bit** in `CssPropertyFlags`, or
   inheritance/cascade can't tell "unset" from "set to the default".
4. **Heap.** Rules are held in RAM during parse. `CssParser` already tracks
   `wasHeapTruncated()`; new per-rule bytes multiply by rule count (415 rules
   observed on one book). Prefer byte enums and packed `CssLength` over new
   `std::string` members — a string per rule is what OOMs a 380 KB device.
5. **Selectors:** only ONE descendant form is supported (the EBPAJ
   `writing-mode` scoping, `CssParser.cpp` ~line 656). Rules like `.callout p`
   never match regardless of property support. Worth fixing on its own (see
   Phase 4) — otherwise property work silently under-delivers on books that
   style via nested containers.

---

## Phase 1 — `font-size` *(highest payoff)*

Makes headings look like headings. 45 declarations in the sample book.

- Add `CssLength fontSize` to `CssStyle` + a `defined.fontSize` bit.
- Parse in `applyDeclaration` with the existing `interpretLength()` — already
  handles `em`, `rem`, `%`, `pt`, `px`.
- Resolve against the *reader's* font size as the em base, not a fixed 16px, so
  the user's size setting still governs. Clamp to the sizes actually available
  (`BUILTIN_READER_POINT_SIZES`, or the SD family's set) and snap with the
  existing `snapToNearestPointSize`.
- `ChapterHtmlSlimParser` picks the per-block font id from the resolved size;
  `ParsedText` must measure with that same id (see `reader-font-substitution` —
  layout and drawing must never disagree on the font).
- Bump `CSS_CACHE_VERSION` **and** `SECTION_FILE_VERSION`.

**Risk:** every distinct size needs a loaded font. Cap the number of distinct
sizes per chapter and snap the rest, or a heading-heavy book will thrash the
font cache. Measure `maxAlloc` during a chapter build before and after.

## Phase 2 — `color` + `background-color`

Stops invisible text. Only three outcomes matter on a 1-bit panel:

- normal (dark on light)
- **light text on a dark background → invert or force black**
- everything else → ignore

Store one `CssInkMode : uint8_t { Normal, Inverted }` byte (+`defined` bit),
derived at parse time from the luminance of `color` vs `background-color`. That
avoids storing RGB per rule and keeps the cache small. Renderer already draws
black-on-white or white-on-black.

## Phase 3 — `page-break-before` / `-after` / `-inside`

`always` / `avoid` on block boundaries, in the paginator. Two bits per block
(before/after) plus one for `avoid-inside`. Bump `SECTION_FILE_VERSION` only —
no `CssLength`, so `RULE_FIXED_BYTES` grows by 1 byte.

## Phase 4 — descendant selectors

Support `A B` (descendant) and `A > B` (child) for at least two levels. This is
what makes the other phases actually land on books that scope styles via
containers. Needs a small tag stack in `ChapterHtmlSlimParser` and a selector
representation change → `CSS_CACHE_VERSION` bump.

**Do this before assuming a phase "didn't work".**

## Phase 5 — `line-height`

Per-block leading, resolved as a multiple of the block's font size, clamped so
the global Line Spacing setting still has authority. `SECTION_FILE_VERSION` bump.

## Phase 6 — long tail

`text-transform:uppercase` (transform at parse, watch UTF-8) · `letter-spacing`
(per-glyph advance already exists in `getSpaceAdvance`-adjacent code) ·
`widows`/`orphans` · `max-width` · `float` (needs real block layout — likely
**out of scope**, see SCOPE.md) · `hyphens:none` (map to the existing
hyphenation flag).

## Explicitly out of scope

- **`@font-face`** — 65 declarations, but fonts must be pre-converted `.cpfont`;
  loading OTF at runtime is not viable on this hardware. Highest count, lowest
  feasibility.
- **`border-radius`**, colour fidelity beyond the invert decision, and anything
  needing float/absolute positioning.

---

## Verification for each phase

1. `pio run -e default` clean, no new warnings.
2. Clear the reading cache — **required**; a stale section cache renders with
   the old layout and will look like the change did nothing.
3. Open a heading-heavy Latin book and a Japanese vertical book (regression).
4. Serial: `maxAlloc` during chapter build must not fall below ~40 KB.
5. Confirm the cache actually rebuilt (`startBuild` / no `Cache found, skipping
   build`) before judging the result.
