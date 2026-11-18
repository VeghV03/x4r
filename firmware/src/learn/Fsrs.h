#pragma once

#include <cstdint>

#include "LearnTypes.h"

// FSRS-6 with the published default parameters. Time is measured in sets; one
// set counts as one day. Pure C++, no Arduino dependencies.
namespace x4::fsrs {

enum class Grade : uint8_t { Again = 1, Hard = 2, Good = 3, Easy = 4 };

struct Memory {
  float stability;
  float difficulty;
};

inline constexpr float DESIRED_RETENTION = 0.9f;
inline constexpr uint32_t MAX_INTERVAL = 36500;

// Memory after the first answer to a new card.
Memory initial(Grade grade);
// Probability of recall `elapsed` sets after the last review.
float retrievability(float elapsed, float stability);
// Memory after answering a reviewed card `elapsed` sets after its last review.
Memory next(Memory memory, float elapsed, Grade grade);
// Sets until recall probability falls to `retention` (at least 1).
uint32_t intervalFor(float stability, float retention = DESIRED_RETENTION);

// Schedules one answer given in `set`: updates memory, counters and due set.
void schedule(CardRecord& record, uint32_t set, Grade grade);
// Retrievability of a reviewed card at `set`; 0 for a new card.
float recordRetrievability(const CardRecord& record, uint32_t set);

}  // namespace x4::fsrs
