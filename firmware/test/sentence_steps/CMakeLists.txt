add_executable(SentenceStepsTest SentenceStepsTest.cpp ${REPO_ROOT}/src/clippings/Sentences.cpp
                                 ${REPO_ROOT}/src/clippings/SentenceSteps.cpp)
target_link_libraries(SentenceStepsTest PRIVATE crosspoint_test_common GTest::gtest_main)
target_include_directories(SentenceStepsTest PRIVATE ${REPO_ROOT}/src/clippings)
gtest_discover_tests(SentenceStepsTest)
