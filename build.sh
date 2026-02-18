#!/bin/bash
set -e  # Exit immediately if a command exits with a non-zero status

if [ -n "${TERM:-}" ] && [ "${TERM}" != "dumb" ]; then
    clear
fi

# Colori per output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Funzione per DEBUG output
DEBUG_MODE=true
debug_print() {
    if [ "$DEBUG_MODE" = true ]; then
        echo -e "${BLUE}[DEBUG] $1${NC}"
    fi
}

# Stampa intestazione
echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}        FAYASM BUILD SCRIPT            ${NC}"
echo -e "${BLUE}========================================${NC}"

# Build optional wasm sample fixtures when Emscripten is available
if command -v emcc >/dev/null 2>&1; then
    echo -e "${YELLOW}Compilazione fixture WASM (wasm_samples/) con emcc...${NC}"
    if ./wasm_samples/build.sh; then
        debug_print "Fixture WASM compilate in wasm_samples/build/"
    else
        echo -e "${RED}Errore durante la compilazione delle fixture WASM.${NC}"
        exit 1
    fi
else
    echo -e "${YELLOW}emcc non trovato: salto la compilazione di wasm_samples (i test fixture verranno segnati come SKIP).${NC}"
fi

# Directory di build
BUILD_DIR="$(pwd)/build"

# Pulisci la directory di build se esiste
if [ -d "$BUILD_DIR" ]; then
    echo -e "${YELLOW}Pulizia della directory di build esistente...${NC}"
    rm -rf "$BUILD_DIR"
    debug_print "Directory di build rimossa"
fi

# Crea directory di build
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
echo -e "${GREEN}Directory di build: $(pwd)${NC}"

# Argomenti di default per CMake
CMAKE_ARGS="-DFAYASM_BUILD_TESTS=ON -DFAYASM_BUILD_SHARED=ON -DFAYASM_BUILD_STATIC=ON"

# Controlla se ci sono argomenti aggiuntivi
if [ $# -gt 0 ]; then
    CMAKE_ARGS="$CMAKE_ARGS $@"
fi

# Configura con CMake
echo -e "${YELLOW}Configurazione con CMake...${NC}"
echo -e "${YELLOW}Argomenti CMake: ${CMAKE_ARGS}${NC}"
cmake .. ${CMAKE_ARGS}
if [ $? -ne 0 ]; then
    echo -e "${RED}Errore durante la configurazione con CMake!${NC}"
    exit 1
fi
echo -e "${GREEN}Configurazione completata con successo!${NC}"

# Compila la libreria fayasm
echo -e "${YELLOW}Compilazione della libreria fayasm in corso...${NC}"
make fayasm
if [ $? -ne 0 ]; then
    # Prova con la libreria statica se la condivisa fallisce
    echo -e "${YELLOW}Tentativo di compilare la libreria statica...${NC}"
    make fayasm_static
    if [ $? -ne 0 ]; then
        echo -e "${RED}Errore durante la compilazione della libreria!${NC}"
        exit 1
    fi
fi
debug_print "Libreria compilata. Contenuto della directory lib:"
debug_print "$(ls -la ${BUILD_DIR}/lib/)"

# Compila i test
echo -e "${YELLOW}Compilazione dei test in corso...${NC}"
make fayasm_test_main
if [ $? -ne 0 ]; then
    echo -e "${RED}Errore durante la compilazione dei test!${NC}"
    exit 1
fi
echo -e "${GREEN}Compilazione completata con successo!${NC}"
debug_print "Eseguibile di test creato. Contenuto della directory bin:"
debug_print "$(ls -la ${BUILD_DIR}/bin/)"

# Esegui i test se abilitati
if [ -f "bin/fayasm_test_main" ]; then
    echo -e "${YELLOW}Esecuzione dei test in corso...${NC}"
    echo -e "${BLUE}----------------------------------------${NC}"
    bin/fayasm_test_main
    TEST_RESULT=$?
    echo -e "${BLUE}----------------------------------------${NC}"

    if [ $TEST_RESULT -eq 0 ]; then
        echo -e "${GREEN}Tutti i test sono stati eseguiti con successo!${NC}"
    else
        echo -e "${RED}Alcuni test sono falliti. Controlla i log sopra per i dettagli.${NC}"
        exit 1
    fi
else
    echo -e "${RED}L'eseguibile dei test non è stato trovato!${NC}"
    debug_print "Contenuto della directory bin: $(ls -la bin/)"
    exit 1
fi

echo -e "${BLUE}========================================${NC}"
echo -e "${GREEN}Build completata con successo!${NC}"
echo -e "${BLUE}========================================${NC}"

# Informazioni aggiuntive
echo -e "${YELLOW}Informazioni:${NC}"
echo -e "- Libreria compilata in: ${BUILD_DIR}/lib/"
echo -e "- Eseguibile di test: ${BUILD_DIR}/bin/fayasm_test_main"
echo -e "- Per eseguire i test manualmente: cd ${BUILD_DIR} && bin/fayasm_test_main"

exit 0
