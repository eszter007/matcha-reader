find_package(ZLIB REQUIRED)  # builds the test's dictzip file

add_executable(DictZipTest
  DictZipTest.cpp
  ${REPO_ROOT}/src/util/DictZip.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c
)

# The stubs directory comes first so <HalStorage.h> resolves to the host stand-in.
target_include_directories(DictZipTest BEFORE PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/uzlib/src
)

target_link_libraries(DictZipTest PRIVATE ZLIB::ZLIB)

# Extra arguments check a real dictionary: <file.dict.dz> <file.dict> <offset> <size>.
add_test(NAME DictZip COMMAND DictZipTest WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
