// SPDX-License-Identifier: Apache-2.0
#include <stdio.h>
#include "game.h"
#include "game_config.h"
#include "mosaico_game_module.h"
#include "mosaico_raylib_fast.h"

typedef struct {
    game_handle_t game;
} host_state_t;

static int initialize(void *value, const char *asset_root)
{
    (void)asset_root;
    host_state_t *state = value;
    const game_config_t config = {GAME_WIDTH, GAME_HEIGHT};
    if (!game_create(&config, &state->game)) return -1;
    InitWindow(GAME_WIDTH, GAME_HEIGHT, GAME_NAME);
    SetTargetFPS(GAME_TICK_HZ);
    return 0;
}

static void shutdown(void *value)
{
    host_state_t *state = value;
    game_delete(state->game);
    state->game = NULL;
}

static void input(void *value, const mosaico_host_input_v1_t *event)
{
    host_state_t *state = value;
    if (!event) return;
    if (event->type == MOSAICO_HOST_INPUT_POINTER && event->track_id == 0) {
        game_set_pointer(state->game, event->x, event->y, event->pressed);
    } else if (event->type == MOSAICO_HOST_INPUT_IMU) {
        game_set_imu(state->game, event->value_x, event->value_y, event->value_z);
    } else if (event->type == MOSAICO_HOST_INPUT_CONTROL) {
        switch (event->code) {
        case MOSAICO_HOST_CONTROL_PAUSE: game_set_paused(state->game, true); break;
        case MOSAICO_HOST_CONTROL_RESUME: game_set_paused(state->game, false); break;
        case MOSAICO_HOST_CONTROL_RESET: game_reset(state->game); break;
        default: break;
        }
    } else if (event->type == MOSAICO_HOST_INPUT_ACTION) {
        if (event->code == GAME_ACTION_PAUSE && event->pressed)
            game_set_paused(state->game, !game_read(state->game).paused);
        else
            game_set_action(state->game, event->code, event->pressed);
    }
}

static void update(void *value)
{
    game_update(((host_state_t *)value)->game);
}

static int render(void *value)
{
    return game_render(((host_state_t *)value)->game) ? 0 : -1;
}

static uint32_t state_hash(const void *value)
{
    const game_snapshot_t state = game_read(((const host_state_t *)value)->game);
    /* Hash named fields, avoiding platform-dependent struct padding. */
    uint32_t hash = state.tick;
    hash = hash * 31U + (uint32_t)state.phase;
    hash = hash * 31U + state.time_centis;
    hash = hash * 31U + state.best_centis;
    hash = hash * 31U + (uint32_t)state.player_x;
    hash = hash * 31U + state.rotation;
    hash = hash * 31U + state.key_left;
    hash = hash * 31U + state.key_right;
    hash = hash * 31U + state.touch_left;
    hash = hash * 31U + state.touch_right;
    hash = hash * 31U + state.pointer_down;
    return hash * 31U + state.paused;
}

static const char *phase_name(game_phase_t phase)
{
    switch (phase) {
    case GAME_PHASE_PLAYING: return "playing";
    case GAME_PHASE_GAME_OVER: return "game_over";
    case GAME_PHASE_WIN: return "win";
    case GAME_PHASE_TITLE:
    default: return "title";
    }
}

static int state_json(const void *value, char *output, size_t capacity)
{
    if (!output || !capacity) return -1;
    const game_snapshot_t state = game_read(((const host_state_t *)value)->game);
    int size = snprintf(output, capacity,
        "{\"phase\":\"%s\",\"tick\":%lu,\"time_centis\":%lu,\"best_centis\":%lu,"
        "\"player_x\":%ld,\"rotation\":%u,\"paused\":%s,"
        "\"state_hash\":\"%08lx\"}",
        phase_name(state.phase), (unsigned long)state.tick,
        (unsigned long)state.time_centis, (unsigned long)state.best_centis,
        (long)state.player_x, (unsigned)state.rotation,
        state.paused ? "true" : "false", (unsigned long)state_hash(value));
    return size < 0 || (size_t)size >= capacity ? -1 : size;
}

static const mosaico_game_module_v1_t s_module = {
    .descriptor = {MOSAICO_HOST_GAME_ABI_V1, GAME_NAME, GAME_NAME,
                   GAME_WIDTH, GAME_HEIGHT, GAME_TICK_HZ, 1},
    .state_size = sizeof(host_state_t),
    .initialize = initialize, .shutdown = shutdown, .input = input,
    .update = update, .render = render,
    .state_hash = state_hash, .state_json = state_json,
};

const mosaico_game_module_v1_t *mosaico_game_module_v1(void)
{
    return &s_module;
}
