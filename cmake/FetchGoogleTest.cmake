#TODO maybe function

message(STATUS " Fetching GoogleTest...")
set(INSTALL_GTEST OFF)
set(BUILD_GMOCK OFF)
FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG        release-1.10.0
  )
FetchContent_GetProperties(googletest)
if(NOT googletest_POPULATED)
  FetchContent_Populate(googletest)
  set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
  add_subdirectory(${googletest_SOURCE_DIR} ${googletest_BINARY_DIR})
endif()
