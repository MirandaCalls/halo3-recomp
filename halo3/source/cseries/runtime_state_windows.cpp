/* ---------- headers */

#include "cseries/runtime_state.h"

#include "cache/physical_memory_map.h"

/* ---------- constants */

/* ---------- definitions */

/* ---------- prototypes */

/* ---------- globals */

/* ---------- private variables */

/* ---------- public code */

uint32_t runtime_state_allocate_buffer(int32_t size) // void*
{
    uint32_t result = _physical_memory_malloc(
        _memory_stage_game_initialize,
        "runtime state (not game state)",
        size,
        1,
        __FILE__,
        __LINE__);
    return result;
}

void runtime_state_free_buffer(uint32_t buffer) // void*
{
    //physical_memory_free(buffer);
}

/* ---------- private code */
