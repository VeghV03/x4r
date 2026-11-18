# ArduinoJson is header-only: use the copy PlatformIO already fetched, else fetch it.
set(LEARN_ARDUINOJSON_DIR "${REPO_ROOT}/.pio/libdeps/default/ArduinoJson/src")
if(NOT EXISTS "${LEARN_ARDUINOJSON_DIR}/ArduinoJson.h")
  FetchContent_Declare(
    arduinojson
    GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
    GIT_TAG v7.4.2
  )
  FetchContent_GetProperties(arduinojson)
  if(NOT arduinojson_POPULATED)
    FetchContent_Populate(arduinojson)
  endif()
  set(LEARN_ARDUINOJSON_DIR "${arduinojson_SOURCE_DIR}/src")
endif()

set(LEARN_SRC ${REPO_ROOT}/src/learn)
# The learning core as the host builds it: everything of the steps and the paper but drawing.
set(LEARN_CORE
  ${LEARN_SRC}/FeedText.cpp
  ${LEARN_SRC}/FeedHighlights.cpp
  ${REPO_ROOT}/src/clippings/Sentences.cpp
  ${REPO_ROOT}/src/clippings/SentenceSteps.cpp
  ${LEARN_SRC}/Fsrs.cpp
  ${LEARN_SRC}/SetRules.cpp
  ${LEARN_SRC}/JsonlReader.cpp
  ${LEARN_SRC}/Item.cpp
  ${LEARN_SRC}/Journal.cpp
  ${LEARN_SRC}/StateFile.cpp
  ${LEARN_SRC}/DeckStore.cpp
  ${LEARN_SRC}/BookStats.cpp
  ${LEARN_SRC}/NextBooks.cpp
  ${LEARN_SRC}/Previously.cpp
  ${LEARN_SRC}/Learn.cpp
  ${LEARN_SRC}/Visits.cpp
  ${LEARN_SRC}/StreamInput.cpp
  ${LEARN_SRC}/Puzzle.cpp
  ${LEARN_SRC}/ReadList.cpp
  ${LEARN_SRC}/Paper.cpp
  ${LEARN_SRC}/StepRun.cpp
  ${LEARN_SRC}/Threads.cpp
  PaperPlayer.cpp
)
set(LEARN_INCLUDES stubs ${LEARN_SRC} ${LEARN_ARDUINOJSON_DIR} ${REPO_ROOT}/src)

add_executable(LearnTest
  LearnTest.cpp
  StreamInputTest.cpp
  StreamV2Test.cpp
  FeedHighlightTest.cpp
  PaperTest.cpp
  ${LEARN_CORE}
)
target_link_libraries(LearnTest PRIVATE crosspoint_test_common GTest::gtest_main)
target_include_directories(LearnTest PRIVATE ${LEARN_INCLUDES})
gtest_discover_tests(LearnTest TEST_PREFIX "LearnTest.")
target_compile_definitions(LearnTest PRIVATE X4_LEARN_HOST_TEST=1)

# The paper as the X4 plays it, over a folder that stands for the SD card: `x4 sim --play`
# sends it presses (companion/src/x4/sim.py).
add_executable(x4play x4play.cpp ${LEARN_CORE})
target_link_libraries(x4play PRIVATE crosspoint_test_common)
target_include_directories(x4play PRIVATE ${LEARN_INCLUDES})
target_compile_definitions(x4play PRIVATE X4_LEARN_HOST_TEST=1)
