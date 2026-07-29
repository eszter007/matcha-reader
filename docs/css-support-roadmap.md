# CSS support roadmap

What the reader's CSS engine currently ignores, why it matters, and how to add it.

## How this was measured

The stylesheet of a real trade book (*The Coaching Habit*, Page Two, 2016 —
14,875 bytes, 931 lines) was dumped after decryption and every declaration
classified against `CssParser::parseDeclarationIntoStyle`:

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
`list-style(-type)` · `width` · `height` · `vertical-align` · `font-size` ·
`color` / `background-color` (as an ink-polarity decision, see Phase 2)

## Ignored, ranked by measured impact

| n | property | effect of ignoring |
|---|---|---|
| 65 | `font-family` | 12 `@font-face` faces ignored; everything uses the reader font |
| 15 | `line-height` | book leading lost; only the global Line Spacing applies |
| 13 | `page-break-{inside,before,after}` | sections run together |
| 8 | `letter-spacing` | tracking on headings lost |
| 8 | `float` | pull-quotes and figures fall inline |
| 3+18 | `hyphens`, `hyphenate-limit-*` (+vendor) | book's hyphenation prefs ignored |
| 3 | `border-radius` | rounded panels square (harmless on 1-bit) |
| 3 | `text-transform` | `uppercase` headings render as authored |
| 3+3 | `widows` / `orphans` | no paragraph-break control |
| 1 | `max-width` | over-wide blocks |
| 1 | `border-collapse` | table borders doubled |

### The `h1` trap — fixed in Phase 2

```css
h1 { color:#fff; background-color:#a7a9ac; padding:.5em 5%; border-radius:20px; }
```

`padding` **is** supported; `color` and `background-color` are **not**. The
indent is reserved while the white text and grey panel are dropped → **white
text on a white page: invisible chapter titles**, with padding still consuming
space. Ignoring the whole rule would be better than what happened before Phase 2,
which is why `color` was not allowed to lag far behind `font-size`.

---

## Where things live

| concern | file |
|---|---|
| property dispatch | `lib/Epub/Epub/css/CssParser.cpp` — `parseDeclarationIntoStyle()` (there is no `applyDeclaration`), `iequalsAscii(name, ...)` chain |
| style struct + enums | `lib/Epub/Epub/css/CssStyle.h` — `CssStyle`, `CssLength`, `CssPropertyFlags` |
| cache format | `CssParser.h` — `CSS_CACHE_VERSION` (currently **14**); the framing constants (`CSS_LEADING_ENUM_BYTES` / `CSS_LENGTH_FIELD_COUNT` / `CSS_TRAILING_ENUM_BYTES` / `RULE_FIXED_BYTES`) are defined once at `CssParser.cpp` ~line 53 and shared by `writeRuleRecord`, `validateCache`, `loadFromCache` and `collectVerticalStyles` |
| style → layout | `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp` — `currentCssStyle`, ~lines 200-310 |
| line breaking / gaps | `lib/Epub/Epub/ParsedText.cpp` |
| section cache format | `lib/Epub/Epub/Section.cpp` — `SECTION_FILE_VERSION` (currently **59**) |

### Rules that apply to every item below

1. **Bump `CSS_CACHE_VERSION`** when the serialised rule layout changes, and
   **`SECTION_FILE_VERSION`** when laid-out output changes. Both are documented
   with a per-version comment — add one. Miss this and stale caches render with
   the old layout and never self-correct (see `reader-font-substitution`).
2. **`RULE_FIXED_BYTES` must match the serialiser exactly.** It is
   `CSS_LEADING_ENUM_BYTES + CSS_LENGTH_FIELD_COUNT*(sizeof(float)+1) +
   CSS_TRAILING_ENUM_BYTES + sizeof(uint32_t)`. Adding a `CssLength` means +1 to
   `CSS_LENGTH_FIELD_COUNT`; adding a byte enum means +1 to
   `CSS_TRAILING_ENUM_BYTES`. All three constants live at file scope in
   `CssParser.cpp` and every reader derives from them.
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

## Phase 1 — `font-size` *(highest payoff)* — **done**

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

## Phase 2 — `color` + `background-color` — **done**

Stops invisible text. Only two outcomes matter on a 1-bit panel, held in one
`CssInkMode : uint8_t { Normal, Inverted }` byte (+ a `defined` bit) derived at
parse time — no RGB is stored per rule or in the cache:

- **Inverted** — the text is meaningfully lighter than its background
  (Rec.601 luma delta ≥ 64 of 255). The block's lines each paint a black panel
  across the block's *padding box* and draw their glyphs, ruby and decorations
  white.
- **Normal** — everything else, including a light `color` with no background and
  a dark `background-color` with no `color`. Normal draws black text on the
  untouched page, so it is always legible; only `Inverted` can be wrong in a way
  that costs contrast, which is why the threshold errs toward `Normal`.

An unspecified side falls back to the page's own polarity (black text, white
page), which is what makes both single-sided cases resolve to `Normal`.
Colours are only compared *within one rule block* — the engine has no cascade of
colour across selectors, so `body{background:#000}` + `p{color:#fff}` in separate
rules resolves to `Normal` (safe) rather than `Inverted`.

## Phase 3 — `page-break-before` / `-after` / `-inside`

`always` / `avoid` on block boundaries, in the paginator. Two bits per block
(before/after) plus one for `avoid-inside`. Needs a `CSS_CACHE_VERSION` bump too:
no `CssLength`, so `CSS_TRAILING_ENUM_BYTES` grows by 1.

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
