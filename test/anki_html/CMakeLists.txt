add_executable(AnkiHtmlTest
  AnkiHtmlTest.cpp
  ${REPO_ROOT}/lib/AnkiHtml/AnkiHtml.cpp
  ${REPO_ROOT}/lib/Epub/Epub/htmlEntities.cpp
  ${REPO_ROOT}/lib/expat/xmlparse.c
  ${REPO_ROOT}/lib/expat/xmlrole.c
  ${REPO_ROOT}/lib/expat/xmltok.c
)

target_include_directories(AnkiHtmlTest PRIVATE
  ${REPO_ROOT}/lib/AnkiHtml
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/expat
)

target_compile_definitions(AnkiHtmlTest PRIVATE
  XML_CONTEXT_BYTES=1024
  XML_GE=0
)

target_link_libraries(AnkiHtmlTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(AnkiHtmlTest)
