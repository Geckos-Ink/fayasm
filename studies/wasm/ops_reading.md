# Guida all'interpretazione del Bytecode WebAssembly (WASM)

## Introduzione

WebAssembly (WASM) è un formato binario progettato come target di compilazione per linguaggi di alto livello. Il suo bytecode è stato progettato per essere compatto, efficiente e facilmente decodificabile. Questo documento spiega come interpretare il bytecode WASM, concentrandosi sulla struttura delle istruzioni e su come vengono codificate.

## Struttura generale del bytecode WASM

Un modulo WebAssembly è composto da diverse sezioni, e il bytecode delle funzioni si trova nella sezione "Code". Prima di esaminare le istruzioni specifiche, è importante capire che il bytecode WASM utilizza una codifica LEB128 (Little Endian Base 128) per i numeri interi, che permette di rappresentare numeri di dimensioni variabili in modo efficiente.

## Codifica delle istruzioni

Ogni istruzione in WASM è rappresentata da un opcode (un singolo byte) seguito da zero o più operandi. Gli operandi possono essere immediati (valori codificati direttamente nel bytecode) o indici che fanno riferimento ad altre strutture nel modulo WASM.

### Struttura di base di un'istruzione

```
[opcode] [operandi (opzionali)]
```

## Istruzioni principali e loro codifica

### Istruzioni di controllo del flusso

#### if (0x04)
```
0x04 [tipo di risultato] [corpo del then] 0x05 [corpo dell'else] 0x0B
```
- `0x04` è l'opcode per `if`
- `tipo di risultato` specifica il tipo restituito (0x40 per void)
- `0x05` è l'opcode per `else` (opzionale)
- `0x0B` è l'opcode per `end`

Esempio:
```
04 40 ... 05 ... 0B  // if-else-end
04 40 ... 0B         // if-end (senza else)
```

#### block (0x02) e loop (0x03)
```
0x02/0x03 [tipo di risultato] [corpo] 0x0B
```

Esempio:
```
02 40 ... 0B  // block
03 40 ... 0B  // loop
```

#### br (0x0C), br_if (0x0D)
```
0x0C [indice del blocco di destinazione]     // br
0x0D [indice del blocco di destinazione]     // br_if
```

L'indice è codificato come un intero unsigned LEB128 e rappresenta la profondità del blocco di destinazione.

### Istruzioni di caricamento e memorizzazione

#### Istruzioni di memoria locale (local)

##### get_local (0x20), set_local (0x21), tee_local (0x22)
```
0x20 [indice locale]  // get_local
0x21 [indice locale]  // set_local
0x22 [indice locale]  // tee_local (set e poi get)
```

L'indice locale è un intero unsigned LEB128 che identifica una variabile locale.

#### Istruzioni di memoria globale (global)

##### get_global (0x23), set_global (0x24)
```
0x23 [indice globale]  // get_global
0x24 [indice globale]  // set_global
```

L'indice globale è un intero unsigned LEB128 che identifica una variabile globale.

#### Istruzioni di memoria lineare (load/store)

##### i32.load (0x28), i32.store (0x36)
```
0x28 [flag di allineamento] [offset]  // i32.load
0x36 [flag di allineamento] [offset]  // i32.store
```

- `flag di allineamento` è un intero unsigned LEB128 che specifica l'allineamento (es. 2 significa allineato a 4 byte)
- `offset` è un intero unsigned LEB128 che specifica l'offset di memoria

Lo stesso schema si applica per i64, f32, f64 con opcode diversi:
- `i64.load` (0x29), `i64.store` (0x37)
- `f32.load` (0x2A), `f32.store` (0x38)
- `f64.load` (0x2B), `f64.store` (0x39)

### Istruzioni aritmetiche e logiche

#### Operazioni i32

```
0x41 [valore immediato]  // i32.const
0x45                    // i32.eqz
0x46                    // i32.eq
0x47                    // i32.ne
0x48                    // i32.lt_s (signed less than)
0x49                    // i32.lt_u (unsigned less than)
0x4A                    // i32.gt_s (signed greater than)
0x4B                    // i32.gt_u (unsigned greater than)
0x4C                    // i32.le_s (signed less than or equal)
0x4D                    // i32.le_u (unsigned less than or equal)
0x4E                    // i32.ge_s (signed greater than or equal)
0x4F                    // i32.ge_u (unsigned greater than or equal)
0x6A                    // i32.add
0x6B                    // i32.sub
0x6C                    // i32.mul
0x6D                    // i32.div_s (signed division)
0x6E                    // i32.div_u (unsigned division)
```

#### Operazioni i64

```
0x42 [valore immediato]  // i64.const
0x50                    // i64.eqz
0x51                    // i64.eq
0x52                    // i64.ne
0x53                    // i64.lt_s
0x54                    // i64.lt_u
0x55                    // i64.gt_s
0x56                    // i64.gt_u
0x57                    // i64.le_s
0x58                    // i64.le_u
0x59                    // i64.ge_s
0x5A                    // i64.ge_u
0x7C                    // i64.add
0x7D                    // i64.sub
0x7E                    // i64.mul
0x7F                    // i64.div_s
0x80                    // i64.div_u
```

## Esempi di interpretazione

### Esempio 1: Una semplice funzione che somma due numeri

```
20 00       // get_local 0
20 01       // get_local 1
6A          // i32.add
0B          // end
```

Questa sequenza:
1. Carica il valore della variabile locale 0 sullo stack
2. Carica il valore della variabile locale 1 sullo stack
3. Somma i due valori in cima allo stack
4. Termina la funzione (implicitamente ritornando il valore in cima allo stack)

### Esempio 2: Una funzione con un'istruzione if

```
20 00       // get_local 0
20 01       // get_local 1
4A          // i32.gt_s (controlla se locale 0 > locale 1)
04 40       // if (void)
  20 00     // get_local 0
  0B        // end (del corpo dell'if)
05          // else
  20 01     // get_local 1
  0B        // end (del corpo dell'else)
0B          // end (della funzione)
```

Questa sequenza:
1. Carica i valori delle variabili locali 0 e 1
2. Confronta se il locale 0 è maggiore del locale 1 (i32.gt_s)
3. Se il risultato è true (1), esegue il blocco then e restituisce il valore della variabile locale 0
4. Altrimenti, esegue il blocco else e restituisce il valore della variabile locale 1

## Decodifica LEB128

La codifica LEB128 (Little Endian Base 128) è utilizzata per rappresentare numeri interi di dimensione variabile. Ecco come funziona:

1. Ogni byte contiene 7 bit di dati e 1 bit di continuazione
2. Il bit più significativo (MSB) è il bit di continuazione: se è 1, ci sono altri byte che seguono; se è 0, questo è l'ultimo byte
3. I 7 bit di dati sono concatenati, con i byte meno significativi che vengono prima

### Esempio di decodifica LEB128

Supponiamo di avere i byte `E5 8E 26`:

```
E5 = 1110 0101 (bit di continuazione = 1)
8E = 1000 1110 (bit di continuazione = 1)
26 = 0010 0110 (bit di continuazione = 0)
```

Rimuovendo i bit di continuazione e concatenando i bit di dati:
```
E5 -> 110 0101
8E -> 000 1110
26 -> 010 0110

Concatenati: 010 0110 000 1110 110 0101 = 624485
```

## Conclusione

Il bytecode WASM è progettato per essere compatto ed efficiente, con un'enfasi sulla velocità di decodifica. Ogni istruzione ha una struttura chiara: un opcode seguito da operandi codificati in modo appropriato. Le istruzioni di controllo del flusso come `if`, `loop` e `block` hanno una struttura nidificata che utilizza i byte `0x02`, `0x03`, `0x04` per iniziare e `0x0B` per terminare. Le istruzioni di accesso alla memoria, sia locale che globale, specificano un indice, mentre le istruzioni di memoria lineare specificano flag di allineamento e offset.

Comprendere questa struttura è fondamentale per analizzare e manipolare il bytecode WASM, che svolge un ruolo cruciale nell'esecuzione di codice ad alte prestazioni nel web moderno.