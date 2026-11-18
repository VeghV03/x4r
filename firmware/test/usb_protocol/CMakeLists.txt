FetchContent_Declare(
  ArduinoJson
  GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
  GIT_TAG v7.4.2
)
FetchContent_MakeAvailable(ArduinoJson)

add_executable(UsbProtocolTest
  UsbProtocolTest.cpp
  ${REPO_ROOT}/src/usb/UsbProtocol.cpp
)

target_include_directories(UsbProtocolTest PRIVATE
  ${REPO_ROOT}/src
)

target_link_libraries(UsbProtocolTest PRIVATE
  crosspoint_test_common
  ArduinoJson
  GTest::gtest_main
)

gtest_discover_tests(UsbProtocolTest)
