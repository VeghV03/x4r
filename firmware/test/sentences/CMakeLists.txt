add_executable(SentencesTest SentencesTest.cpp ${REPO_ROOT}/src/clippings/Sentences.cpp)
target_link_libraries(SentencesTest PRIVATE crosspoint_test_common GTest::gtest_main)
target_include_directories(SentencesTest PRIVATE ${REPO_ROOT}/src/clippings)
gtest_discover_tests(SentencesTest)
