#!/bin/bash
set -e  # Exit immediately if a command exits with a non-zero status

# Colori per output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Stampa intestazione
echo -e "${BLUE}========================================${NC}"
echo -e "${BLUE}     FAYASM BUILD AND TEST SCRIPT      ${NC}"
echo -e "${BLUE}========================================${NC}"

# Controlla se la directory src esiste
if [ ! -d "../src" ]; then
    echo -e "${RED}Errore: La directory ../src non esiste!${NC}"
    echo "Assicurati di eseguire questo script dalla directory 'test'"
    exit 1
fi

# Crea e vai nella directory di build
echo -e "${YELLOW}Creazione directory di build...${NC}"
BUILD_DIR="../build"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
echo -e "${GREEN}Directory di build creata: $(pwd)${NC}"

# Configura con CMake
echo -e "${YELLOW}Configurazione con CMake...${NC}"
cmake .. -DFAYASM_BUILD_TESTS=ON
if [ $? -ne 0 ]; then
    echo -e "${RED}Errore durante la configurazione con CMake!${NC}"
    exit 1
fi
echo -e "${GREEN}Configurazione completata con successo!${NC}"

# Compila
echo -e "${YELLOW}Compilazione in corso...${NC}"
make
if [ $? -ne 0 ]; then
    echo -e "${RED}Errore durante la compilazione!${NC}"
    exit 1
fi
echo -e "${GREEN}Compilazione completata con successo!${NC}"

# Esegui i test
echo -e "${YELLOW}Esecuzione dei test in corso...${NC}"
echo -e "${BLUE}----------------------------------------${NC}"
make check
TEST_RESULT=$?
echo -e "${BLUE}----------------------------------------${NC}"

if [ $TEST_RESULT -eq 0 ]; then
    echo -e "${GREEN}Tutti i test sono stati eseguiti con successo!${NC}"
else
    echo -e "${RED}Alcuni test sono falliti. Controlla i log sopra per i dettagli.${NC}"
    exit 1
fi

# Torna alla directory originale
cd - > /dev/null

echo -e "${BLUE}========================================${NC}"
echo -e "${GREEN}Build e test completati con successo!${NC}"
echo -e "${BLUE}========================================${NC}"

# Informazioni aggiuntive
echo -e "${YELLOW}Informazioni:${NC}"
echo -e "- Cartella build: ${BUILD_DIR}"
echo -e "- Eseguibile di test: ${BUILD_DIR}/bin/fayasm_test_main"
echo -e "- Per eseguire i test manualmente: cd ${BUILD_DIR} && ctest --verbose"

exit 0