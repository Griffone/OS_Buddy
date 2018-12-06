#include <stddef.h>

/// Test the buddy allocation algorithm
void verboseTest();

/// Prints states of free lists
void printFreeLists();

/// Allocate size bytes
///
/// Allocates size bytes of memory and returns the address of allocated memory
/// Uses custom Buddy algorithm to manage the memory and avoid unnecessary kernel traps
void *balloc(size_t size);

/// Free memory used by given address
///
/// Frees up memory using Buddy algorithm, allowing reusing said memory
void bfree(void *memory);
