#include "stdio_legacy.h"

// The compiler just checks for the presence of this symbol name.
// Defining it as an int satisfies the symbol resolution.
int _fltused = 0x9876;

#if defined(_WIN64)
    #define CRT_FILE_SIZE 48
#else
    #define CRT_FILE_SIZE 32
#endif

// Provide the missing modern symbol that the compiler expects
FILE* __cdecl __acrt_iob_func(unsigned index) {
    // Note: This relies on the standard stream sizing structure from the old CRT.
    // Index 0 = stdin, 1 = stdout, 2 = stderr
    return (FILE*)((char*)__iob_func() + (index * CRT_FILE_SIZE));
}

int* __cdecl __p__fmode(void) {
	extern int _fmode;
	return &_fmode;
}

// void __chkstk() {}
    // A minimal stub if your code strictly avoids massive stack allocations.
    // Real __chkstk handles page walking; an empty stub assumes you won't blow past a page guard.

