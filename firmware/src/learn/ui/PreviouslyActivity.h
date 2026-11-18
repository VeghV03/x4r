#pragma once

#include "activities/Activity.h"
#include "learn/Previously.h"
#include "util/ButtonNavigator.h"

// "Previously": a book's recap, shown over the reader before its first page. Confirm or Next
// goes on to the page; Back finishes cancelled, and the reader goes home.
class PreviouslyActivity final : public Activity {
 public:
  PreviouslyActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const char* text);
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  static void drawGrayPass(void* self);
  void draw(bool gray);

  char text[x4::previously::TEXT_MAX + 1];
  ButtonNavigator navigator;
  int pagesUntilFullRefresh = 1;  // the first screen after the sleep image is a clean one
  bool leaving = false;
};
