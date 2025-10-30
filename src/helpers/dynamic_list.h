/**
 * @file dynamic_list.h
 * @brief A header-only dynamic list (vector) for void pointers in C.
 *
 * ## Description
 * This library provides a simple, efficient, and easy-to-use dynamic array
 * (often called a vector) for storing pointers. It automatically handles memory
 * allocation and resizing.
 *
 * It is designed to behave like a standard array for element access, allowing
 * you to use the standard index notation (e.g., `my_list->items[i]`).
 *
 * ## Usage
 * 1.  Copy this file into your project.
 * 2.  In the C file where you want the implementation to be generated, add
 * the following lines:
 * ```c
 * #define LIST_IMPLEMENTATION
 * #include "dynamic_list.h"
 * ```
 * 3.  In all other files where you need to use the list, just include the header:
 * ```c
 * #include "dynamic_list.h"
 * ```
 *
 * ## Example
 * ```c
 * #include <stdio.h>
 * #include <string.h>
 *
 * // Define LIST_IMPLEMENTATION in one C file
 * #define LIST_IMPLEMENTATION
 * #include "dynamic_list.h"
 *
 * int main() {
 * // Create a list with an initial capacity of 4
 * list_t* my_list = list_create(4);
 * if (!my_list) return 1;
 *
 * // Push some string literals onto the list
 * char* str1 = "Hello";
 * char* str2 = "World";
 * char* str3 = "!";
 * list_push(my_list, str1);
 * list_push(my_list, str2);
 * list_push(my_list, str3);
 *
 * // Access elements like a normal array
 * printf("Accessing elements with array syntax:\n");
 * for (size_t i = 0; i < list_count(my_list); ++i) {
 * printf("  my_list->items[%zu] = %s\n", i, (char*)my_list->items[i]);
 * }
 *
 * // Insert an item
 * char* str_inserted = "C ";
 * list_insert(my_list, 1, str_inserted); // Insert at index 1
 * printf("\nAfter inserting '%s' at index 1:\n", str_inserted);
 * for (size_t i = 0; i < list_count(my_list); ++i) {
 * printf("  my_list->items[%zu] = %s\n", i, (char*)my_list->items[i]);
 * }
 *
 * // Pop an item from the end
 * char* popped_item = (char*)list_pop(my_list);
 * printf("\nPopped item: %s\n", popped_item);
 *
 * // Remove an item from a specific index
 * char* removed_item = (char*)list_remove(my_list, 0); // Remove "Hello"
 * printf("Removed item at index 0: %s\n", removed_item);
 *
 * printf("\nFinal list state:\n");
 * for (size_t i = 0; i < list_count(my_list); ++i) {
 * printf("  my_list->items[%zu] = %s\n", i, (char*)list_get(my_list, i));
 * }
 *
 * // Clean up
 * list_destroy(my_list);
 *
 * return 0;
 * }
 * ```
 */

#ifndef DYNAMIC_LIST_H
#define DYNAMIC_LIST_H

#include <stddef.h> // For size_t

//==============================================================================
// API Declaration
//==============================================================================

/**
 * @struct list_t
 * @brief The main structure for the dynamic list.
 *
 * @var list_t::items
 * A pointer to the dynamically allocated array of void pointers.
 * **You can use this directly for array-like access, e.g., `my_list->items[i]`**.
 * @var list_t::count
 * The number of items currently stored in the list.
 * @var list_t::capacity
 * The total number of items the list can hold before needing to resize.
 */
typedef struct {
    void** items;
    size_t  count;
    size_t  capacity;
} list_t;


/**
 * @brief Creates and initializes a new list.
 * @param initial_capacity The initial number of elements the list can hold.
 * A default of 0 is safe and will cause allocation on the first push.
 * @return A pointer to the newly created list, or NULL if allocation fails.
 */
list_t* list_create(size_t initial_capacity);

/**
 * @brief Frees all memory associated with the list.
 * @note This does NOT free the data pointed to by the elements in the list.
 * The user is responsible for managing that memory.
 * @param list The list to destroy.
 */
void list_destroy(list_t* list);

/**
 * @brief Appends an item to the end of the list.
 * Amortized O(1) time complexity.
 * @param list The list to append to.
 * @param item The pointer to add to the list.
 * @return 0 on success, -1 on failure (memory allocation failed).
 */
int list_push(list_t* list, void* item);

/**
 * @brief Removes and returns the last item from the list.
 * O(1) time complexity.
 * @param list The list to pop from.
 * @return The last item, or NULL if the list is empty.
 */
void* list_pop(list_t* list);

/**
 * @brief Inserts an item at a specific index, shifting subsequent items.
 * O(n) time complexity.
 * @param list The list to insert into.
 * @param index The index at which to insert the item. Must be <= list->count.
 * @param item The pointer to insert.
 * @return 0 on success, -1 on failure (invalid index or memory allocation failed).
 */
int list_insert(list_t* list, size_t index, void* item);

/**
 * @brief Removes and returns an item from a specific index, shifting subsequent items.
 * O(n) time complexity.
 * @param list The list to remove from.
 * @param index The index of the item to remove. Must be < list->count.
 * @return The removed item, or NULL if the index is out of bounds.
 */
void* list_remove(list_t* list, size_t index);


//==============================================================================
// Helper Macros for convenience & array-like behavior
//==============================================================================

/**
 * @brief Gets the item at a specific index. Provides a safe alternative
 * to direct array access if you prefer function-like syntax.
 */
#define list_get(list, index) ((list)->items[index])

/**
 * @brief Returns the current number of items in the list.
 */
#define list_count(list) ((list)->count)


#endif // DYNAMIC_LIST_H

//==============================================================================
// API Implementation
//==============================================================================

#ifdef LIST_IMPLEMENTATION

#include <stdlib.h> // For malloc, realloc, free
#include <string.h> // For memmove

// A sensible default initial capacity if 0 is provided.
#define LIST_DEFAULT_CAPACITY 8
// The factor by which the list grows when its capacity is reached.
#define LIST_GROWTH_FACTOR 2

/**
 * @brief (Internal) Resizes the list to a new capacity.
 * @return 0 on success, -1 on failure.
 */
static int list_resize(list_t* list, size_t new_capacity) {
    if (new_capacity < list->count) {
        // Cannot resize smaller than the current number of items.
        return -1;
    }

    void** new_items = (void**)realloc(list->items, new_capacity * sizeof(void*));
    if (!new_items) {
        return -1; // realloc failed
    }

    list->items = new_items;
    list->capacity = new_capacity;
    return 0;
}

list_t* list_create(size_t initial_capacity) {
    list_t* list = (list_t*)malloc(sizeof(list_t));
    if (!list) {
        return NULL;
    }

    list->count = 0;
    list->capacity = (initial_capacity > 0) ? initial_capacity : 0;
    
    if (list->capacity > 0) {
        list->items = (void**)malloc(list->capacity * sizeof(void*));
        if (!list->items) {
            free(list);
            return NULL;
        }
    } else {
        list->items = NULL; // No allocation until first push
    }

    return list;
}

void list_destroy(list_t* list) {
    if (list) {
        free(list->items); // free(NULL) is safe
        free(list);
    }
}

int list_push(list_t* list, void* item) {
    if (!list) return -1;

    // If list is full or uninitialized, resize it
    if (list->count >= list->capacity) {
        size_t new_capacity = (list->capacity == 0) ? LIST_DEFAULT_CAPACITY : list->capacity * LIST_GROWTH_FACTOR;
        if (list_resize(list, new_capacity) != 0) {
            return -1; // Resize failed
        }
    }

    list->items[list->count++] = item;
    return 0;
}

void* list_pop(list_t* list) {
    if (!list || list->count == 0) {
        return NULL;
    }
    // Decrement count first, then return the item at the new (old) count index
    return list->items[--list->count];
}

int list_insert(list_t* list, size_t index, void* item) {
    if (!list || index > list->count) {
        // Allow insertion at the end (index == list->count)
        return -1;
    }

    // Check if resize is needed
    if (list->count >= list->capacity) {
        size_t new_capacity = (list->capacity == 0) ? LIST_DEFAULT_CAPACITY : list->capacity * LIST_GROWTH_FACTOR;
        if (list_resize(list, new_capacity) != 0) {
            return -1;
        }
    }

    // Shift elements to the right to make space for the new item
    // memmove is safe for overlapping memory regions
    if (index < list->count) {
        memmove(&list->items[index + 1], &list->items[index], (list->count - index) * sizeof(void*));
    }

    list->items[index] = item;
    list->count++;
    return 0;
}

void* list_remove(list_t* list, size_t index) {
    if (!list || index >= list->count) {
        return NULL;
    }

    void* removed_item = list->items[index];

    // Shift elements to the left to fill the gap
    if (index < list->count - 1) {
        memmove(&list->items[index], &list->items[index + 1], (list->count - index - 1) * sizeof(void*));
    }

    list->count--;
    return removed_item;
}


#endif // LIST_IMPLEMENTATION