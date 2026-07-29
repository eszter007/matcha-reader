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
`color` / `background-color` (as an ink-polarity decision, see Phase 2) ·
`page-break-{before,after,inside}` + the `break-*` aliases (see Phase 3)

## Ignored, ranked by measured impact

| n | property | effect of ignoring |
|---|---|---|
| 65 | `font-family` | 12 `@font-face` faces ignored; everything uses the reader font |
| 15 | `line-height` | book leading lost; only the global Line Spacing applies |
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
| selector syntax + ancestor stack | `lib/Epub/Epub/css/CssSelector.h` — `parse()`, `forEachCandidate()`, `CssElementPath` (host-testable, no Arduino deps) |
| style struct + enums | `lib/Epub/Epub/css/CssStyle.h` — `CssStyle`, `CssLength`, `CssPropertyFlags` |
| cache format | `CssParser.h` — `CSS_CACHE_VERSION` (currently **16**); the framing constants (`CSS_LEADING_ENUM_BYTES` / `CSS_LENGTH_FIELD_COUNT` / `CSS_TRAILING_ENUM_BYTES` / `RULE_FIXED_BYTES`) are defined once at `CssParser.cpp` ~line 53 and shared by `writeRuleRecord`, `validateCache`, `loadFromCache` and `collectVerticalStyles` |
| style → layout | `lib/Epub/Epub/parsers/ChapterHtmlSlimParser.cpp` — `currentCssStyle`, ~lines 200-310 |
| line breaking / gaps | `lib/Epub/Epub/ParsedText.cpp` |
| section cache format | `lib/Epub/Epub/Section.cpp` — `SECTION_FILE_VERSION` (currently **61**) |

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
5. **Selectors:** `tag`, `.class`, `tag.class`, and two of those joined by a
   descendant or child combinator, all parsed by `lib/Epub/Epub/css/CssSelector.h`
   (see Phase 4). Three or more compounds, pseudo-classes, attribute/id/sibling
   selectors and the universal selector are still dropped. The EBPAJ
   `writing-mode` scoping (`.hltr X` / `.vrtl X`) keeps its own `h|`/`v|` key
   space and takes precedence — storing `.vrtl X` as an ordinary descendant rule
   would let the horizontal engine apply vertical-only styling.

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

## Phase 3 — `page-break-before` / `-after` / `-inside` — **done**

`always` and `avoid` on block boundaries, in the paginator. Both spellings are
accepted (`page-break-*` and the CSS3 `break-*`); `page` and the spread keywords
(`left`/`right`/`recto`/`verso`) all mean `always` on a one-page-per-screen
reader. Everything else — `auto`, `inherit`, the column/region values — leaves
the property unset. Three properties, two bits each, in ONE `CssStyle` byte
(`pageBreaks`) plus one `defined` bit; `CSS_TRAILING_ENUM_BYTES` 7 → 8.

| declaration | what the paginator does |
|---|---|
| `-before: always` | the block starts a new page |
| `-after: always` | the next block starts a new page |
| `-inside: avoid` | the block is laid out on one page if it fits on a page at all |
| `-after: avoid` | one line of room is kept after the block, so a heading is not orphaned at the page bottom |

`always` is raised as a one-shot pending break by the element that declares it
(open for `-before`, close for `-after`), NOT read from a block style: a
container's accumulated style is reused for every text block it opens, so
reading it per block would break a page before every paragraph inside it.
The `avoid` values do come from the block style, and are not inherited: a
container's `-inside: avoid` reaches only the blocks the container itself opens,
not the `<p>`s nested inside it. The block, not the container, is this engine's
unit of pagination — which is also why `h1,h2,…{page-break-inside:avoid}`, the
form books actually use, lands exactly as intended.

**Forward progress.** Layout is streaming (`layoutAndExtractLines` hands over one
line at a time), so a block's height is not known until its last line. An
`avoid` block therefore BUFFERS its lines and places them once the height is
known — and stops buffering the moment the block exceeds one viewport height, so
the buffer is bounded by a page and an over-long block is split exactly as it
would have been without the property. Every page boundary goes through
`breakPage()`, which refuses to flush a page with no elements on it. Those two
rules together are the guarantee: no blank page, and no block that fails to land.

## Phase 4 — descendant selectors — **done**

`A B` (descendant) and `A > B` (child), where each side is `tag`, `.class` or
`tag.class`. Selector lists (`h1, h2, h3 {…}`) were already split per selector and
now accept compound members. The selector subset, its normalized storage key and
the candidate enumeration live in `lib/Epub/Epub/css/CssSelector.h` — header-only
and free of Arduino/HAL deps, so the host test compiles the same code the device
runs.

**Two compounds, not three.** `.a .b p` is REJECTED rather than approximated by
its rightmost two: `.b p` matches outside `.a` too, which applies styling the
author scoped away. Dropping it is what those selectors did before, so nothing
regresses; a wrong match would be new damage.

**Matching.** A rule is stored under its normalized selector (`.callout p`,
`blockquote>p`) in the same map as simple rules, so a compound rule costs one
map node like any other. `resolveStyle` enumerates the keys the element *could*
match (its own forms, and each recorded ancestor's forms joined by each
combinator) and does one hash lookup each — no scan of the rule table. The
ancestor walk is skipped entirely unless the table holds a compound rule, so a
book without them performs exactly the lookups it did before.

**Specificity.** Every match is collected and applied in ascending
`16*classes + types` order: `p` < `div p` < `.note` < {`p.note`, `.c p`} <
`div p.note` < `.c .note`. On a tie, the LAST match in enumeration order wins —
simple before compound, outer ancestor before inner, descendant before child.
That is a documented deviation: real CSS breaks a tie by document order, which
this rule table cannot preserve (the map is unordered and the cache is written in
map order). Rules sharing one selector still merge in source order.

**Ancestor stack.** `CssElementPath` (also in `CssSelector.h`): 12 inline
entries of 16-byte tag + 32-byte classes, one push per start tag and one pop per
end tag in `ChapterHtmlSlimParser`, no heap and no per-element allocation. Past
12 levels the recorded entries stay a *prefix* of the real chain, so an over-deep
document loses matches instead of inventing them, and the child combinator turns
itself off once the real parent is no longer recorded. A tag or class name too
long for its buffer is dropped, never truncated into a name that could collide.

`CSS_CACHE_VERSION` 15 → 16 (the record framing is unchanged; the bump exists
because a v15 cache was written by a parser that dropped these rules) and
`SECTION_FILE_VERSION` 60 → 61.

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
