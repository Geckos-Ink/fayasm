# Struttura delle Funzioni nel Binario WebAssembly

## 1. Panoramica della dichiarazione delle funzioni in WASM

Nel formato binario WebAssembly, le funzioni sono definite e rappresentate attraverso diverse sezioni separate. Questa separazione permette una decodifica efficiente e un caricamento veloce del modulo. Le funzioni in WASM sono costituite da diversi componenti distribuiti in sezioni distinte del file binario.

## 2. Sezioni principali per la dichiarazione delle funzioni

### 2.1 Sezione Type (ID: 1)

La sezione Type definisce i tipi di funzione utilizzati nel modulo. Ogni tipo di funzione specifica la sua firma: i tipi dei parametri e i tipi dei valori di ritorno.

**Formato binario:**
```
01                     # ID della sezione Type
<size: u32>            # Dimensione della sezione in byte (LEB128)
<count: u32>           # Numero di tipi di funzione (LEB128)
<function_types>       # Sequenza di tipi di funzione
```

**Dove ogni tipo di funzione è codificato come:**
```
0x60                   # Identificatore di tipo funzione
<param_count: u32>     # Numero di parametri (LEB128)
<param_types>          # Tipi dei parametri (sequenza di byte)
<return_count: u32>    # Numero di valori di ritorno (LEB128)
<return_types>         # Tipi dei valori di ritorno (sequenza di byte)
```

I tipi di valori sono codificati come singoli byte:
- `0x7F` per i32
- `0x7E` per i64
- `0x7D` per f32
- `0x7C` per f64
- `0x7B` per v128 (vettore)
- `0x70` per funcref
- `0x6F` per externref

### 2.2 Sezione Function (ID: 3)

La sezione Function associa le funzioni definite nel modulo ai loro tipi (definiti nella sezione Type). In pratica, questa sezione contiene solo indici che fanno riferimento ai tipi nella sezione Type.

**Formato binario:**
```
03                     # ID della sezione Function
<size: u32>            # Dimensione della sezione in byte (LEB128)
<count: u32>           # Numero di funzioni definite (LEB128)
<type_indices>         # Indici dei tipi per ciascuna funzione (LEB128)
```

Ogni funzione è rappresentata dal suo indice di tipo, che punta alla definizione del tipo nella sezione Type.

### 2.3 Sezione Code (ID: 10)

La sezione Code contiene le implementazioni (i corpi) delle funzioni. Ogni funzione ha le sue variabili locali e il codice delle istruzioni.

**Formato binario:**
```
0A                     # ID della sezione Code
<size: u32>            # Dimensione della sezione in byte (LEB128)
<count: u32>           # Numero di corpi di funzione (LEB128)
<function_bodies>      # Corpi delle funzioni
```

**Dove ogni corpo di funzione è codificato come:**
```
<body_size: u32>       # Dimensione del corpo in byte (LEB128)
<local_count: u32>     # Numero di dichiarazioni locali (LEB128)
<locals>               # Dichiarazioni delle variabili locali
<code>                 # Istruzioni del corpo della funzione
```

**Dove ogni dichiarazione locale è:**
```
<count: u32>           # Numero di variabili locali di questo tipo (LEB128)
<type: byte>           # Tipo delle variabili (i32, i64, f32, f64, etc.)
```

**Il codice della funzione è:**
```
<instructions>         # Sequenza di istruzioni codificate come bytecode
0x0B                   # Opcode 'end' che termina il corpo della funzione
```

### 2.4 Sezione Export (ID: 7)

La sezione Export definisce quali funzioni (e altri oggetti) sono visibili all'esterno del modulo. Le funzioni esportate possono essere chiamate dall'ambiente host o da altri moduli.

**Formato binario:**
```
07                     # ID della sezione Export
<size: u32>            # Dimensione della sezione in byte (LEB128)
<count: u32>           # Numero di esportazioni (LEB128)
<exports>              # Elementi esportati
```

**Dove ogni esportazione è codificata come:**
```
<name_len: u32>        # Lunghezza del nome (LEB128)
<name_bytes>           # UTF-8 nome dell'esportazione
<kind: byte>           # Tipo dell'oggetto esportato (0x00 per funzione)
<index: u32>           # Indice dell'oggetto esportato (LEB128)
```

### 2.5 Sezione Import (ID: 2)

La sezione Import definisce le funzioni (e altri oggetti) che il modulo importa dall'ambiente host o da altri moduli.

**Formato binario:**
```
02                     # ID della sezione Import
<size: u32>            # Dimensione della sezione in byte (LEB128)
<count: u32>           # Numero di importazioni (LEB128)
<imports>              # Elementi importati
```

**Dove ogni importazione è codificata come:**
```
<module_len: u32>      # Lunghezza del nome del modulo (LEB128)
<module_bytes>         # UTF-8 nome del modulo
<name_len: u32>        # Lunghezza del nome dell'elemento (LEB128)
<name_bytes>           # UTF-8 nome dell'elemento
<kind: byte>           # Tipo dell'oggetto importato (0x00 per funzione)
<type: u32>            # Per le funzioni: indice del tipo (LEB128)
```

## 3. Relazioni tra le sezioni di funzioni

La struttura modulare delle funzioni in WASM permette la separazione della dichiarazione dall'implementazione:

1. **Definizione dei tipi** (Type): Definisce le firme delle funzioni
2. **Importazioni** (Import): Dichiara funzioni importate con i loro tipi
3. **Indici di tipo** (Function): Associa ogni funzione definita internamente a un tipo
4. **Implementazioni** (Code): Contiene il codice delle funzioni definite internamente
5. **Esportazioni** (Export): Rende le funzioni accessibili all'esterno

**Importante:** Il numero di funzioni nella sezione Function deve corrispondere esattamente al numero di corpi di funzione nella sezione Code. L'ordine delle funzioni nella sezione Function definisce gli indici delle funzioni, che sono utilizzati in altri punti del modulo.

## 4. Indirizzamento e indici delle funzioni

Nel binario WASM, le funzioni sono identificate da indici numerici. L'indirizzamento segue queste regole:

1. Le funzioni importate ricevono i primi indici (a partire da 0)
2. Le funzioni definite localmente ricevono indici successivi alle importazioni

Esempio: Se un modulo importa 3 funzioni e ne definisce 5 localmente:
- Le funzioni importate avranno indici 0, 1, 2
- Le funzioni definite localmente avranno indici 3, 4, 5, 6, 7

## 5. Struttura dettagliata del corpo di una funzione

Il corpo di una funzione nella sezione Code è composto da:

### 5.1 Dichiarazioni delle variabili locali

Le variabili locali sono dichiarate all'inizio del corpo della funzione. Le variabili locali includono i parametri della funzione (che sono pre-popolati al momento della chiamata) seguiti da variabili locali aggiuntive.

**Formato binario per le dichiarazioni locali:**
```
<count: u32>           # Numero di gruppi di dichiarazioni locali (LEB128)
<local_group_1>        # (count, type) per il primo gruppo
<local_group_2>        # (count, type) per il secondo gruppo
...
```

Dove ogni gruppo specifica un numero di variabili locali dello stesso tipo:
```
<num_locals: u32>      # Numero di variabili locali in questo gruppo (LEB128)
<type: byte>           # Tipo delle variabili (i32, i64, f32, f64)
```

I gruppi di variabili locali vengono concatenati per formare l'insieme completo delle variabili locali. I parametri della funzione sono considerati come le prime variabili locali (indici 0, 1, 2, ecc.).

### 5.2 Codice delle istruzioni

Dopo le dichiarazioni locali segue la sequenza di istruzioni che costituisce il corpo della funzione. Le istruzioni sono codificate come una sequenza di opcode (1 byte) seguiti da eventuali operandi.

**Esempio di sequenza di istruzioni:**
```
20 00       # get_local 0     (carica parametro 0)
20 01       # get_local 1     (carica parametro 1)
6A          # i32.add         (somma i due valori)
21 02       # set_local 2     (memorizza il risultato nella variabile locale 2)
20 02       # get_local 2     (carica il risultato)
0B          # end             (termina la funzione)
```

Il corpo della funzione termina sempre con l'istruzione `end` (opcode `0x0B`).

## 6. Esempio completo di dichiarazione di funzione

Ecco un esempio completo di come verrebbe codificata una funzione `add(a: i32, b: i32) -> i32` nel binario WASM:

```
# Sezione Type
01                     # ID della sezione Type
07                     # Dimensione della sezione (7 byte)
01                     # Un solo tipo
60                     # Tipo funzione
02                     # Due parametri
7F 7F                  # Tipo i32, i32
01                     # Un valore di ritorno
7F                     # Tipo i32

# Sezione Function
03                     # ID della sezione Function
02                     # Dimensione della sezione (2 byte)
01                     # Una funzione
00                     # Indice di tipo 0 (riferimento alla funzione definita sopra)

# Sezione Export
07                     # ID della sezione Export
07                     # Dimensione della sezione (7 byte)
01                     # Una esportazione
03                     # Lunghezza del nome (3 caratteri)
61 64 64               # "add" in UTF-8
00                     # Tipo: funzione
00                     # Indice della funzione 0

# Sezione Code
0A                     # ID della sezione Code
0A                     # Dimensione della sezione (10 byte)
01                     # Un corpo di funzione
08                     # Dimensione del corpo (8 byte)
00                     # Nessuna variabile locale aggiuntiva
20 00                  # get_local 0
20 01                  # get_local 1
6A                     # i32.add
0B                     # end
```

## 7. Partizionamento e indirizzamento interno delle funzioni

### 7.1 Tabelle di funzione

Per supportare chiamate indirette (tramite puntatori a funzione), WebAssembly utilizza la sezione Table. Le tabelle sono array di riferimenti a funzioni che possono essere acceduti dinamicamente.

**Formato binario della sezione Table:**
```
04                     # ID della sezione Table
<size: u32>            # Dimensione della sezione in byte (LEB128)
<count: u32>           # Numero di tabelle (generalmente 1)
<type: byte>           # Tipo della tabella (0x70 per funcref)
<limits>               # Limiti della tabella (min, max)
```

### 7.2 Sezione Element

La sezione Element inizializza le tabelle di funzioni con riferimenti a funzioni specifiche.

**Formato binario della sezione Element:**
```
09                     # ID della sezione Element
<size: u32>            # Dimensione della sezione in byte (LEB128)
<count: u32>           # Numero di segmenti
<segments>             # Segmenti di inizializzazione
```

**Dove ogni segmento è codificato come:**
```
<table_index: u32>     # Indice della tabella (LEB128, generalmente 0)
<offset_expr>          # Espressione per l'offset iniziale
<num_elems: u32>       # Numero di elementi (LEB128)
<elem_indices>         # Indici delle funzioni (LEB128)
```

### 7.3 Chiamate tra funzioni

Nel bytecode WASM, le chiamate dirette tra funzioni utilizzano l'istruzione `call` seguita dall'indice della funzione chiamata:

```
10 03       # call 3    (chiama la funzione con indice 3)
```

Le chiamate indirette utilizzano `call_indirect` con un indice di tipo e un indice nella tabella:

```
11 02 00    # call_indirect type:2 table:0
```

## 8. Considerazioni sull'ottimizzazione

La struttura binaria WASM è progettata per permettere:

1. **Validazione veloce**: La separazione tra tipi, indici di funzione e corpi permette la validazione dei tipi senza decodificare i corpi delle funzioni
2. **Caricamento lazy**: I corpi delle funzioni possono essere caricati solo quando necessario
3. **Compilazione parallela**: Le funzioni possono essere compilate contemporaneamente dato che sono indipendenti
4. **Linking efficiente**: Le esportazioni e le importazioni facilitano il linking tra moduli

## 9. Partizionamento fisico delle funzioni nel file binario

Dal punto di vista dell'organizzazione fisica del file binario, le funzioni in WASM sono partizionate in questo modo:

1. **Metadati delle funzioni**: Distribuiti tra le sezioni Type, Function, Import ed Export
2. **Corpi delle funzioni**: Raggruppati insieme nella sezione Code
3. **Tabelle di indirizzamento**: Nelle sezioni Table ed Element

Questa separazione permette:
- Generazione di codice efficiente
- Caricamento incrementale
- Ottimizzazioni di compilazione
- Streaming e decodifica parallela

## 10. Esempio di partizionamento con più funzioni

Ecco come apparirebbe nel binario un modulo con tre funzioni: due definite localmente e una importata:

```
# Sezione Type
01                     # ID sezione Type
0D                     # Dimensione (13 byte)
03                     # 3 tipi di funzione
60 00 01 7F            # Tipo 0: () -> i32
60 01 7F 01 7F         # Tipo 1: (i32) -> i32
60 02 7F 7F 01 7F      # Tipo 2: (i32, i32) -> i32

# Sezione Import
02                     # ID sezione Import
0E                     # Dimensione (14 byte)
01                     # 1 importazione
03                     # Lunghezza nome modulo (3 byte)
65 6E 76               # "env" (UTF-8)
07                     # Lunghezza nome funzione (7 byte)
67 65 74 54 69 6D 65   # "getTime" (UTF-8)
00                     # Tipo: funzione
00                     # Indice del tipo 0: () -> i32

# Sezione Function
03                     # ID sezione Function
03                     # Dimensione (3 byte)
02                     # 2 funzioni
01 02                  # Indici dei tipi: 1, 2

# Sezione Export
07                     # ID sezione Export
15                     # Dimensione (21 byte)
02                     # 2 esportazioni
08                     # Lunghezza del nome (8 byte)
64 6F 75 62 6C 65 49 74 # "doubleIt" (UTF-8)
00                     # Tipo: funzione
01                     # Indice funzione 1 (la prima funzione dopo l'importata)
03                     # Lunghezza del nome (3 byte)
61 64 64               # "add" (UTF-8)
00                     # Tipo: funzione
02                     # Indice funzione 2 (la seconda funzione dopo l'importata)

# Sezione Code
0A                     # ID sezione Code
18                     # Dimensione (24 byte)
02                     # 2 corpi di funzione

# Corpo della prima funzione locale (doubleIt)
0A                     # Dimensione del corpo (10 byte)
00                     # Nessuna variabile locale aggiuntiva
20 00                  # get_local 0
20 00                  # get_local 0
6A                     # i32.add
0B                     # end

# Corpo della seconda funzione locale (add)
0A                     # Dimensione del corpo (10 byte)
00                     # Nessuna variabile locale aggiuntiva
20 00                  # get_local 0
20 01                  # get_local 1
6A                     # i32.add
0B                     # end
```

In questo esempio, il modulo ha tre funzioni con indici:
- Indice 0: funzione importata "getTime" (tipo 0)
- Indice 1: funzione locale "doubleIt" (tipo 1)
- Indice 2: funzione locale "add" (tipo 2)

La sezione Code contiene i corpi solo delle due funzioni definite localmente, mentre la funzione importata è definita solo nella sezione Import.