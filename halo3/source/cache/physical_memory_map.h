#ifndef __PHYSICAL_MEMORY_MAP_H__
#define __PHYSICAL_MEMORY_MAP_H__
#pragma once

/* ---------- headers */

#include <stdint.h>

/* ---------- constants */

enum memory_stage
{
    _memory_stage_initial = 0,
    _memory_stage_game_initialize,
    _memory_stage_resize_initialize,
    _memory_stage_resize_available,
    _memory_stage_map_configuration,
    _memory_stage_level_initialize,
    _memory_stage_in_level,

    k_memory_stage_count,
};

/* ---------- definitions */

/* ---------- prototypes */

extern void physical_memory_stage_push(memory_stage stage);
extern void physical_memory_stage_pop(memory_stage stage);
extern uint32_t _physical_memory_malloc(memory_stage stage, char const* name, uint32_t size, uint32_t flags, char const* file, int32_t line); // void*

/* ---------- globals */

/* ---------- public code */

/* ---------- private code */

#endif // __PHYSICAL_MEMORY_MAP_H__
