#pragma once
#ifndef __THREAD_LOCAL_STORAGE_H__
#define __THREAD_LOCAL_STORAGE_H__

/* ---------- headers */

#include <stdint.h>

#include "rex_macros.h"

/* ---------- constants */

/* ---------- definitions */

constexpr uint32_t k_tls_game_globals_offset = 0x80;

/* ---------- prototypes */

REX_EXTERN(__imp__get_thread_local_by_offset);

/* ---------- globals */

/* ---------- public code */

template<typename t_type>
t_type* get_thread_local_by_offset(uint32_t offset)
{
	t_type* result = rex::ppc::GuestToHostFunction<t_type*>(__imp__get_thread_local_by_offset, offset);
	return result;
}

#endif // __THREAD_LOCAL_STORAGE_H__