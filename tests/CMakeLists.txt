find_package(Qt6 REQUIRED COMPONENTS Test)
find_package(Python3 REQUIRED COMPONENTS Interpreter)

function(haul_add_test name)
    add_executable(${name} ${name}.cpp ${ARGN})
    target_link_libraries(${name} PRIVATE haul_engine Qt6::Test)
    target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_compile_definitions(${name} PRIVATE
        HAUL_TEST_SERVER="${CMAKE_CURRENT_SOURCE_DIR}/server/test_server.py"
        HAUL_PYTHON="${Python3_EXECUTABLE}")
    add_test(NAME ${name} COMMAND ${name})
    set_tests_properties(${name} PROPERTIES
        TIMEOUT 300
        ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
endfunction()

haul_add_test(tst_core)
haul_add_test(tst_queue)
haul_add_test(tst_storage)
haul_add_test(tst_network)
haul_add_test(tst_manager)

if(TARGET haul_ui)
    haul_add_test(tst_ui)
    target_link_libraries(tst_ui PRIVATE haul_ui)
endif()
