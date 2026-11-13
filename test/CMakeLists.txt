aux_source_directory(. rime_test_src)
list(REMOVE_ITEM rime_test_src ./config_deployment_test.cc)
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_BINARY_DIR}/test)
add_executable(rime_test ${rime_test_src})
target_link_libraries(rime_test
  ${rime_library}
  ${rime_dict_library}
  ${rime_gears_library}
  ${GTEST_LIBRARIES})
if(BUILD_SHARED_LIBS AND BUILD_SEPARATE_LIBS)
  target_link_libraries(rime_test ${rime_levers_library})
endif()
if(BUILD_SHARED_LIBS)
  target_compile_definitions(rime_test PRIVATE RIME_IMPORTS)
endif(BUILD_SHARED_LIBS)
if(NOT Opencc_DICT_DIR)
  message(FATAL_ERROR "Could not locate the installed opencc dict directory "
                       "(e.g. t2s.json); required to build rime_test.")
endif()
target_compile_definitions(rime_test PRIVATE
  RIME_OPENCC_DICT_DIR="${Opencc_DICT_DIR}")

file(GLOB test_data_files ${PROJECT_SOURCE_DIR}/data/test/*.yaml)
file(COPY ${test_data_files} DESTINATION ${EXECUTABLE_OUTPUT_PATH})

add_test(NAME rime_test
  COMMAND rime_test
  WORKING_DIRECTORY ${EXECUTABLE_OUTPUT_PATH})
set_tests_properties(rime_test PROPERTIES ENVIRONMENT
  "GLOG_alsologtostderr=1;GLOG_log_dir=${EXECUTABLE_OUTPUT_PATH}")

# Deployment needs separate source and output directories from the other tests.
add_executable(config_deployment_test config_deployment_test.cc)
target_link_libraries(config_deployment_test ${rime_library} ${GTEST_LIBRARIES})
if(BUILD_SHARED_LIBS AND BUILD_SEPARATE_LIBS)
  target_link_libraries(config_deployment_test
    ${rime_dict_library} ${rime_gears_library} ${rime_levers_library})
endif()
if(BUILD_SHARED_LIBS)
  target_compile_definitions(config_deployment_test PRIVATE RIME_IMPORTS)
endif()
add_test(NAME config_deployment_test COMMAND config_deployment_test)
