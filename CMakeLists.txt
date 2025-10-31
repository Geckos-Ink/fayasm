cmake_minimum_required(VERSION 3.10)
project(fayasm VERSION 0.1.0 DESCRIPTION "Experimental, lightweight WebAssembly executor focused on transparency and portability over raw speed." LANGUAGES C)

# Opzioni di compilazione
option(FAYASM_BUILD_SHARED "Build as shared library" ON)
option(FAYASM_BUILD_STATIC "Build as static library" ON)
option(FAYASM_BUILD_TESTS "Build tests" ON)

# Impostazioni standard di compilazione
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Configurazione per le compilazioni di debug e release
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -DDEBUG -Wall -Wextra -Wpedantic")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3 -DNDEBUG")

# Verifica il sistema operativo e imposta macro specifiche
if(UNIX AND NOT APPLE)
    add_definitions(-DFAYASM_LINUX)
elseif(APPLE)
    add_definitions(-DFAYASM_MACOS)
elseif(WIN32)
    message(WARNING "Windows is not supported, may not work properly")
    add_definitions(-DFAYASM_WINDOWS)
endif()

# Imposta la directory di output per i binari
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/test/bin)

# Passa al sottomodulo della libreria principale
add_subdirectory(src)

# Aggiungi i test se richiesto
if(FAYASM_BUILD_TESTS)
    enable_testing()
    add_subdirectory(test)
endif()

# Pacchettizzazione
set(CPACK_PACKAGE_NAME "fayasm")
set(CPACK_PACKAGE_VENDOR "Gecko's Ink")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "fayasm - WASM-like runtime")
set(CPACK_PACKAGE_VERSION ${PROJECT_VERSION})
set(CPACK_PACKAGE_VERSION_MAJOR ${PROJECT_VERSION_MAJOR})
set(CPACK_PACKAGE_VERSION_MINOR ${PROJECT_VERSION_MINOR})
set(CPACK_PACKAGE_VERSION_PATCH ${PROJECT_VERSION_PATCH})
set(CPACK_GENERATOR "TGZ;DEB;RPM")
include(CPack)