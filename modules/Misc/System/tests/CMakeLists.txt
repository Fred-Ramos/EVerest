set(TEST_TARGET_NAME ${PROJECT_NAME}_System_tests)

add_executable(${TEST_TARGET_NAME}
    LogFilesTest.cpp
    ../main/log_files.cpp
)

target_include_directories(${TEST_TARGET_NAME} PRIVATE
    ../main
)

target_link_libraries(${TEST_TARGET_NAME} PRIVATE
    GTest::gtest_main
    everest::run_application
)

add_test(${TEST_TARGET_NAME} ${TEST_TARGET_NAME})
ev_register_test_target(${TEST_TARGET_NAME})
