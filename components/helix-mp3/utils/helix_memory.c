#include "helix_memory.h"
#include <stdlib.h>

void* helix_malloc(int size) {
    return malloc(size);
}

void helix_free(void* ptr) {
    free(ptr);
}
