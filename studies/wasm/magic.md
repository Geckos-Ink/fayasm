# Struttura del File WebAssembly (WASM): Formato Binario e Confronto con Memory64

## 1. Introduzione al formato binario WASM

WebAssembly (WASM) è un formato binario progettato per l'esecuzione efficiente di codice in ambienti web e non. I file WASM sono rappresentazioni binarie di moduli WebAssembly, progettati per essere efficienti nella dimensione, nel tempo di caricamento e nella velocità di decodifica.

## 2. Formato binario di un file WASM

### 2.1 Struttura generale

Un file WebAssembly ha la seguente struttura di alto livello:

```
Magic Number + Version + Sezioni
```

### 2.2 Magic Number e Version

I primi 8 byte di ogni file WASM contengono un preambolo che identifica il file come un modulo WebAssembly:

```
0x00 0x61 0x73 0x6D  // Magic number: "\0asm"
0x01 0x00 0x00 0x00  // Version: 1 (little-endian)
```

- I primi 4 byte rappresentano la firma magica (`\0asm`) che identifica il file come WebAssembly
- I successivi 4 byte indicano la versione del formato (attualmente 1, rappresentato in little-endian)

### 2.3 Sezioni

Dopo il preambolo, un modulo WebAssembly è composto da una sequenza di sezioni. Ogni sezione ha il seguente formato:

```
ID della sezione (1 byte) + Dimensione della sezione (unsigned LEB128) + Contenuto (byte)
```

Le sezioni sono identificate da un ID numerico e possono apparire in qualsiasi ordine, tranne la sezione Custom che deve precedere le sezioni con lo stesso ID.

### 2.4 Tipi di sezioni

| ID | Tipo di Sezione | Descrizione |
|----|----------------|-------------|
| 0 | Custom | Sezione personalizzata che può contenere metadati |
| 1 | Type | Dichiarazioni di tipi di funzione |
| 2 | Import | Importazioni di funzioni, tabelle, memorie e globali |
| 3 | Function | Indici dei tipi per le funzioni |
| 4 | Table | Dichiarazioni di tabelle |
| 5 | Memory | Dichiarazioni di memoria |
| 6 | Global | Dichiarazioni di variabili globali |
| 7 | Export | Esportazioni di funzioni, tabelle, memorie e globali |
| 8 | Start | Funzione di avvio |
| 9 | Element | Inizializzatori per tabelle |
| 10 | Code | Corpi delle funzioni |
| 11 | Data | Inizializzatori per memorie |
| 12 | DataCount | Numero di segmenti di dati |

## 3. Dettagli delle sezioni principali

### 3.1 Sezione Type (ID: 1)

Definisce i tipi di funzione utilizzati nel modulo. Ogni tipo di funzione è rappresentato come:

```
0x60 + Numero di parametri + Tipi dei parametri + Numero di risultati + Tipi dei risultati
```

I tipi di valori sono codificati come:

- 0x7F: i32
- 0x7E: i64
- 0x7D: f32
- 0x7C: f64
- 0x7B: v128 (vettore)
- 0x70: funcref (riferimento a funzione)
- 0x6F: externref (riferimento esterno)

### 3.2 Sezione Function (ID: 3)

Associa le funzioni definite nel modulo ai loro tipi:

```
Numero di funzioni + Sequenza di indici di tipo
```

### 3.3 Sezione Memory (ID: 5)

Definisce spazi di memoria utilizzati dal modulo:

```
Numero di memorie + Limiti di memoria
```

Dove i limiti di memoria sono:
- Flag (0x00 per limite superiore non specificato, 0x01 per limite superiore specificato)
- Limite inferiore (pagine di 64KiB)
- [Opzionale] Limite superiore (pagine di 64KiB)

### 3.4 Sezione Code (ID: 10)

Contiene i corpi delle funzioni:

```
Numero di funzioni + [Per ogni funzione: dimensione in byte, numero di locali, tipi locali, espressione del corpo]
```

Il corpo della funzione è una sequenza di istruzioni terminata dall'istruzione "end" (0x0B).

## 4. Come deve leggere un file WASM un interprete

Un interprete WebAssembly dovrebbe seguire questi passaggi per elaborare un file WASM:

### 4.1 Convalida del preambolo
1. Verificare il magic number: `\0asm` (0x00 0x61 0x73 0x6D)
2. Verificare la versione: attualmente 1 (0x01 0x00 0x00 0x00)

### 4.2 Lettura delle sezioni
1. Leggere l'ID della sezione (1 byte)
2. Leggere la dimensione della sezione usando la codifica LEB128
3. Leggere il contenuto della sezione (byte)
4. Ripetere fino alla fine del file

### 4.3 Elaborazione delle sezioni
1. Processare le importazioni (sezione Import)
2. Allocare tabelle, memorie e globali (sezioni Table, Memory, Global)
3. Compilare o interpretare le funzioni (sezione Code)
4. Inizializzare le tabelle con i dati degli elementi (sezione Element)
5. Inizializzare le memorie con i dati (sezione Data)
6. Se presente, eseguire la funzione di avvio (sezione Start)

### 4.4 Accesso ai moduli
1. Rendere disponibili le esportazioni (sezione Export)
2. Gestire le chiamate alle funzioni esportate

## 5. Esempio di struttura di un file WASM semplice

Ecco un esempio di come potrebbe apparire il formato binario di un semplice modulo WebAssembly:

```
; Magic number e versione
00 61 73 6D        ; magic number: "\0asm"
01 00 00 00        ; version: 1

; Sezione Type (ID: 1)
01                 ; ID sezione
07                 ; lunghezza sezione
01                 ; numero di tipi
60                 ; tipo funzione
01                 ; 1 parametro
7F                 ; i32
01                 ; 1 risultato
7F                 ; i32

; Sezione Function (ID: 3)
03                 ; ID sezione
02                 ; lunghezza sezione
01                 ; numero di funzioni
00                 ; indice di tipo 0

; Sezione Export (ID: 7)
07                 ; ID sezione
0A                 ; lunghezza sezione
01                 ; numero di esportazioni
04 61 64 64 32     ; "add2" (nome)
00                 ; funzione
00                 ; indice di funzione

; Sezione Code (ID: 10)
0A                 ; ID sezione
0D                 ; lunghezza sezione
01                 ; numero di funzioni
0B                 ; dimensione del corpo della funzione
01                 ; numero di gruppi di variabili locali
01                 ; numero di variabili locali
7F                 ; tipo i32
20 00              ; get_local 0
41 02              ; i32.const 2
6A                 ; i32.add
0B                 ; end
```

Questo esempio definisce un semplice modulo con una funzione chiamata "add2" che aggiunge 2 al suo parametro.

## 6. WebAssembly Memory64 vs Standard WASM

### 6.1 WebAssembly Standard (Memory32)

Nella versione standard di WebAssembly, la memoria è indirizzata mediante indici a 32 bit, il che significa:
- Lo spazio di indirizzi massimo è 4 GiB (2^32 byte)
- Le istruzioni di caricamento e memorizzazione utilizzano indici a 32 bit
- I limiti di memoria sono misurati in pagine di 64 KiB (65,536 byte)
- Il limite massimo è di 65,536 pagine (4 GiB)

### 6.2 WebAssembly Memory64

WebAssembly Memory64 è un'estensione che consente spazi di memoria più grandi tramite indirizzamento a 64 bit:

#### 6.2.1 Differenze principali

1. **Sezione Memory**
   Nel formato binario, la definizione di memoria include un flag aggiuntivo che indica il tipo di memoria:
   ```
   0x00: memoria a 32 bit (standard)
   0x01: memoria a 64 bit (Memory64)
   ```

2. **Nuova definizione dei limiti di memoria**
   I limiti di memoria in Memory64 sono codificati come interi a 64 bit (invece di 32 bit), utilizzando sempre la codifica LEB128:
   ```
   Flag tipo (1 = limite superiore specificato) | Flag indirizzamento (1 = Memory64) + Limite inferiore (i64) + [Opzionale] Limite superiore (i64)
   ```

3. **Nuove istruzioni di caricamento e memorizzazione**
   Memory64 introduce nuove istruzioni per operazioni di memoria con indici a 64 bit:
   - `i32.load`: diventa `i32.load (i32.i32)`
   - `i32.load`: diventa `i32.load (i64.i32)` per Memory64
   - Similmente per tutte le altre operazioni di caricamento e memorizzazione

4. **Codifica in file binario**
   Per indicare Memory64, nella sezione Memory:
   ```
   Standard: 0x00 (limite non specificato) o 0x01 (limite specificato)
   Memory64: 0x02 (limite non specificato) o 0x03 (limite specificato)
   ```

5. **Indirizzamento**
   - WASM standard: gli offset di memoria sono di tipo `i32` (massimo 4 GiB)
   - Memory64: gli offset di memoria sono di tipo `i64` (massimo 2^64 byte)

#### 6.2.2 Vantaggi di Memory64

1. **Supporto per dataset molto grandi**: Consente di elaborare dataset che superano i 4 GiB
2. **Compatibilità con sistemi a 64 bit**: Meglio allineato con le architetture di calcolo moderne
3. **Semplificazione per compilatori**: Nessuna necessità di strategie di chunking per grandi array
4. **Supporto nativo per file di grandi dimensioni**: Elaborazione diretta di file di dimensioni superiori a 4 GiB

#### 6.2.3 Modifiche richieste all'interprete

Un interprete WebAssembly che supporta Memory64 deve:

1. Riconoscere il flag di tipo di memoria nella sezione Memory
2. Allocare strutture di gestione della memoria che supportino indirizzamento a 64 bit
3. Implementare le istruzioni di caricamento e memorizzazione con indici a 64 bit
4. Gestire correttamente le semantiche di memoria per operazioni a 64 bit
5. Verificare i limiti di memoria utilizzando il tipo appropriato (i32 o i64)

## 7. Convalida e Sicurezza

### 7.1 Convalida di un modulo WASM

Prima dell'esecuzione, un interprete WASM deve validare il modulo per garantire che sia sicuro e ben formato:

1. Verificare la sintassi binaria corretta di tutte le sezioni
2. Verificare che tutte le funzioni, tabelle, memorie e globali siano ben tipizzate
3. Verificare che tutte le istruzioni siano valide e utilizzino operandi di tipo corretto
4. Verificare che i flussi di controllo siano ben formati (ad esempio, ogni `if` ha un corrispondente `end`)
5. Verificare che tutti i riferimenti ad elementi importati siano validi

### 7.2 Considerazioni per Memory64

Quando si convalida un modulo Memory64, ci sono considerazioni aggiuntive:

1. Verificare che tutte le istruzioni di memoria utilizzino indirizzi a 64 bit
2. Garantire che le istruzioni di controllo di flusso gestiscano correttamente gli offset a 64 bit
3. Verificare la semantica corretta per operazioni di caricamento e memorizzazione a 64 bit
4. Garantire che qualsiasi operazione di conversione tra i32 e i64 sia valida per l'indirizzamento della memoria

## 8. Considerazioni sull'esecuzione

Un interprete deve considerare quanto segue durante l'esecuzione di un modulo WebAssembly:

1. **Gestione della memoria**: Allocazione efficiente, deallocazione e accesso alla memoria
2. **Stack di valori**: Mantenimento di uno stack di valori per operandi ed espressioni
3. **Stack di chiamate**: Gestione delle chiamate di funzione e del flusso di controllo
4. **Importazioni/Esportazioni**: Gestione dell'interfaccia con l'ambiente host
5. **Gestione degli errori**: Rilevamento e gestione di condizioni di errore (overflow, divisione per zero, ecc.)

### 8.1 Considerazioni specifiche per Memory64

1. **Allocazione di memoria**: Gestire correttamente l'allocazione di aree di memoria di grandi dimensioni
2. **Portabilità**: Garantire che l'esecuzione sia coerente su piattaforme a 32 bit e 64 bit
3. **Ottimizzazione**: Implementare tecniche di ottimizzazione specifiche per l'indirizzamento a 64 bit
4. **Interoperabilità**: Gestire correttamente l'interazione tra moduli Memory32 e Memory64

## 9. Conclusione

Il formato binario WebAssembly è progettato per essere compatto, facile da decodificare e veloce da eseguire. La variante Memory64 estende le capacità standard consentendo spazi di memoria molto più grandi attraverso l'indirizzamento a 64 bit, il che è particolarmente utile per applicazioni che richiedono l'elaborazione di grandi quantità di dati.

Un interprete WebAssembly deve essere in grado di leggere e convalidare correttamente il formato binario, eseguire le istruzioni in modo sicuro ed efficiente, e gestire le importazioni e le esportazioni per interagire con l'ambiente host. Con Memory64, l'interprete deve inoltre implementare correttamente l'indirizzamento a 64 bit e garantire che tutte le operazioni di memoria siano gestite in modo appropriato.