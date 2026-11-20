#define WIN32_LEAN_AND_MEAN 1
#include <windows.h>

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH)
    return DisableThreadLibraryCalls(instance);
  return TRUE;
}
