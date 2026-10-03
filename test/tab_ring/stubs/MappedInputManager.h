#pragma once

#include <cstdint>

class MappedInputManager {
 public:
  enum class Button { Left, Right, ScreenLeft, ScreenRight, Confirm };
  uint8_t pressed = 0;
  uint8_t released = 0;
  bool wasPressed(const Button button) const { return pressed & (1u << static_cast<unsigned>(button)); }
  bool wasReleased(const Button button) const { return released & (1u << static_cast<unsigned>(button)); }
};
