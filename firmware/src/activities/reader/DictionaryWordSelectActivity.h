#pragma once

#include <Epub/Page.h>
#include <I18n.h>

#include <memory>
#include <string>

#include "activities/Activity.h"
#include "util/Dictionary.h"

// Controller for dictionary lookup: renders the background page, tries each
// dictionary of DictionaryRegistry::lookupChain() in turn until one has the
// word, and either opens DictionaryDefinitionActivity on it or displays a
// status popup (Looking up, Indexing, Not found, Error). A word found is logged
// to /x4/lookups.jsonl with the book it was read in.
class DictionaryWordSelectActivity final : public Activity {
 public:
  explicit DictionaryWordSelectActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                        std::unique_ptr<Page> page, int marginLeft, int marginTop,
                                        std::string lookupText, std::string bookPath, std::string bookTitle)
      : Activity("DictionaryWordSelect", renderer, mappedInput),
        page(std::move(page)),
        marginLeft(marginLeft),
        marginTop(marginTop),
        lookupText(std::move(lookupText)),
        bookPath(std::move(bookPath)),
        bookTitle(std::move(bookTitle)) {}

  void onEnter() override;
  void loop() override;
  void render(RenderLock&&) override;

 private:
  enum class Popup : uint8_t { None, Busy, NotFound, Error };

  void performLookup();

  std::unique_ptr<Page> page;
  const int marginLeft;
  const int marginTop;
  int fontId = 0;

  std::string lookupText;
  std::string bookPath;
  std::string bookTitle;
  bool lookupPending = false;
  // The dictionary of the source being tried; after a hit, the one nested
  // lookups in the definition use.
  Dictionary dict;

  Popup popup = Popup::None;
  StrId popupMsg = StrId::STR_DICT_NOT_FOUND;
  unsigned long popupTime = 0;
};
