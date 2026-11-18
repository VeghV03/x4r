#include "Fsrs.h"

#include <algorithm>
#include <cmath>

namespace x4::fsrs {

namespace {

// FSRS-6 default parameters (py-fsrs 6 / ts-fsrs 5).
constexpr double W[21] = {0.212,  1.2931, 2.3065, 8.2956, 6.4133, 0.8334, 3.0194, 0.001,  1.8722, 0.1666, 0.796,
                          1.4835, 0.0614, 0.2629, 1.6483, 0.6014, 1.8729, 0.5425, 0.0912, 0.0658, 0.1542};
constexpr double STABILITY_MIN = 0.001;
constexpr double DIFFICULTY_MIN = 1.0;
constexpr double DIFFICULTY_MAX = 10.0;

double decay() { return -W[20]; }
// Chosen so that retrievability(S, S) == 0.9.
double factor() { return std::pow(0.9, 1.0 / decay()) - 1.0; }

double gradeValue(const Grade grade) { return static_cast<double>(static_cast<uint8_t>(grade)); }

double initialDifficultyRaw(const Grade grade) { return W[4] - std::exp(W[5] * (gradeValue(grade) - 1.0)) + 1.0; }

double clampDifficulty(const double d) { return std::clamp(d, DIFFICULTY_MIN, DIFFICULTY_MAX); }

double clampStability(const double s) { return std::max(s, STABILITY_MIN); }

double nextDifficulty(const double d, const Grade grade) {
  const double delta = -W[6] * (gradeValue(grade) - 3.0);
  const double damped = d + delta * (10.0 - d) / 9.0;
  // Mean reversion towards the (unclamped) initial difficulty of Easy.
  return clampDifficulty(W[7] * initialDifficultyRaw(Grade::Easy) + (1.0 - W[7]) * damped);
}

double recallStability(const double d, const double s, const double r, const Grade grade) {
  const double hardPenalty = grade == Grade::Hard ? W[15] : 1.0;
  const double easyBonus = grade == Grade::Easy ? W[16] : 1.0;
  return s * (1.0 + std::exp(W[8]) * (11.0 - d) * std::pow(s, -W[9]) * (std::exp((1.0 - r) * W[10]) - 1.0) *
                        hardPenalty * easyBonus);
}

double forgetStability(const double d, const double s, const double r) {
  const double longTerm = W[11] * std::pow(d, -W[12]) * (std::pow(s + 1.0, W[13]) - 1.0) * std::exp((1.0 - r) * W[14]);
  const double shortTerm = s / std::exp(W[17] * W[18]);
  return std::min(longTerm, shortTerm);
}

double shortTermStability(const double s, const Grade grade) {
  const double next = s * std::exp(W[17] * (gradeValue(grade) - 3.0 + W[18])) * std::pow(s, -W[19]);
  return gradeValue(grade) >= 3.0 ? std::max(next, s) : next;
}

}  // namespace

Memory initial(const Grade grade) {
  const double s = clampStability(W[static_cast<uint8_t>(grade) - 1]);
  return {static_cast<float>(s), static_cast<float>(clampDifficulty(initialDifficultyRaw(grade)))};
}

float retrievability(const float elapsed, const float stability) {
  if (stability <= 0.0f) return 0.0f;
  const double t = std::max(0.0f, elapsed);
  return static_cast<float>(std::pow(1.0 + factor() * t / stability, decay()));
}

Memory next(const Memory memory, const float elapsed, const Grade grade) {
  const double d = memory.difficulty;
  const double s = clampStability(memory.stability);
  double nextS = 0.0;
  if (elapsed <= 0.0f) {
    nextS = shortTermStability(s, grade);
  } else {
    const double r = retrievability(elapsed, static_cast<float>(s));
    nextS = grade == Grade::Again ? forgetStability(d, s, r) : recallStability(d, s, r, grade);
  }
  return {static_cast<float>(clampStability(nextS)), static_cast<float>(nextDifficulty(d, grade))};
}

uint32_t intervalFor(const float stability, const float retention) {
  const double raw = stability / factor() * (std::pow(static_cast<double>(retention), 1.0 / decay()) - 1.0);
  const double rounded = std::round(raw);
  if (!(rounded >= 1.0)) return 1;
  return rounded > MAX_INTERVAL ? MAX_INTERVAL : static_cast<uint32_t>(rounded);
}

void schedule(CardRecord& record, const uint32_t set, const Grade grade) {
  Memory memory{};
  if (record.reps == 0) {
    memory = initial(grade);
  } else {
    const float elapsed = set > record.lastDay ? static_cast<float>(set - record.lastDay) : 0.0f;
    memory = next({record.stability, record.difficulty}, elapsed, grade);
    if (grade == Grade::Again && record.lapses < UINT8_MAX) ++record.lapses;
  }
  record.stability = memory.stability;
  record.difficulty = memory.difficulty;
  record.lastDay = set;
  record.dueDay = set + intervalFor(memory.stability);
  if (record.reps < UINT16_MAX) ++record.reps;
}

float recordRetrievability(const CardRecord& record, const uint32_t set) {
  if (record.reps == 0) return 0.0f;
  const float elapsed = set > record.lastDay ? static_cast<float>(set - record.lastDay) : 0.0f;
  return retrievability(elapsed, record.stability);
}

}  // namespace x4::fsrs
