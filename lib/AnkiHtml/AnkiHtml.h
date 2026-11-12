#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// Lenient HTML5 -> XHTML conversion for Anki card HTML (AnkiConnect cards.render), so the
// expat-based chapter parser accepts it. Single pass, no DOM; pure and host-testable.
namespace ankihtml {

// Replaces `out` with an XHTML fragment for `html`. Never fails on malformed input; returns false
// (and clears `out`) only when the result would exceed `maxOut` bytes.
bool toXhtml(std::string_view html, std::string& out, size_t maxOut);

// Replaces `css` with the concatenated text of the <style> elements toXhtml() drops, each
// followed by '\n'. Returns false (and clears `css`) when it would exceed `maxOut` bytes.
bool extractStyles(std::string_view html, std::string& css, size_t maxOut);

}  // namespace ankihtml
