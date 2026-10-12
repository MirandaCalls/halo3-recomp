#ifndef __RUNTIME_STATE_H__
#define __RUNTIME_STATE_H__
#pragma once

/* ---------- headers */

#include <stdint.h>

/* ---------- constants */

/* ---------- definitions */

struct s_runtime_state_globals;

/* ---------- prototypes */

extern void runtime_state_shell_initialize();
extern void runtime_state_shell_dispose();
extern void runtime_state_initialize();
extern void runtime_state_dispose();
extern void runtime_state_initialize_for_new_map();
extern void runtime_state_dispose_from_old_map();
extern uint32_t runtime_state_get_buffer_address(int32_t* buffer_size); // void const*

// runtime state windows

extern uint32_t runtime_state_allocate_buffer(int32_t size); // void*
extern void runtime_state_free_buffer(uint32_t buffer); // void*

/* ---------- globals */

//extern s_runtime_state_globals g_runtime_state_globals;
extern s_runtime_state_globals& g_runtime_state_globals;

/* ---------- public code */

/* ---------- private code */

#endif // __RUNTIME_STATE_H__
