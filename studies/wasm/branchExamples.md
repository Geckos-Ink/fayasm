Ecco un esempio di bytecode WebAssembly (WASM) che mostra come funzionano le chiamate a funzioni, strutture di controllo if/else e altri controlli essenziali di flusso:

```
;; Esempio di bytecode WASM (rappresentazione letterale delle istruzioni)

;; Dichiarazione delle funzioni
func $somma (param $a i32) (param $b i32) (result i32)
  local.get $a
  local.get $b
  i32.add
end

func $calcola (param $x i32) (result i32)
  ;; Dichiarazione di una variabile locale
  (local $risultato i32)

  ;; Esempio di controllo if/else
  local.get $x
  i32.const 10
  i32.lt_s
  if (result i32)
    ;; Ramo if: se x < 10
    i32.const 5
    local.get $x
    call $somma
  else
    ;; Ramo else: se x >= 10
    local.get $x
    i32.const 2
    i32.mul
  end
  
  ;; Memorizza il risultato nella variabile locale
  local.set $risultato
  
  ;; Esempio di loop
  block $exit_loop
    loop $continue_loop
      local.get $risultato
      i32.const 100
      i32.gt_s
      br_if $exit_loop   ;; Esce dal loop se risultato > 100
      
      local.get $risultato
      i32.const 5
      i32.add
      local.set $risultato
      
      br $continue_loop  ;; Salta all'inizio del loop
    end
  end
  
  local.get $risultato
  return
end

;; Funzione principale
func $main
  i32.const 8
  call $calcola
  drop
end
```

Ed ecco la rappresentazione in bytecode effettivo (come apparirebbe nel binario finale):

```
;; Funzione $somma
20 00       ;; local.get $a (0)
20 01       ;; local.get $b (1)
6A          ;; i32.add
0B          ;; end

;; Funzione $calcola
20 00       ;; local.get $x (0)
41 0A       ;; i32.const 10
48          ;; i32.lt_s
04 7F       ;; if (result i32)
  41 05     ;; i32.const 5
  20 00     ;; local.get $x
  10 00     ;; call $somma (0)
05          ;; else
  20 00     ;; local.get $x 
  41 02     ;; i32.const 2
  6C        ;; i32.mul
0B          ;; end

21 01       ;; local.set $risultato (1)

02 40       ;; block $exit_loop
  03 40     ;; loop $continue_loop
    20 01   ;; local.get $risultato
    41 64   ;; i32.const 100
    48      ;; i32.gt_s
    0D 01   ;; br_if $exit_loop (1)
    
    20 01   ;; local.get $risultato
    41 05   ;; i32.const 5
    6A      ;; i32.add
    21 01   ;; local.set $risultato
    
    0C 00   ;; br $continue_loop (0)
  0B        ;; end loop
0B          ;; end block

20 01       ;; local.get $risultato
0F          ;; return

;; Funzione $main
41 08       ;; i32.const 8
10 01       ;; call $calcola (1)
1A          ;; drop
0B          ;; end
```

Elementi principali del bytecode WASM:

1. **Chiamate a funzioni**:
   - `10 XX` - `call` seguito dall'indice della funzione
   - I parametri vengono inseriti nello stack prima della chiamata
   - I valori di ritorno sono lasciati in cima allo stack

2. **Strutture if/else**:
   - `04 XX` - `if` con tipo risultato (7F = i32)
   - `05` - `else`
   - `0B` - `end` (chiude il blocco if/else)
   - La condizione viene verificata prima dell'istruzione if (0 = falso, diverso da 0 = vero)

3. **Cicli e salti**:
   - `02 XX` - `block` definisce un punto di salto
   - `03 XX` - `loop` definisce l'inizio di un ciclo
   - `0C XX` - `br` salto incondizionato all'etichetta specificata
   - `0D XX` - `br_if` salto condizionato all'etichetta se la condizione è vera

4. **Manipolazione variabili**:
   - `20 XX` - `local.get` carica il valore di una variabile locale
   - `21 XX` - `local.set` imposta il valore di una variabile locale
   - `41 XX` - `i32.const` carica una costante intera a 32 bit

5. **Operazioni**:
   - `6A` - `i32.add` addizione
   - `6C` - `i32.mul` moltiplicazione
   - `48` - `i32.lt_s` confronto "minore di" con segno

Questo esempio illustra i principali meccanismi di controllo di flusso in WebAssembly nel loro formato di bytecode finale.