// SPDX-License-Identifier: Apache-2.0
#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct game_t *game_handle_t;
typedef struct {
    uint16_t width;
    uint16_t height;
} game_config_t;

/* Survival target: 60 s of dodging, "是男人就坚持60秒" style. */
#define GAME_TARGET_CENTIS 6000U
#define GAME_LEADERBOARD_SIZE 5

typedef enum {
    GAME_PHASE_TITLE = 0,
    GAME_PHASE_PLAYING,
    GAME_PHASE_GAME_OVER,
    GAME_PHASE_WIN,
} game_phase_t;

/* Attachment edge of the interaction module, expressed as the clockwise angle
 * of that edge in the device frame (0=up). 0 means no module is attached. */
enum { GAME_MODULE_NONE = 0, GAME_MODULE_RIGHT_EDGE = 90, GAME_MODULE_LEFT_EDGE = 270 };

typedef struct {
    uint32_t tick;
    game_phase_t phase;
    /* Survival time of the current run and the best leaderboard entry. */
    uint32_t time_centis;
    uint32_t best_centis;
    int32_t player_x;
    int32_t player_y;
    /* Rendered rotation in degrees, clockwise from view to device frame. */
    uint16_t rotation;
    /* Raw input state, mirrored for the Host JSON state. */
    bool key_left;
    bool key_right;
    bool touch_left;
    bool touch_right;
    bool paused;
    int32_t pointer_x;
    int32_t pointer_y;
    bool pointer_down;
} game_snapshot_t;

/* Leaderboard: survival times in centiseconds, sorted descending.
 * On the Host it lives in RAM; on the device main.c seeds it from NVS and
 * persists it through the save callback. */
void game_set_leaderboard(const uint32_t *times_centi, int count);
int game_get_leaderboard(uint32_t *out_times, int max);
typedef void (*game_leaderboard_save_cb_t)(const uint32_t *times, int count,
                                           void *user);
void game_set_leaderboard_save_cb(game_leaderboard_save_cb_t cb, void *user);

/* Portable C shared by Host and device. One loop owns each instance and calls
 * these methods; input adapters enqueue or deliver events on that same loop. */
bool game_create(const game_config_t *config, game_handle_t *ret_handle);
void game_delete(game_handle_t handle);
void game_reset(game_handle_t handle);
void game_set_paused(game_handle_t handle, bool paused);
void game_set_pointer(game_handle_t handle, int32_t x, int32_t y, bool pressed);
/* Action codes follow the shared Mosaico convention: 0=left, 1=right,
 * 2=jump, 3=pause, 4=restart, 5=back, 6=fire. */
void game_set_action(game_handle_t handle, int32_t code, bool pressed);
/* Gravity in g, device frame (x right, y down). Drives screen rotation. */
void game_set_imu(game_handle_t handle, float gx, float gy, float gz);
/* Absolute vertical target in view pixels; the plane steers toward it at a
 * capped speed. Feeds the camera head-tracking control. */
void game_set_vertical_target(game_handle_t handle, int32_t y);
/* Interaction module attachment edge; one of the GAME_MODULE_* angles. */
void game_set_module_edge(game_handle_t handle, int32_t edge_angle);
void game_update(game_handle_t handle);
bool game_render(game_handle_t handle);
game_snapshot_t game_read(game_handle_t handle);
