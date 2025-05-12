Okay, creiamo un esempio di JIT (Just-In-Time compiler) molto semplice in C.

Questo esempio si concentrerà sul concetto fondamentale: generare codice macchina nativo in memoria ed eseguirlo. Per semplicità, useremo l'architettura **x86-64** (comune su PC desktop/laptop moderni Linux e Windows) e definiremo un set minimale di operazioni.

**Concetto Chiave:**

1.  **Definisci le Operazioni:** Stabiliamo un piccolo "linguaggio" intermedio o set di istruzioni che vogliamo compilare (es. carica costante, somma, ritorna).
2.  **Traduci in Codice Macchina:** Per ogni operazione, conosciamo la sequenza di byte (opcode) corrispondente sull'architettura target (x86-64 nel nostro caso).
3.  **Alloca Memoria Eseguibile:** Usiamo funzioni di sistema (`mmap` su POSIX, `VirtualAlloc` su Windows) per ottenere un blocco di memoria che può essere scritto e poi marcato come eseguibile. Questa è la parte critica e dipendente dal sistema operativo.
4.  **Scrivi il Codice:** Copiamo le sequenze di byte del codice macchina nel buffer di memoria allocato.
5.  **Esegui:** Facciamo un cast del puntatore al buffer di memoria a un puntatore a funzione e lo chiamiamo direttamente.

**Limitazioni dell'Esempio:**

* **Specifico per x86-64:** Il codice macchina generato funzionerà *solo* su CPU x86-64. Un JIT reale per diverse architetture è molto più complesso (spesso usa librerie come LLVM).
* **Operazioni Limitate:** Solo poche operazioni base per dimostrare il concetto.
* **Gestione Semplice dei Registri:** Useremo principalmente il registro `rax` per i calcoli e il ritorno del valore (convenzione x86-64).
* **Nessuna Ottimizzazione:** Non ci sono passaggi di ottimizzazione reali.
* **Sicurezza:** Allocare memoria scrivibile ed eseguibile può essere un rischio per la sicurezza (W^X violation). Le implementazioni reali spesso scrivono il codice e *poi* cambiano i permessi a solo lettura ed esecuzione (`mprotect`, `VirtualProtect`). Lo faremo anche noi.

---

**Codice Esempio (C)**

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h> // Per memcpy
#include <stdint.h> // Per tipi come uint8_t

// === Gestione Memoria Eseguibile (Dipendente dal Sistema Operativo) ===
#ifdef _WIN32
    #include <windows.h>
    // Funzione per allocare memoria eseguibile su Windows
    void* alloc_executable_memory(size_t size) {
        // Alloca memoria con permessi READWRITE inizialmente
        void* mem = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!mem) {
            perror("VirtualAlloc failed");
            return NULL;
        }
        return mem;
    }

    // Funzione per rendere la memoria eseguibile (e opzionalmente read-only)
    int make_memory_executable(void* mem, size_t size) {
        DWORD oldProtect;
        // Cambia i permessi a EXECUTE_READ
        if (!VirtualProtect(mem, size, PAGE_EXECUTE_READ, &oldProtect)) {
            perror("VirtualProtect failed");
            return 0; // Fallimento
        }
        return 1; // Successo
    }

    // Funzione per liberare la memoria
    void free_executable_memory(void* mem, size_t size) {
        // Bisogna passare size=0 e MEM_RELEASE per VirtualFree quando si rilascia
        // La size originale non è necessaria qui.
        VirtualFree(mem, 0, MEM_RELEASE);
    }
#else // Assumiamo POSIX (Linux, macOS, etc.)
    #include <sys/mman.h>
    #include <unistd.h> // Per sysconf

    // Funzione per allocare memoria eseguibile su POSIX
    void* alloc_executable_memory(size_t size) {
        // Alloca memoria anonima con permessi READWRITE
        void* mem = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) {
            perror("mmap failed");
            return NULL;
        }
        return mem;
    }

    // Funzione per rendere la memoria eseguibile (e opzionalmente read-only)
    int make_memory_executable(void* mem, size_t size) {
        // Cambia i permessi a READ | EXEC
        if (mprotect(mem, size, PROT_READ | PROT_EXEC) == -1) {
            perror("mprotect failed");
            return 0; // Fallimento
        }
        return 1; // Successo
    }

    // Funzione per liberare la memoria
    void free_executable_memory(void* mem, size_t size) {
        munmap(mem, size);
    }
#endif
// =====================================================================

// Definiamo le nostre "pseudo-istruzioni"
typedef enum {
    OP_LOAD_CONST, // Carica una costante nel registro 'accumulatore' (rax)
    OP_ADD_CONST,  // Aggiunge una costante all'accumulatore (rax)
    OP_RETURN      // Ritorna il valore nell'accumulatore (rax)
} OpType;

// Struttura per rappresentare un'istruzione del nostro linguaggio
typedef struct {
    OpType type;
    int32_t value; // Valore costante (usiamo 32 bit per semplicità)
} Instruction;

// Tipo del puntatore alla funzione generata
// Prenderà nessun argomento e ritornerà un int (convenzione: valore in rax)
typedef int (*GeneratedFunction)();

// Funzione JIT: compila le istruzioni in codice macchina
GeneratedFunction jit_compile(Instruction* instructions, size_t count) {
    // Stima (molto generosa) della dimensione del buffer necessario
    // Ogni istruzione x86-64 richiederà pochi byte (es. 1-10 bytes)
    size_t buffer_size = count * 15 + 1; // 15 byte per istruzione + 1 per ret finale
    uint8_t* code_buffer = (uint8_t*)alloc_executable_memory(buffer_size);
    if (!code_buffer) {
        return NULL;
    }

    uint8_t* current_pos = code_buffer; // Puntatore alla posizione corrente nel buffer

    printf("Compilazione JIT...\n");

    for (size_t i = 0; i < count; ++i) {
        Instruction instr = instructions[i];
        printf("  Compilando: ");

        switch (instr.type) {
            case OP_LOAD_CONST:
                // mov rax, immediate64 (Codice: 48 b8 [8 byte immediate])
                // Usiamo un valore 32bit sign-extended a 64bit
                printf("LOAD_CONST %d\n", instr.value);
                *(current_pos++) = 0x48; // REX.W prefix (operand size 64 bit)
                *(current_pos++) = 0xb8; // mov rax, imm64 opcode
                // Scrive il valore 64bit (little-endian)
                // Mettiamo il valore 32bit nei 4 byte bassi, e 0 nei 4 alti
                // (o si potrebbe sign-extend, ma per questo esempio va bene così)
                *(int32_t*)current_pos = instr.value;
                current_pos += 4;
                 *(int32_t*)current_pos = (instr.value < 0) ? -1 : 0; // Sign extension
                current_pos += 4;
                 //memcpy(current_pos, &instr.value, sizeof(int64_t)); // Più semplice se value fosse int64_t
                 //current_pos += sizeof(int64_t);
                break;

            case OP_ADD_CONST:
                // add rax, immediate32 (Codice: 48 81 c0 [4 byte immediate])
                // Questa istruzione somma un valore 32bit (sign-extended) a rax
                 printf("ADD_CONST %d\n", instr.value);
                 *(current_pos++) = 0x48; // REX.W prefix
                 *(current_pos++) = 0x81; // Opcode per add r/m64, imm32
                 *(current_pos++) = 0xc0; // ModR/M byte: specifica 'add rax, immediate'
                // Scrive il valore 32bit (little-endian)
                *(int32_t*)current_pos = instr.value;
                current_pos += sizeof(int32_t);
                 break;

            case OP_RETURN:
                // ret (Codice: c3)
                printf("RETURN\n");
                *(current_pos++) = 0xc3;
                break;

            default:
                fprintf(stderr, "Errore: Tipo di istruzione non riconosciuto!\n");
                free_executable_memory(code_buffer, buffer_size);
                return NULL;
        }
    }

    // Calcola la dimensione effettiva del codice generato
    size_t code_size = current_pos - code_buffer;
    printf("Dimensione codice generato: %zu bytes\n", code_size);

    // Rendi la memoria eseguibile (e preferibilmente non più scrivibile)
    if (!make_memory_executable(code_buffer, code_size)) {
        // Se fallisce, dobbiamo liberare la memoria che avevamo allocato
        free_executable_memory(code_buffer, buffer_size); // Passiamo la dimensione allocata originale
        return NULL;
    }

    printf("Compilazione JIT completata.\n");

    // Fai il cast del buffer a un puntatore a funzione e restituiscilo
    // NOTA: Questo cast è tecnicamente "undefined behavior" in C standard,
    // ma funziona sulla maggior parte delle piattaforme comuni.
    return (GeneratedFunction)code_buffer;
}

int main() {
    // Definisci una sequenza di operazioni da eseguire
    // Calcolerà: (5 + 10) + 7 = 22
    Instruction program[] = {
        { OP_LOAD_CONST, 5  }, // rax = 5
        { OP_ADD_CONST,  10 }, // rax = rax + 10 (rax = 15)
        { OP_ADD_CONST,  7  }, // rax = rax + 7  (rax = 22)
        { OP_RETURN,     0  }  // return rax
    };
    size_t program_size = sizeof(program) / sizeof(program[0]);

    // Compila il programma in codice macchina
    GeneratedFunction func = jit_compile(program, program_size);

    if (func) {
        printf("Esecuzione del codice JIT...\n");
        int result = func(); // Chiama la funzione generata!
        printf("Risultato: %d\n", result);

        // Libera la memoria quando hai finito
        // Dobbiamo sapere la dimensione originale allocata per munmap/VirtualFree
        // (Recuperarla precisamente richiederebbe di salvarla, usiamo la stima)
        size_t allocated_size = program_size * 15 + 1;
        free_executable_memory((void*)func, allocated_size);
        printf("Memoria JIT liberata.\n");
    } else {
        fprintf(stderr, "Fallimento della compilazione JIT.\n");
        return 1;
    }

    return 0;
}
```

**Come Compilare ed Eseguire:**

1.  **Salva:** Salva il codice come `simple_jit.c`.
2.  **Compila:**
    * **Linux/macOS (con GCC o Clang):**
        ```bash
        gcc simple_jit.c -o simple_jit
        # Oppure
        # clang simple_jit.c -o simple_jit
        ```
        Potrebbe essere necessario linkare `-ldl` in alcuni sistemi, anche se `mmap`/`mprotect` sono solitamente in libc.
    * **Windows (con MinGW/GCC):**
        ```bash
        gcc simple_jit.c -o simple_jit.exe
        ```
    * **Windows (con MSVC - Visual Studio Compiler):**
        ```bash
        cl simple_jit.c
        ```
        Potrebbe dare warning sul cast da puntatore a dati a puntatore a funzione, che è normale per questo tipo di codice.

3.  **Esegui:**
    * **Linux/macOS:** `./simple_jit`
    * **Windows:** `.\simple_jit.exe`

**Output Atteso:**

```
Compilazione JIT...
  Compilando: LOAD_CONST 5
  Compilando: ADD_CONST 10
  Compilando: ADD_CONST 7
  Compilando: RETURN
Dimensione codice generato: 32 bytes  // La dimensione esatta può variare leggermente
Compilazione JIT completata.
Esecuzione del codice JIT...
Risultato: 22
Memoria JIT liberata.
```

**Spiegazione delle Performance:**

Il vantaggio di questo approccio rispetto a un interprete che usa `if` o `switch` è enorme:

* **Interprete:** Per ogni operazione nel `program`, l'interprete deve:
    1.  Leggere il tipo di operazione.
    2.  Eseguire uno `switch` o una serie di `if/else` per determinare quale codice C eseguire.
    3.  Eseguire quel codice C (che a sua volta viene compilato in codice macchina).
    4.  Passare all'operazione successiva (overhead del loop).
    Questo comporta salti condizionali e indirezioni continue.

* **JIT (questo esempio):**
    1.  **Compilazione (una volta):** Traduce l'intera sequenza `program` in un blocco contiguo di istruzioni macchina native (es. `mov`, `add`, `ret`).
    2.  **Esecuzione (molte volte, potenzialmente):** Esegue *direttamente* quel blocco di codice macchina. La CPU fetcha ed esegue le istruzioni native una dopo l'altra, senza overhead di interpretazione, switch, o loop di alto livello per ogni singola operazione.

Questo è il motivo per cui i JIT sono fondamentali per le performance di linguaggi come Java, C#, JavaScript (nei browser moderni): trasformano il bytecode o il codice sorgente in codice macchina veloce durante l'esecuzione. Questo esempio cattura l'essenza di quel processo, sebbene in modo molto semplificato.