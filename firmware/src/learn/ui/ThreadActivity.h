#pragma once

#include <memory>
#include <string>

#include "activities/Activity.h"
#include "learn/Threads.h"
#include "learn/ui/StreamActivity.h"
#include "util/ButtonNavigator.h"

// A running story so far, shown before a piece of the paper, once for each version of its text.
// Confirm or Next goes on to the piece's question or its article, Back to where it was opened.
class ThreadActivity final : public Activity {
 public:
  ThreadActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::unique_ptr<x4::threads::Thread> thread,
                 StreamActivity::Piece piece)
      : Activity("Thread", renderer, mappedInput), piece(std::move(piece)), thread(std::move(thread)) {}
  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  StreamActivity::Piece piece;
  static void drawGrayPass(void* self);
  void draw(bool gray);

  std::unique_ptr<x4::threads::Thread> thread;
  ButtonNavigator navigator;
  int pagesUntilFullRefresh = 1;
  bool leaving = false;
};
