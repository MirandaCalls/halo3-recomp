/* ---------- headers */

#include "cseries/cseries_windows.h"

#include "rex_macros.h"

#include <time.h>

#if !defined(_WIN32)
#include <chrono>
#endif

/* ---------- constants */

/* ---------- definitions */

/* ---------- prototypes */

/* ---------- globals */

/* ---------- private variables */

/* ---------- ppc */

// exports

REX_PPC_EXTERN_IMPORT(GetTickCount);

// hooks

/* ---------- public code */

uint32_t system_milliseconds(void)
{
#if defined(_WIN32)
	return GetTickCount();
#else
	// host stand-in for Win32 GetTickCount: a monotonic millisecond counter
	auto const now = std::chrono::steady_clock::now().time_since_epoch();
	return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
#endif
}

/* ---------- private code */
