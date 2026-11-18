#include "Item.h"

#include <ArduinoJson.h>
#include <Logging.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

namespace x4 {

namespace {

// Copies strings into the arena after the JSON document has taken its own
// copies, so the line that used to live there may be overwritten.
class ArenaWriter {
 public:
  explicit ArenaWriter(char* arena) : arena(arena) {}

  bool put(const std::string_view a, Slice& out) { return put3(a, {}, {}, out); }

  bool put3(const std::string_view a, const std::string_view b, const std::string_view c, Slice& out) {
    const size_t total = a.size() + b.size() + c.size();
    if (total > UINT16_MAX || used + total + 1 > ARENA_MAX) return false;
    out.offset = static_cast<uint16_t>(used);
    out.length = static_cast<uint16_t>(total);
    std::memcpy(arena + used, a.data(), a.size());
    std::memcpy(arena + used + a.size(), b.data(), b.size());
    std::memcpy(arena + used + a.size() + b.size(), c.data(), c.size());
    used += total;
    arena[used++] = '\0';
    return true;
  }

 private:
  char* arena;
  size_t used = 0;
};

constexpr char CLOZE_BLANK[] = "______";

// A required, non-empty string no longer than `limit` bytes.
bool textField(JsonVariantConst value, const size_t limit, std::string_view& out) {
  if (!value.is<const char*>()) return false;
  const char* s = value.as<const char*>();
  const size_t len = std::strlen(s);
  if (len == 0 || len > limit) return false;
  out = {s, len};
  return true;
}

bool optionalText(JsonVariantConst value, const size_t limit, std::string_view& out) {
  out = {};
  if (value.isNull()) return true;
  return textField(value, limit, out);
}

bool copyId(JsonVariantConst value, char (&id)[ID_MAX + 1]) {
  std::string_view s;
  if (!textField(value, ID_MAX, s)) return false;
  std::memcpy(id, s.data(), s.size());
  id[s.size()] = '\0';
  return true;
}

bool parseLine(JsonDocument& doc, const char* line, const size_t length) {
  const DeserializationError error = deserializeJson(doc, line, length);
  if (error) {
    LOG_ERR("ITEM", "Bad JSON line: %s", error.c_str());
    return false;
  }
  return doc.is<JsonObject>();
}

bool cardType(const char* type, CardType& out) {
  if (!type) return false;
  for (uint8_t i = 0; i <= static_cast<uint8_t>(CardType::Explain); ++i) {
    if (std::strcmp(type, kindOf(static_cast<Shape>(i)).name) != 0) continue;
    out = static_cast<CardType>(i);
    return true;
  }
  return false;
}

// Splits "a {{term}} b" into prefix, term and suffix; exactly one term allowed.
bool splitCloze(const std::string_view text, std::string_view& prefix, std::string_view& term,
                std::string_view& suffix) {
  const size_t open = text.find("{{");
  if (open == std::string_view::npos) return false;
  const size_t close = text.find("}}", open + 2);
  if (close == std::string_view::npos || close == open + 2) return false;
  if (text.find("{{", close + 2) != std::string_view::npos) return false;
  prefix = text.substr(0, open);
  term = text.substr(open + 2, close - open - 2);
  suffix = text.substr(close + 2);
  return true;
}

// Validates a card and fills `out` when `writer` is set.
bool readCard(JsonObjectConst card, char (&id)[ID_MAX + 1], CardType& type, Item* out, ArenaWriter* writer) {
  if (!copyId(card["id"], id) || !cardType(card["type"].as<const char*>(), type)) return false;
  std::string_view why;
  if (!optionalText(card["why"], TEXT_MAX, why)) return false;

  std::string_view title;
  std::string_view body;
  std::string_view options[POINTS_MAX];
  uint8_t optionCount = 0;
  uint8_t answer = 0;
  std::string_view clozePrefix;
  std::string_view clozeTerm;
  std::string_view clozeSuffix;

  const auto readList = [&](JsonVariantConst list, const uint8_t minCount, const uint8_t maxCount) {
    if (!list.is<JsonArrayConst>()) return false;
    const JsonArrayConst array = list.as<JsonArrayConst>();
    if (array.size() < minCount || array.size() > maxCount) return false;
    for (JsonVariantConst value : array) {
      if (!textField(value, TEXT_MAX, options[optionCount])) return false;
      ++optionCount;
    }
    return true;
  };

  switch (type) {
    case CardType::Flip:
      if (!textField(card["front"], TEXT_MAX, title) || !textField(card["back"], TEXT_MAX, body)) return false;
      break;
    case CardType::Choice: {
      if (!textField(card["prompt"], TEXT_MAX, title) || !readList(card["options"], OPTIONS_MIN, OPTIONS_MAX)) {
        return false;
      }
      if (!card["answer"].is<int>()) return false;
      const int index = card["answer"].as<int>();
      if (index < 0 || index >= optionCount) return false;
      answer = static_cast<uint8_t>(index);
      break;
    }
    case CardType::Cloze: {
      std::string_view raw;
      if (!textField(card["text"], TEXT_MAX, raw) || !splitCloze(raw, clozePrefix, clozeTerm, clozeSuffix) ||
          !readList(card["options"], OPTIONS_MIN, OPTIONS_MAX)) {
        return false;
      }
      bool found = false;
      for (uint8_t i = 0; i < optionCount && !found; ++i) {
        if (options[i] == clozeTerm) {
          answer = i;
          found = true;
        }
      }
      if (!found) return false;
      break;
    }
    case CardType::Explain:
      if (!textField(card["prompt"], TEXT_MAX, title) || !readList(card["points"], POINTS_MIN, POINTS_MAX)) {
        return false;
      }
      break;
  }

  if (!out || !writer) return true;
  out->shape = static_cast<Shape>(type);  // the card types open Shape, in this order
  std::memcpy(out->id, id, sizeof(out->id));
  bool ok = true;
  if (type == CardType::Cloze) {
    ok = writer->put3(clozePrefix, CLOZE_BLANK, clozeSuffix, out->title) &&
         writer->put3(clozePrefix, clozeTerm, clozeSuffix, out->body);
  } else {
    ok = writer->put(title, out->title) && writer->put(body, out->body);
  }
  ok = ok && writer->put(why, out->why);
  for (uint8_t i = 0; ok && i < optionCount; ++i) ok = writer->put(options[i], out->options[i]);
  out->optionCount = optionCount;
  out->answer = answer;
  return ok;
}

// What a kind's parser reads off its stream line, before the arena takes copies.
struct StreamFields {
  std::string_view title;
  std::string_view body;
  std::string_view book;
  std::string_view why;
  std::string_view rows[LIST_MAX];
  std::string_view links[LIST_MAX];
  std::string_view linkPrompt;
  std::string_view linkWhy;
  std::string_view after;  // a paragraph under the title
  char scratch[24] = {};   // holds `after` when the parser composes it
  uint8_t count = 0;
  uint8_t answer = 0;
  uint8_t linkAnswer = 0;
};

// `count` short strings, minCount..maxCount of them.
bool shortList(JsonVariantConst list, const uint8_t minCount, const uint8_t maxCount, StreamFields& out) {
  if (!list.is<JsonArrayConst>()) return false;
  const JsonArrayConst array = list.as<JsonArrayConst>();
  if (array.size() < minCount || array.size() > maxCount) return false;
  for (JsonVariantConst value : array) {
    if (!textField(value, SHORT_MAX, out.rows[out.count])) return false;
    ++out.count;
  }
  return true;
}

bool answerIndex(JsonVariantConst value, StreamFields& out) {
  if (!value.is<int>()) return false;
  const int index = value.as<int>();
  if (index < 0 || index >= out.count) return false;
  out.answer = static_cast<uint8_t>(index);
  return true;
}

// ---- the stream kinds' parsers: one each, false for an invalid line -----------

using Parser = bool (*)(JsonObjectConst line, StreamFields& f, Item& out);

bool parseText(JsonObjectConst line, StreamFields& f, Item&) {
  return textField(line["title"], TEXT_MAX, f.title) && textField(line["body"], BODY_MAX, f.body);
}

// `link`: three things read earlier ({"text"}; the host keeps what each refers to), the one
// that connects, and why.
bool parseLink(JsonVariantConst link, StreamFields& f) {
  if (!link.is<JsonObjectConst>() || !textField(link["prompt"], TEXT_MAX, f.linkPrompt) ||
      !textField(link["why"], TEXT_MAX, f.linkWhy) || !link["options"].is<JsonArrayConst>()) {
    return false;
  }
  const JsonArrayConst options = link["options"].as<JsonArrayConst>();
  if (options.size() != LINK_OPTIONS) return false;
  uint8_t i = 0;
  for (JsonVariantConst option : options) {
    if (!option.is<JsonObjectConst>() || !textField(option["text"], SHORT_MAX, f.links[i++])) return false;
  }
  if (!link["answer"].is<int>()) return false;
  const int answer = link["answer"].as<int>();
  if (answer < 0 || answer >= LINK_OPTIONS) return false;
  f.linkAnswer = static_cast<uint8_t>(answer);
  return true;
}

// With `predict`: a question the article answers, and the sentence of it that does. With
// `link`: a question after it.
bool parseArticle(JsonObjectConst line, StreamFields& f) {
  if (!textField(line["title"], TEXT_MAX, f.title) || f.book.empty()) return false;
  const JsonVariantConst predict = line["predict"];
  if (!predict.isNull() &&
      !(predict.is<JsonObjectConst>() && textField(predict["prompt"], TEXT_MAX, f.body) &&
        shortList(predict["options"], OPTIONS_MIN, OPTIONS_MAX, f) && answerIndex(predict["answer"], f) &&
        textField(predict["quote"], TEXT_MAX, f.why))) {
    return false;
  }
  return line["link"].isNull() || parseLink(line["link"], f);
}

// Inline text lets the host resurface a highlight it already pulled and deleted
// from highlights.jsonl.
bool parseHighlight(JsonObjectConst line, StreamFields& f, Item& out) {
  return copyId(line["highlight"], out.highlightId) && optionalText(line["text"], BODY_MAX, f.body) &&
         optionalText(line["title"], TEXT_MAX, f.title);
}

bool parseRiddle(JsonObjectConst line, StreamFields& f, Item&) {
  return textField(line["prompt"], BODY_MAX, f.title) && textField(line["answer"], TEXT_MAX, f.body);
}

bool parseOrder(JsonObjectConst line, StreamFields& f, Item&) {
  return textField(line["prompt"], BODY_MAX, f.title) && shortList(line["items"], ORDER_MIN, ORDER_MAX, f) &&
         optionalText(line["why"], TEXT_MAX, f.why);
}

bool parseOptions(JsonObjectConst line, StreamFields& f, const uint8_t minCount, const uint8_t maxCount) {
  return textField(line["prompt"], BODY_MAX, f.title) && shortList(line["options"], minCount, maxCount, f) &&
         answerIndex(line["answer"], f) && optionalText(line["why"], TEXT_MAX, f.why);
}

bool parseOdd(JsonObjectConst line, StreamFields& f, Item&) { return parseOptions(line, f, ODD_MIN, ODD_MAX); }

bool parseLogic(JsonObjectConst line, StreamFields& f, Item&) {
  return parseOptions(line, f, OPTIONS_MIN, OPTIONS_MAX);
}

void forecastRows(StreamFields& f) {
  static constexpr const char* ROWS[FORECAST_ROWS] = {"10%", "30%", "50%", "70%", "90%"};
  for (uint8_t i = 0; i < FORECAST_ROWS; ++i) f.rows[i] = ROWS[i];
  f.count = FORECAST_ROWS;
}

// "2027-05-23" as "23 May 2027", the day a forecast is settled.
bool settledBy(JsonVariantConst value, StreamFields& f) {
  static constexpr const char* MONTHS[12] = {"January", "February", "March",     "April",   "May",      "June",
                                             "July",    "August",   "September", "October", "November", "December"};
  std::string_view day;
  if (!textField(value, 10, day) || day.size() != 10 || day[4] != '-' || day[7] != '-') return false;
  const int year = atoi(day.data());
  const int month = atoi(day.data() + 5);
  const int date = atoi(day.data() + 8);
  if (year < 2000 || month < 1 || month > 12 || date < 1 || date > 31) return false;
  const int n = snprintf(f.scratch, sizeof(f.scratch), "%d %s %d", date, MONTHS[month - 1], year);
  if (n <= 0 || static_cast<size_t>(n) >= sizeof(f.scratch)) return false;
  f.after = {f.scratch, static_cast<size_t>(n)};
  return true;
}

// A statement about what happens next, and the day that settles it.
bool parseForecast(JsonObjectConst line, StreamFields& f) {
  forecastRows(f);
  return textField(line["text"], TEXT_MAX, f.title) && settledBy(line["by"], f);
}

// Copies what a parser read off a line into the arena.
bool store(ArenaWriter& writer, const StreamFields& f, Item& out) {
  bool ok = (f.after.empty() ? writer.put(f.title, out.title) : writer.put3(f.title, "\n\n", f.after, out.title)) &&
            writer.put(f.body, out.body) && writer.put(f.book, out.book) && writer.put(f.why, out.why) &&
            writer.put(f.linkPrompt, out.linkPrompt) && writer.put(f.linkWhy, out.linkWhy);
  for (uint8_t i = 0; ok && i < LIST_MAX; ++i) {
    if (i < f.count) ok = writer.put(f.rows[i], out.options[i]);
    if (ok && !f.links[i].empty()) ok = writer.put(f.links[i], out.links[i]);
  }
  out.optionCount = f.count;
  out.answer = f.answer;
  out.linkAnswer = f.linkAnswer;
  return ok;
}

// ---- the table: one row per Shape, in its order -------------------------------

struct Row {
  StepKind kind;
  Parser parse;  // null for the card types (readCard)
};

constexpr Row KINDS[] = {
    {{"flip", Layout::Answer, Result::Graded}, nullptr},
    {{"choice", Layout::Options, Result::Graded}, nullptr},
    {{"cloze", Layout::Options, Result::Graded}, nullptr},
    {{"explain", Layout::Points, Result::Graded}, nullptr},
    {{"bite", Layout::Text, Result::Seen}, &parseText},
    {{"highlight", Layout::Quote, Result::Seen}, &parseHighlight},
    {{"riddle", Layout::Answer, Result::Solved}, &parseRiddle},
    {{"order", Layout::Order, Result::Solved}, &parseOrder},
    {{"odd", Layout::Options, Result::Solved}, &parseOdd},
    {{"logic", Layout::Options, Result::Solved}, &parseLogic},
    // A piece's parts (parsePiece): no stream line has them. The article itself is never on screen.
    {{"article", Layout::Options, Result::Seen}, nullptr},
    {{"article", Layout::Options, Result::Seen}, nullptr},
    {{"article", Layout::Options, Result::Seen}, nullptr},
    {{"forecast", Layout::Options, Result::Seen}, nullptr},
};
constexpr size_t KIND_COUNT = sizeof(KINDS) / sizeof(KINDS[0]);
static_assert(KIND_COUNT == static_cast<size_t>(Shape::Forecast) + 1, "one row per shape");

}  // namespace

const StepKind& kindOf(const Shape shape) { return KINDS[static_cast<uint8_t>(shape)].kind; }

uint64_t cardKeyHash(const char* deckId, const char* cardId) {
  uint64_t hash = fnv1a64(deckId, std::strlen(deckId));
  hash = fnv1a64("/", 1, hash);
  return fnv1a64(cardId, std::strlen(cardId), hash);
}

uint64_t itemIdHash(const char* id) { return fnv1a64(id, std::strlen(id)); }

bool scanCard(const char* line, const size_t length, char (&id)[ID_MAX + 1], CardType& type) {
  JsonDocument doc;
  if (!parseLine(doc, line, length)) return false;
  return readCard(doc.as<JsonObjectConst>(), id, type, nullptr, nullptr);
}

bool parseCard(char* arena, const size_t length, Item& out) {
  JsonDocument doc;
  if (!parseLine(doc, arena, length)) return false;
  out = Item{};
  ArenaWriter writer(arena);
  char id[ID_MAX + 1];
  CardType type{};
  return readCard(doc.as<JsonObjectConst>(), id, type, &out, &writer);
}

bool scanId(const char* line, const size_t length, char (&id)[ID_MAX + 1]) {
  JsonDocument filter;
  filter["id"] = true;
  JsonDocument doc;
  if (deserializeJson(doc, line, length, DeserializationOption::Filter(filter))) return false;
  return copyId(doc["id"], id);
}

bool parseStreamItem(char* arena, const size_t length, Item& out) {
  JsonDocument doc;
  if (!parseLine(doc, arena, length)) return false;
  const JsonObjectConst line = doc.as<JsonObjectConst>();
  out = Item{};
  const char* kind = line["kind"].as<const char*>();
  if (!copyId(line["id"], out.id) || !kind) return false;
  const Row* row = nullptr;
  for (size_t i = 0; i < KIND_COUNT && !row; ++i) {
    if (KINDS[i].parse && std::strcmp(kind, KINDS[i].kind.name) == 0) {
      row = &KINDS[i];
      out.shape = static_cast<Shape>(i);
    }
  }
  if (!row) return false;

  StreamFields f;
  if (!optionalText(line["book"], TEXT_MAX, f.book) || !row->parse(line, f, out)) return false;
  ArenaWriter writer(arena);
  return store(writer, f, out);
}

bool parsePiece(char* arena, const size_t length, Item& article, Item& forecast, char (&thread)[ID_MAX + 1]) {
  JsonDocument doc;
  if (!parseLine(doc, arena, length)) return false;
  const JsonObjectConst line = doc.as<JsonObjectConst>();
  article = Item{};
  forecast = Item{};
  thread[0] = '\0';
  article.shape = Shape::Article;
  StreamFields f;
  if (!copyId(line["id"], article.id) || !optionalText(line["book"], TEXT_MAX, f.book) ||
      !parseArticle(line, f)) {
    return false;
  }
  if (!line["thread"].isNull() && !copyId(line["thread"], thread)) return false;
  StreamFields ff;
  const JsonVariantConst carried = line["forecast"];
  const bool forecasts = carried.is<JsonObjectConst>();
  if (forecasts) {
    forecast.shape = Shape::Forecast;
    if (!copyId(carried["id"], forecast.id) || !parseForecast(carried.as<JsonObjectConst>(), ff)) return false;
    const char* by = carried["by"].as<const char*>();  // YYYY-MM-DD, as settledBy() has checked
    forecast.settles = static_cast<uint32_t>(atoi(by) * 10000 + atoi(by + 5) * 100 + atoi(by + 8));
  }
  ArenaWriter writer(arena);
  return store(writer, f, article) && (!forecasts || store(writer, ff, forecast));
}

bool parseHighlightInto(char* arena, const size_t length, Item& item) {
  JsonDocument doc;
  if (!parseLine(doc, arena, length)) return false;
  const JsonObjectConst line = doc.as<JsonObjectConst>();
  std::string_view body;
  std::string_view title;
  std::string_view book;
  if (!textField(line["text"], BODY_MAX, body) || !optionalText(line["title"], TEXT_MAX, title) ||
      !optionalText(line["book"], TEXT_MAX, book)) {
    return false;
  }
  item.shape = Shape::Highlight;
  ArenaWriter writer(arena);
  return writer.put(title, item.title) && writer.put(body, item.body) && writer.put(book, item.book);
}

}  // namespace x4
