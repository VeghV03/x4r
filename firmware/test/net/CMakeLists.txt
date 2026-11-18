# ArduinoJson is header-only: use the copy PlatformIO already fetched, else fetch it.
set(NET_ARDUINOJSON_DIR "${REPO_ROOT}/.pio/libdeps/x4r/ArduinoJson/src")
if(NOT EXISTS "${NET_ARDUINOJSON_DIR}/ArduinoJson.h")
  set(NET_ARDUINOJSON_DIR "${REPO_ROOT}/.pio/libdeps/default/ArduinoJson/src")
endif()
if(NOT EXISTS "${NET_ARDUINOJSON_DIR}/ArduinoJson.h")
  FetchContent_Declare(
    arduinojson_net
    GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
    GIT_TAG v7.4.2
  )
  FetchContent_GetProperties(arduinojson_net)
  if(NOT arduinojson_net_POPULATED)
    FetchContent_Populate(arduinojson_net)
  endif()
  set(NET_ARDUINOJSON_DIR "${arduinojson_net_SOURCE_DIR}/src")
endif()

add_executable(NetTest
  NetTest.cpp
  ${REPO_ROOT}/src/net/Ical.cpp
  ${REPO_ROOT}/src/net/Agenda.cpp
  ${REPO_ROOT}/src/net/Issue.cpp
  ${REPO_ROOT}/src/net/Upload.cpp
  ${REPO_ROOT}/src/net/Football.cpp
)
target_link_libraries(NetTest PRIVATE crosspoint_test_common GTest::gtest_main)
target_include_directories(NetTest PRIVATE ${REPO_ROOT}/src ${NET_ARDUINOJSON_DIR})
gtest_discover_tests(NetTest TEST_PREFIX "NetTest.")
