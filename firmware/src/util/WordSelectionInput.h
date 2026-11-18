#pragma once

#include <cstdint>

#include "MappedInputManager.h"

class WordSelectionInput {
 public:
  enum ButtonEvent : uint8_t {
    INPUT_LEFT = 1,
    INPUT_RIGHT = 2,
    INPUT_UP = 4,
    INPUT_DOWN = 8,
    INPUT_CONFIRM = 16,
    INPUT_BACK = 32,
    INPUT_PREVIOUS = 64,
    INPUT_NEXT = 128,
    // Sentence steps: a held Left trims a word, a held Right grows a paragraph.
    INPUT_WORD_LEFT = INPUT_PREVIOUS,
    INPUT_WORD_RIGHT = INPUT_NEXT
  };
  static constexpr uint32_t BUTTON_REPEAT_MS = 500;
  static constexpr uint32_t WORD_REPEAT_MS = 300;
  // A held Right that grows by paragraph repeats slower, so each step is drawn before the next.
  static constexpr uint32_t PARAGRAPH_REPEAT_MS = 700;

  static uint8_t buttonEdges(const MappedInputManager& input) {
    using B = MappedInputManager::Button;
    return (input.wasPressed(B::ScreenLeft) ? INPUT_LEFT : 0) | (input.wasPressed(B::ScreenRight) ? INPUT_RIGHT : 0) |
           (input.wasPressed(B::ScreenUp) ? INPUT_UP : 0) | (input.wasPressed(B::ScreenDown) ? INPUT_DOWN : 0) |
           (input.wasReleased(B::Confirm) ? INPUT_CONFIRM : 0) | (input.wasReleased(B::Back) ? INPUT_BACK : 0) |
           (input.wasPressed(B::NavPrevious) ? INPUT_PREVIOUS : 0) | (input.wasPressed(B::NavNext) ? INPUT_NEXT : 0);
  }

  uint8_t pollButtons(const MappedInputManager& input, const uint32_t now) {
    uint8_t buttons = buttonEdges(input);
    const uint8_t held = (input.isPressed(MappedInputManager::Button::ScreenLeft) ? INPUT_LEFT : 0) |
                         (input.isPressed(MappedInputManager::Button::ScreenRight) ? INPUT_RIGHT : 0);
    if (held != repeatingButton || (buttons & (INPUT_LEFT | INPUT_RIGHT))) {
      repeatingButton = held;
      lastRepeat = now;
    } else if ((held == INPUT_LEFT || held == INPUT_RIGHT) && now - lastRepeat >= BUTTON_REPEAT_MS) {
      buttons |= held;
      lastRepeat = now;
    }
    return buttons;
  }

  // Sentence steps: Left/Right give INPUT_LEFT/RIGHT when released before
  // BUTTON_REPEAT_MS; held, they give INPUT_WORD_LEFT/RIGHT then and every
  // WORD_REPEAT_MS after (`rightRepeatMs` for Right). Up/Down, Confirm and Back as in buttonEdges().
  uint8_t pollSentenceButtons(const MappedInputManager& input, const uint32_t now,
                              const uint32_t rightRepeatMs = WORD_REPEAT_MS) {
    using B = MappedInputManager::Button;
    uint8_t buttons = (input.wasPressed(B::ScreenUp) ? INPUT_UP : 0) | (input.wasPressed(B::ScreenDown) ? INPUT_DOWN : 0) |
                      (input.wasReleased(B::Confirm) ? INPUT_CONFIRM : 0) | (input.wasReleased(B::Back) ? INPUT_BACK : 0);
    const uint8_t pressed =
        (input.wasPressed(B::ScreenLeft) ? INPUT_LEFT : 0) | (input.wasPressed(B::ScreenRight) ? INPUT_RIGHT : 0);
    if (pressed == INPUT_LEFT || pressed == INPUT_RIGHT) {
      holdButton = pressed;
      holdStart = now;
      wordSteps = 0;
    }
    if (holdButton != 0 && input.isPressed(holdButton == INPUT_LEFT ? B::ScreenLeft : B::ScreenRight)) {
      const uint32_t repeat = holdButton == INPUT_RIGHT ? rightRepeatMs : WORD_REPEAT_MS;
      if (now - holdStart >= BUTTON_REPEAT_MS + wordSteps * repeat) {
        buttons |= holdButton == INPUT_LEFT ? INPUT_WORD_LEFT : INPUT_WORD_RIGHT;
        ++wordSteps;
      }
    } else if (holdButton != 0) {
      if (wordSteps == 0) buttons |= holdButton;
      holdButton = 0;
    }
    return buttons;
  }

 private:
  uint8_t repeatingButton = 0;
  uint32_t lastRepeat = 0;
  uint8_t holdButton = 0;
  uint32_t holdStart = 0;
  uint32_t wordSteps = 0;
};
