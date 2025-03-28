# CMakeLists.txt per la directory test/

# Trova tutti i file di test
file(GLOB TEST_SOURCES "*.c")

# Crea un eseguibile per main.c
add_executable(fayasm_test_main main.c)

# Imposta le proprietà dell'eseguibile
set_target_properties(fayasm_test_main PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Collega l'eseguibile alla libreria fayasm
if(FAYASM_BUILD_SHARED)
    target_link_libraries(fayasm_test_main PRIVATE fayasm)
else()
    target_link_libraries(fayasm_test_main PRIVATE fayasm_static)
endif()

# Includi la directory principale per gli header
target_include_directories(fayasm_test_main PRIVATE 
    ${CMAKE_SOURCE_DIR}/include
)

# Aggiungi il test al sistema di test di CMake
add_test(
    NAME fayasm_main_test
    COMMAND fayasm_test_main
    WORKING_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# Imposta le variabili d'ambiente per il test se necessario
# set_tests_properties(fayasm_main_test PROPERTIES
#    ENVIRONMENT "VAR1=value1;VAR2=value2"
# )

# Se hai bisogno di creare altri eseguibili di test, puoi aggiungere:
# foreach(test_source ${TEST_SOURCES})
#     if(NOT ${test_source} STREQUAL "main.c")
#         get_filename_component(test_name ${test_source} NAME_WE)
#         add_executable(${test_name} ${test_source})
#         target_link_libraries(${test_name} PRIVATE fayasm)
#         target_include_directories(${test_name} PRIVATE ${CMAKE_SOURCE_DIR}/include)
#         add_test(NAME ${test_name}_test COMMAND ${test_name})
#     endif()
# endforeach()

# Aggiungi target per eseguire tutti i test
add_custom_target(check
    COMMAND ${CMAKE_CTEST_COMMAND} --verbose
    DEPENDS fayasm_test_main
)