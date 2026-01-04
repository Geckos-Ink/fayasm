cmake_minimum_required(VERSION 3.10)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Impostazioni standard di compilazione
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Trova tutti i file sorgente
file(GLOB FAYASM_SOURCES "fa_*.c")
set(FAYASM_HEADERS "fayasm.h")
file(GLOB FAYASM_HEADERS_EXTRA "fa_*.h")
file(GLOB FAYASM_HEADERS_EXTRA "helpers/*.h")

## Advised best practice to include files
#set(FAYASM_HEADERS_EXTRA
#    helpers/file1.h
#    helpers/file2.h
#    helpers/another_file.h
#)
#add_library(my_lib ${FAYASM_HEADERS_EXTRA})

# Configurazione per le compilazioni di debug e release
set(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -DDEBUG -Wall -Wextra -Wpedantic -ferror-limit=100")
set(CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE} -O3 -DNDEBUG -ferror-limit=100")

# Verifica il sistema operativo e imposta macro specifiche
if(UNIX AND NOT APPLE)
    add_definitions(-DFAYASM_LINUX)
elseif(APPLE)
    add_definitions(-DFAYASM_MACOS)
elseif(WIN32)
    message(WARNING "Windows is not supported, may not work properly")
    add_definitions(-DFAYASM_WINDOWS)
endif()

if(FAYASM_TARGET_ESP32)
    add_definitions(-DFAYASM_TARGET_ESP32)
endif()

# Crea la libreria condivisa
if(FAYASM_BUILD_SHARED)
    add_library(fayasm SHARED ${FAYASM_SOURCES})
    set_target_properties(fayasm PROPERTIES
        VERSION ${PROJECT_VERSION}
        SOVERSION ${PROJECT_VERSION_MAJOR}
        PUBLIC_HEADER "${FAYASM_HEADERS};${FAYASM_HEADERS_EXTRA}"
    )
    target_include_directories(fayasm PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
    )
endif()

# Crea la libreria statica
if(FAYASM_BUILD_STATIC)
    add_library(fayasm_static STATIC ${FAYASM_SOURCES})
    set_target_properties(fayasm_static PROPERTIES
        OUTPUT_NAME fayasm
        PUBLIC_HEADER "${FAYASM_HEADERS};${FAYASM_HEADERS_EXTRA}"
    )
    target_include_directories(fayasm_static PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
    )
endif()

# Librerie di sistema necessarie (esempio: pthread, rt, m)
find_package(Threads REQUIRED)
if(FAYASM_BUILD_SHARED)
    target_link_libraries(fayasm PUBLIC ${CMAKE_THREAD_LIBS_INIT})
    if(NOT APPLE)
        target_link_libraries(fayasm PUBLIC rt m)
    else()
        target_link_libraries(fayasm PUBLIC m)
    endif()
endif()

if(FAYASM_BUILD_STATIC)
    target_link_libraries(fayasm_static PUBLIC ${CMAKE_THREAD_LIBS_INIT})
    if(NOT APPLE)
        target_link_libraries(fayasm_static PUBLIC rt m)
    else()
        target_link_libraries(fayasm_static PUBLIC m)
    endif()
endif()

# Installazione
include(GNUInstallDirs)

# Installa le librerie
if(FAYASM_BUILD_SHARED)
    install(TARGETS fayasm
        EXPORT fayasm-targets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/fayasm
    )
endif()

if(FAYASM_BUILD_STATIC)
    install(TARGETS fayasm_static
        EXPORT fayasm-targets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        PUBLIC_HEADER DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/fayasm
    )
endif()
