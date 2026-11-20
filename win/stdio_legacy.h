#pragma once
#define _NO_CRT_STDIO_INLINE 1

#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// Declare the legacy function exported by msvcrt.dll
_CRTIMP FILE* __cdecl __iob_func(void);

// The compiler just checks for the presence of this symbol name.
// Defining it as an int satisfies the symbol resolution.
extern int _fltused;

// Provide the missing modern symbol that the compiler expects
extern FILE* __cdecl __acrt_iob_func(unsigned index);

extern int* __cdecl __p__fmode(void);

// By overriding this, you lose global C++ constructor calls and the standard memory heap!
extern int __stdcall _DllMainCRTStartup(void* hinstDLL, unsigned int fdwReason, void* lpvReserved);

// extern void __chkstk(void);

#ifdef __cplusplus
}
#endif
