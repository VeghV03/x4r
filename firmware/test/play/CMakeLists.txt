# ArduinoJson is header-only: use the copy PlatformIO already fetched, else fetch it.
set(PLAY_ARDUINOJSON_DIR "${REPO_ROOT}/.pio/libdeps/default/ArduinoJson/src")
if(NOT EXISTS "${PLAY_ARDUINOJSON_DIR}/ArduinoJson.h")
  FetchContent_Declare(
    arduinojson
    GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
    GIT_TAG v7.4.2
  )
  FetchContent_GetProperties(arduinojson)
  if(NOT arduinojson_POPULATED)
    FetchContent_Populate(arduinojson)
  endif()
  set(PLAY_ARDUINOJSON_DIR "${arduinojson_SOURCE_DIR}/src")
endif()

set(PLAY_SRC ${REPO_ROOT}/src/play)
set(PLAY_LEARN_SRC ${REPO_ROOT}/src/learn)
add_executable(PlayTest
  PlayTest.cpp
  NonogramTest.cpp
  ${PLAY_SRC}/Play.cpp
  ${PLAY_SRC}/games/Sudoku.cpp
  ${PLAY_SRC}/games/Nonogram.cpp
  ${PLAY_SRC}/games/Einstein.cpp
  ${PLAY_SRC}/games/MemoryGames.cpp
  ${PLAY_LEARN_SRC}/Journal.cpp
  ${PLAY_LEARN_SRC}/StateFile.cpp
  ${PLAY_LEARN_SRC}/Item.cpp
)
target_link_libraries(PlayTest PRIVATE crosspoint_test_common GTest::gtest_main)
# The learn stubs stand in for HalStorage, Logging and Memory.
target_include_directories(PlayTest PRIVATE
  ${REPO_ROOT}/test/learn/stubs
  ${REPO_ROOT}/src
  ${PLAY_LEARN_SRC}
  ${PLAY_ARDUINOJSON_DIR}
)
target_compile_definitions(PlayTest PRIVATE X4_LEARN_HOST_TEST=1)
gtest_discover_tests(PlayTest TEST_PREFIX "PlayTest.")

add_executable(WordsTest
  WordsTest.cpp
  ${PLAY_SRC}/Play.cpp
  ${PLAY_SRC}/games/Words.cpp
  ${PLAY_LEARN_SRC}/Journal.cpp
  ${PLAY_LEARN_SRC}/StateFile.cpp
  ${PLAY_LEARN_SRC}/Item.cpp
)
target_link_libraries(WordsTest PRIVATE crosspoint_test_common GTest::gtest_main)
target_include_directories(WordsTest PRIVATE
  ${REPO_ROOT}/test/learn/stubs
  ${REPO_ROOT}/src
  ${PLAY_LEARN_SRC}
  ${PLAY_ARDUINOJSON_DIR}
)
target_compile_definitions(WordsTest PRIVATE X4_LEARN_HOST_TEST=1)
gtest_discover_tests(WordsTest TEST_PREFIX "WordsTest.")

add_executable(ChessTest
  ChessTest.cpp
  ${PLAY_SRC}/games/Chess.cpp
)
target_link_libraries(ChessTest PRIVATE crosspoint_test_common GTest::gtest_main)
target_include_directories(ChessTest PRIVATE ${REPO_ROOT}/src)
gtest_discover_tests(ChessTest TEST_PREFIX "ChessTest.")
