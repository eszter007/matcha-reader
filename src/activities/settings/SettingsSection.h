#pragma once

#include <I18n.h>

#include <cstddef>
#include <cstdint>

// Top-level groups on the Settings screen, in display order. End doubles as
// "no section" where one is optional.
// Appended only: a section's index is persisted for the silent-restart return (main.cpp).
enum class SettingsSection : uint8_t {
  System,
  Display,
  Sleep,
  Library,
  Insights,
  Reader,
  Controls,
  Connectivity,
  Flashcards,
  End
};

inline constexpr size_t SETTINGS_SECTION_COUNT = static_cast<size_t>(SettingsSection::End);

inline constexpr StrId SETTINGS_SECTION_TITLES[SETTINGS_SECTION_COUNT] = {
    StrId::STR_CAT_SYSTEM, StrId::STR_CAT_DISPLAY, StrId::STR_CAT_SLEEP,    StrId::STR_CAT_LIBRARY,
    StrId::STR_STATS,      StrId::STR_CAT_READER,  StrId::STR_CAT_CONTROLS, StrId::STR_SETTINGS_NETWORK,
    StrId::STR_FLASHCARDS,
};
