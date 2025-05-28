Nei binari WebAssembly (WASM), le **sezioni (sections)** sono blocchi di alto livello che organizzano i dati all'interno di un modulo, mentre le **funzioni (functions)** sono le effettive unità di codice eseguibile. Per sapere di quanto incrementare il puntatore di istruzione, è necessario decodificare l'istruzione corrente, poiché le istruzioni WASM hanno lunghezza variabile.

---

## Sezioni vs. Funzioni in WASM

Un file binario WebAssembly è strutturato in diverse **sezioni**. Ogni sezione ha uno scopo specifico e contiene un tipo particolare di informazione relativa al modulo. Alcune sezioni sono "note" (standardizzate), altre possono essere "personalizzate" (custom sections) per contenere metadati o informazioni specifiche per gli sviluppatori.

Le sezioni note principali includono:

* **Type (Tipo):** Definisce le firme delle funzioni (tipi dei parametri e dei risultati).
* **Import (Importazione):** Specifica le funzioni, memorie, tabelle o globali importati da altri moduli o dall'ambiente host.
* **Function (Funzione):** Associa un indice interno a ogni funzione definita nel modulo con la sua firma (un indice nella sezione Type).
* **Table (Tabella):** Definisce le tabelle, che sono array di riferimenti opachi (come riferimenti a funzioni), usate per l'indirizzamento indiretto delle chiamate di funzione.
* **Memory (Memoria):** Definisce la memoria lineare del modulo.
* **Global (Globale):** Definisce le variabili globali del modulo.
* **Export (Esportazione):** Rende disponibili funzioni, memorie, tabelle o globali all'ambiente host.
* **Start (Avvio):** Specifica una funzione che viene eseguita automaticamente quando il modulo viene istanziato.
* **Element (Elemento):** Inizializza le tabelle.
* **Code (Codice):** Contiene il corpo effettivo (bytecode) delle **funzioni** definite all'interno del modulo. Qui risiedono le istruzioni eseguibili.
* **Data (Dati):** Inizializza segmenti della memoria lineare.

Le **funzioni**, quindi, sono le unità logiche di codice che eseguono compiti specifici. Sono definite concettualmente nella sezione "Function" (che ne mappa l'indice alla firma) e il loro codice binario effettivo si trova nella sezione "Code". In sostanza, le sezioni sono il contenitore organizzativo, e le funzioni (nella sezione Code) sono uno dei tipi di contenuto.

---

## Lunghezza delle Istruzioni e Incremento del Puntatore

Le istruzioni WebAssembly **non hanno una lunghezza fissa**; sono a lunghezza variabile. Questo significa che non puoi semplicemente aggiungere un valore costante al puntatore di istruzione per passare all'istruzione successiva.

Per determinare la lunghezza di un'istruzione e sapere di quanto incrementare il puntatore, è necessario:

1.  **Leggere l'opcode:** Ogni istruzione inizia con un byte (o a volte più byte per istruzioni estese) che rappresenta il suo **opcode** (codice operativo).
2.  **Interpretare l'opcode:** L'opcode stesso determina la struttura dell'istruzione, inclusa la presenza e la dimensione di eventuali **operandi immediati** (ad esempio, costanti numeriche, indici di locali/globali, offset di memoria, etc.).
    * Molti operandi numerici in WASM, come gli interi, sono codificati usando il formato **LEB128 (Little Endian Base 128)**. La caratteristica di LEB128 è che è una codifica a lunghezza variabile per gli interi, dove i numeri più piccoli usano meno byte.
3.  **Calcolare la lunghezza totale:** La lunghezza totale dell'istruzione è data dalla somma della dimensione dell'opcode più la dimensione di tutti i suoi operandi immediati.

**Esempio:**
Un'istruzione come `i32.const <valore>` avrà un byte per l'opcode `i32.const`, seguito da `valore` codificato in LEB128. Se `valore` è piccolo (es. compreso tra 0 e 63 e non negativo, o tra -64 e -1), potrebbe essere codificato in un solo byte. Valori più grandi richiederanno più byte. Un'istruzione `nop` (no operation) è tipicamente un singolo byte. Un'istruzione `br_table` (branch table) avrà un opcode seguito da una lista di target e un target di default, rendendola significativamente più lunga.

Di conseguenza, un interprete, un compilatore JIT (Just-In-Time) o un disassemblatore WASM devono leggere e decodificare byte per byte l'istruzione corrente per capirne la natura e la lunghezza complessiva prima di poter avanzare correttamente alla successiva. Non esiste un incremento fisso per il puntatore di istruzione.