#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h> // Per mmap, munmap
#include <unistd.h>   // Per sysconf (_SC_PAGESIZE)

// Definizione del tipo di funzione che vogliamo eseguire
typedef int (*my_function_type)(int, int);

int main() {
    // 1. Codice macchina per una funzione semplice: int add(int a, int b) { return a + b; }
    //    Questo è il codice assembly compilato per x86-64 (System V AMD64 ABI):
    //    push rbp        ; 55
    //    mov rbp, rsp    ; 48 89 E5
    //    mov eax, edi    ; 89 F8  (edi = 1° argomento 'a')
    //    add eax, esi    ; 01 F0  (esi = 2° argomento 'b')
    //    pop rbp         ; 5D
    //    ret             ; C3
    unsigned char code[] = {
        0x55,                   // push rbp
        0x48, 0x89, 0xE5,       // mov rbp, rsp
        0x89, 0xF8,             // mov eax, edi (a -> eax)
        0x01, 0xF0,             // add eax, esi (eax += b)
        0x5D,                   // pop rbp
        0xC3                    // ret
    };
    size_t code_size = sizeof(code);

    // 2. Ottieni la dimensione della pagina di memoria del sistema
    long page_size = sysconf(_SC_PAGESIZE);
    if (page_size == -1) {
        perror("Errore nel recupero della dimensione della pagina");
        return 1;
    }

    // Assicurati che la dimensione del codice sia un multiplo della dimensione della pagina
    // o alloca almeno una pagina intera.
    // Per semplicità, allochiamo una pagina intera.
    size_t alloc_size = page_size;
    if (code_size > alloc_size) {
        // Se il codice è più grande di una pagina, alloca pagine sufficienti
        alloc_size = ((code_size + page_size - 1) / page_size) * page_size;
    }

    // 3. Alloca memoria eseguibile usando mmap
    // PROT_READ | PROT_WRITE: Permessi di lettura e scrittura iniziali
    // MAP_PRIVATE | MAP_ANONYMOUS: Memoria privata, non mappata da un file
    unsigned char* executable_memory = (unsigned char*)mmap(
        NULL,
        alloc_size,
        PROT_READ | PROT_WRITE, // Inizialmente scrivibile per copiare il codice
        MAP_PRIVATE | MAP_ANONYMOUS,
        -1,
        0
    );

    if (executable_memory == MAP_FAILED) {
        perror("Errore nell'allocazione della memoria eseguibile con mmap");
        return 1;
    }

    printf("Memoria eseguibile allocata a: %p (dimensione: %zu byte)\n", (void*)executable_memory, alloc_size);

    // 4. Copia il codice macchina nell'area di memoria allocata
    memcpy(executable_memory, code, code_size);
    printf("Codice macchina copiato in memoria.\n");

    // 5. Cambia i permessi della memoria a sola esecuzione e lettura (PROT_EXEC | PROT_READ)
    //    Questo è un passo di sicurezza cruciale (W^X - Write XOR Execute)
    if (mprotect(executable_memory, alloc_size, PROT_READ | PROT_EXEC) == -1) {
        perror("Errore nel cambio dei permessi della memoria con mprotect");
        munmap(executable_memory, alloc_size); // Libera la memoria in caso di errore
        return 1;
    }
    printf("Permessi della memoria cambiati a lettura ed esecuzione.\n");

    // 6. Cast del puntatore alla memoria eseguibile a un puntatore a funzione
    my_function_type func_ptr = (my_function_type)executable_memory;

    // 7. Esegui la funzione dinamicamente
    int a = 10;
    int b = 20;
    int result = func_ptr(a, b);

    printf("Esecuzione della funzione dinamica: add(%d, %d) = %d\n", a, b, result);

    // 8. Libera la memoria allocata
    if (munmap(executable_memory, alloc_size) == -1) {
        perror("Errore nella liberazione della memoria con munmap");
        return 1;
    }
    printf("Memoria eseguibile liberata.\n");

    return 0;
}
