#pragma once
// The process id for unique temp folder names, on either platform.
#ifdef _WIN32
#include <windows.h>
inline unsigned long test_process_id() { return GetCurrentProcessId(); }
#else
#include <unistd.h>
inline unsigned long test_process_id() { return static_cast<unsigned long>(getpid()); }
#endif
