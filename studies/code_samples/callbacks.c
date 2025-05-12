#include <stdio.h>
#include <stdlib.h>

// Definiamo una struttura che conterrà il puntatore alla funzione callback
typedef struct {
    // Dichiariamo un puntatore a funzione che accetta un int e un double come argomenti
    // e restituisce un float
    float (*callback)(int, double);
    
    // Altri campi della struttura
    int id;
} CallbackHandler;

// Funzione di esempio che useremo come callback
float esempio_callback(int a, double b) {
    float risultato = a + (float)b;
    printf("Callback eseguita: %d + %.2f = %.2f\n", a, b, risultato);
    return risultato;
}

// Funzione per impostare il callback nella struttura
void imposta_callback(CallbackHandler *handler, float (*func)(int, double)) {
    handler->callback = func;
    printf("Callback impostata nella struttura\n");
}

// Funzione per chiamare il callback
float chiama_callback(CallbackHandler *handler, int x, double y) {
    if (handler->callback != NULL) {
        return handler->callback(x, y);
    } else {
        printf("Errore: callback non impostata\n");
        return 0.0f;
    }
}

int main() {
    // Creiamo e inizializziamo la struttura
    CallbackHandler handler;
    handler.id = 1;
    handler.callback = NULL;  // Inizializziamo a NULL per sicurezza
    
    // Impostiamo la funzione callback
    imposta_callback(&handler, esempio_callback);
    
    // Chiamiamo la funzione callback
    float risultato = chiama_callback(&handler, 5, 3.5);
    
    printf("Risultato finale: %.2f\n", risultato);
    
    return 0;
}

///
/// Callback alias example
///

#include <stdio.h>

// Definisci l'alias per il tipo di puntatore a funzione
typedef void (*PrepareCallback)(int, double);

// Dichiara la struct utilizzando l'alias
struct MyStruct {
    PrepareCallback prepare;
    // altri membri della struct
};

// Una funzione che corrisponde alla firma della callback
void my_prepare_function(int a, double b) {
    printf("Preparing with int: %d and double: %f\n", a, b);
}

int main() {
    struct MyStruct instance;

    // Assegna la funzione all'interno della struct usando l'alias
    instance.prepare = my_prepare_function;

    // Chiama la callback tramite l'alias nella struct
    instance.prepare(10, 3.14);

    // Puoi anche dichiarare variabili puntatore a funzione direttamente con l'alias
    PrepareCallback another_callback = my_prepare_function;
    another_callback(20, 6.28);

    return 0;
}