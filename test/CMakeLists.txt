# CMakeLists.txt per la directory test/
cmake_minimum_required(VERSION 3.10)

# Trova tutti i file di test
file(GLOB TEST_SOURCES "*.c")

# Crea un eseguibile per main.c
add_executable(fayasm_test_main main.c)

# Imposta le proprietà dell'eseguibile
set_target_properties(fayasm_test_main PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)

# Includi la directory principale per gli header
target_include_directories(fayasm_test_main PRIVATE 
    ${CMAKE_SOURCE_DIR}/src
)

# Collega l'eseguibile alla libreria fayasm
# Usa i target definiti nella directory src
if(TARGET fayasm)
    message(STATUS "Usando la libreria condivisa fayasm")
    target_link_libraries(fayasm_test_main PRIVATE fayasm)
elseif(TARGET fayasm_static)
    message(STATUS "Usando la libreria statica fayasm")
    target_link_libraries(fayasm_test_main PRIVATE fayasm_static)
else()
    message(FATAL_ERROR "Non è stata trovata la libreria fayasm. Assicurati di compilarla prima.")
endif()

# Aggiungi altri test se necessario
add_custom_target(check
    COMMAND ${CMAKE_BINARY_DIR}/bin/fayasm_test_main
    DEPENDS fayasm_test_main
)