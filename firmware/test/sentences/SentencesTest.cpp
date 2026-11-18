#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "Sentences.h"

namespace {

// Splits on spaces like the reader's layout does; a newline starts a paragraph.
std::vector<sentences::Word> tokens(const std::string& text, std::vector<std::string>& storage) {
  storage.clear();
  std::vector<bool> starts;
  bool paragraph = true;
  std::string current;
  auto flush = [&] {
    if (current.empty()) return;
    storage.push_back(current);
    starts.push_back(paragraph);
    paragraph = false;
    current.clear();
  };
  for (const char c : text) {
    if (c == ' ') {
      flush();
    } else if (c == '\n') {
      flush();
      paragraph = true;
    } else {
      current += c;
    }
  }
  flush();
  std::vector<sentences::Word> words;
  for (size_t i = 0; i < storage.size(); ++i) words.push_back({storage[i], starts[i]});
  return words;
}

sentences::Word at(const void* ctx, const size_t index) {
  return (*static_cast<const std::vector<sentences::Word>*>(ctx))[index];
}

std::vector<std::string> split(const std::string& text) {
  std::vector<std::string> storage;
  const auto words = tokens(text, storage);
  std::vector<std::string> out;
  std::string sentence;
  for (size_t i = 0; i < words.size(); ++i) {
    if (!sentence.empty()) sentence += ' ';
    sentence += words[i].text;
    if (sentences::endsAt(&at, &words, words.size(), i)) {
      out.push_back(sentence);
      sentence.clear();
    }
  }
  return out;
}

using Sentences = std::vector<std::string>;

TEST(Sentences, SplitsAtFullStopsAndMarks) {
  EXPECT_EQ(split("You have power over your mind. Not outside events! Realize this? Strength follows."),
            (Sentences{"You have power over your mind.", "Not outside events!", "Realize this?", "Strength follows."}));
}

TEST(Sentences, KeepsClosingQuotesAndBrackets) {
  EXPECT_EQ(split("He said \"Go home.\" Then he left. (It was late.) We slept."),
            (Sentences{"He said \"Go home.\"", "Then he left.", "(It was late.)", "We slept."}));
  EXPECT_EQ(split("She asked “why?” Nobody knew."), (Sentences{"She asked “why?”", "Nobody knew."}));
  // A closing quote laid out as its own word stays with its sentence.
  EXPECT_EQ(split("It ended. ” Then silence."), (Sentences{"It ended. ”", "Then silence."}));
}

TEST(Sentences, EllipsisEndsUnlessTheSentenceGoesOn) {
  EXPECT_EQ(split("Wait… Then go. Well... maybe not."), (Sentences{"Wait…", "Then go.", "Well... maybe not."}));
}

TEST(Sentences, EnglishAbbreviationsAndInitials) {
  EXPECT_EQ(split("Mr. Smith met Dr. Jones. They talked."), (Sentences{"Mr. Smith met Dr. Jones.", "They talked."}));
  EXPECT_EQ(split("Fruit, e.g. Apples, etc. Are fine. Use it, i.e. Daily."),
            (Sentences{"Fruit, e.g. Apples, etc. Are fine.", "Use it, i.e. Daily."}));
  EXPECT_EQ(split("J. R. R. Tolkien wrote it. See p. 12 and No. 5 here."),
            (Sentences{"J. R. R. Tolkien wrote it.", "See p. 12 and No. 5 here."}));
  EXPECT_EQ(split("It was in the U.S. Army files."), (Sentences{"It was in the U.S. Army files."}));
}

TEST(Sentences, DecimalsAndNumbers) {
  EXPECT_EQ(split("Pi is 3.14 or so. It costs $2.50. Born in 1985. Done."),
            (Sentences{"Pi is 3.14 or so.", "It costs $2.50.", "Born in 1985.", "Done."}));
}

TEST(Sentences, LowerCaseAfterAMarkContinues) {
  EXPECT_EQ(split("\"Run!\" he said. \"Now?\" she asked."), (Sentences{"\"Run!\" he said.", "\"Now?\" she asked."}));
}

TEST(Sentences, ParagraphsEndSentences) {
  EXPECT_EQ(split("Chapter One\nIt began.\n1. Introduction\nText here"),
            (Sentences{"Chapter One", "It began.", "1. Introduction", "Text here"}));
}

TEST(Sentences, Hungarian) {
  EXPECT_EQ(split("Ma szép idő van. Holnap esni fog! Mit csinálsz?"),
            (Sentences{"Ma szép idő van.", "Holnap esni fog!", "Mit csinálsz?"}));
  // Abbreviations, dates and ordinals.
  EXPECT_EQ(split("Hoztam pl. Almát, körtét stb. Ezt Dr. Kovács mondta."),
            (Sentences{"Hoztam pl. Almát, körtét stb. Ezt Dr. Kovács mondta."}));
  EXPECT_EQ(split("1848. március 15-én kezdődött. A 2. fejezet rövid. XIV. Lajos uralkodott."),
            (Sentences{"1848. március 15-én kezdődött.", "A 2. fejezet rövid.", "XIV. Lajos uralkodott."}));
  EXPECT_EQ(split("Ez kb. Egy óra. Ún. Szabály volt."), (Sentences{"Ez kb. Egy óra.", "Ún. Szabály volt."}));
  // Dialogue with dashes and „…” quotes.
  EXPECT_EQ(split("– Jó! – kiáltotta. – Gyere! Elindultak."),
            (Sentences{"– Jó! – kiáltotta.", "– Gyere!", "Elindultak."}));
  EXPECT_EQ(split("Azt mondta: „Megyek.” Aztán ment."), (Sentences{"Azt mondta: „Megyek.”", "Aztán ment."}));
  EXPECT_EQ(split("Ő mondta. Ű betű."), (Sentences{"Ő mondta.", "Ű betű."}));
}

TEST(Sentences, EndOfRunEndsTheSentence) {
  EXPECT_EQ(split("cut off mid"), (Sentences{"cut off mid"}));
}

TEST(Sentences, CanStart) {
  EXPECT_TRUE(sentences::canStart("The"));
  EXPECT_TRUE(sentences::canStart("„Megyek"));
  EXPECT_TRUE(sentences::canStart("Ő"));
  EXPECT_TRUE(sentences::canStart("1848."));
  EXPECT_FALSE(sentences::canStart("and"));
  EXPECT_FALSE(sentences::canStart("(és"));
  EXPECT_FALSE(sentences::canStart("ő"));
  EXPECT_FALSE(sentences::canStart(""));
}

}  // namespace
