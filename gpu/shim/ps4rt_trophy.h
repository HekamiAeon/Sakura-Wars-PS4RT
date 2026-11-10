// SPDX-License-Identifier: GPL-2.0-or-later
// Guest libSceNpTrophy layouts, following shadPS4's np_trophy.h (2025-2026 contributors).
// The implementation is independent of the renderer and the original game.
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct {
    size_t size;
    int32_t trophy_id, grade, group_id;
    uint8_t hidden, reserved[3];
    char name[128], description[1024];
} Ps4RtTrophyDetails;
typedef struct {
    size_t size;
    int32_t trophy_id;
    uint8_t unlocked, reserved[3];
    uint64_t timestamp;
} Ps4RtTrophyData;
typedef struct {
    size_t size;
    uint32_t num_groups, num_trophies, num_platinum, num_gold, num_silver, num_bronze;
    char title[128], description[1024];
} Ps4RtTrophyGameDetails;
typedef struct {
    size_t size;
    uint32_t unlocked_trophies, unlocked_platinum, unlocked_gold, unlocked_silver,
             unlocked_bronze, progress_percentage;
} Ps4RtTrophyGameData;

#ifdef __cplusplus
extern "C" {
#endif
int32_t ps4rt_trophy_create_context(int32_t*, int32_t, uint32_t, uint64_t);
int32_t ps4rt_trophy_create_handle(int32_t*);
int32_t ps4rt_trophy_register(int32_t, int32_t, uint64_t);
int32_t ps4rt_trophy_unlock(int32_t, int32_t, int32_t, int32_t*);
int32_t ps4rt_trophy_game_info(int32_t, int32_t, Ps4RtTrophyGameDetails*, Ps4RtTrophyGameData*);
int32_t ps4rt_trophy_info(int32_t, int32_t, int32_t, Ps4RtTrophyDetails*, Ps4RtTrophyData*);
int32_t ps4rt_trophy_unlock_state(int32_t, int32_t, uint32_t*, uint32_t*);
int32_t ps4rt_trophy_abort(int32_t);
int32_t ps4rt_trophy_destroy_handle(int32_t);
int32_t ps4rt_trophy_destroy_context(int32_t);
#ifdef __cplusplus
}
#endif
