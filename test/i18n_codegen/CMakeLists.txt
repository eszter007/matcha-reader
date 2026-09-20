find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_test(
  NAME I18nCodegenTest
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/I18nCodegenTest.py
          ${REPO_ROOT}/scripts/gen_i18n.py
)
