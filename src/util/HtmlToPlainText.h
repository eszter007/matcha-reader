#pragma once

#include <string>

// Find the closing '>' of a tag without mistaking quoted attribute text for markup.
// Returns npos for an incomplete tag or non-tag '<'; limit is exclusive.
bool htmlTagStart(const std::string& input, size_t pos);
size_t htmlTagEnd(const std::string& input, size_t pos, size_t limit);

// Convert an HTML fragment to readable plain text. This intentionally ignores
// styling; block elements become line breaks and HTML entities are decoded.
std::string htmlToPlainText(const std::string& html);
