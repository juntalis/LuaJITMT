#define VC_EXTRALEAN 1
#define WIN32_LEAN_AND_MEAN 1
#define _NO_CRT_STDIO_INLINE 1
#pragma comment(lib, "kernel32.lib")
#pragma comment(lib, "shell32.lib")
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>


// Declare your standard main function
int main(int argc, char* argv[]);

// The true entry point for a console application
void mainCRTStartup(void)
{
    int exitCode;
    int argc = 0;
    char** argv = NULL;

    // 1. Get the full command line as a wide string
    wchar_t* cmdLineW = GetCommandLineW();

    if (cmdLineW != NULL) {
        // 2. Parse wide string command line into a standard argv array
        wchar_t** wargv = CommandLineToArgvW(cmdLineW, &argc);
        
        if (wargv != NULL) {
            // 3. Convert wide character arguments (UTF-16) to ANSI/UTF-8 (char**)
            argv = (char**)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(char*) * (argc + 1));
            
            if (argv != NULL) {
                for (int i = 0; i < argc; i++) {
                    // Determine required buffer size for UTF-8/ANSI conversion
                    int size = WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, NULL, 0, NULL, NULL);
                    if (size > 0) {
                        argv[i] = (char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size);
                        if (argv[i] != NULL) {
                            WideCharToMultiByte(CP_UTF8, 0, wargv[i], -1, argv[i], size, NULL, NULL);
                        }
                    }
                }
                argv[argc] = NULL; // Null-terminate the array
            }
            
            // Free the memory allocated by CommandLineToArgvW
            LocalFree(wargv);
        }
    }

    // Fallback if parsing completely failed
    if (argc == 0 || argv == NULL) {
        argc = 1;
        argv = (char**)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(char*) * 2);
        if (argv != NULL) {
            argv[0] = "";
            argv[1] = NULL;
        }
    }

    // 4. Call the user's main function
    exitCode = main(argc, argv);

    // 5. Clean up allocated ANSI/UTF-8 arguments
    if (argv != NULL) {
        for (int i = 0; i < argc; i++) {
            if (argv[i] && argv[i][0] != '\0') {
                HeapFree(GetProcessHeap(), 0, argv[i]);
            }
        }
        HeapFree(GetProcessHeap(), 0, argv);
    }

    // 6. Exit the process cleanly
    ExitProcess(exitCode);
}
