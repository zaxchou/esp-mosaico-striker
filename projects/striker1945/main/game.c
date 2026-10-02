// SPDX-License-Identifier: Apache-2.0
// Striker 1945: "是男人就坚持60秒" — survive sixty seconds of bullet hell.
// One life, no shooting: steer left/right, dodge, watch the timer climb.
// Survive to 60.00 s and the screen celebrates; every run seeds a
// survival-time leaderboard (NVS-backed on the device, RAM on the Host).
//
// The canvas always renders in view coordinates; a gravity-driven rotation
// maps view to device so the interaction module appears at the bottom of the
// screen. Rotation is quantized to 90-degree steps around the square center.
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "game.h"
#include "game_config.h"
#include "mosaico_raylib_fast.h"

#define PLAYER_SPEED 6
#define PLAYER_RADIUS 9
#define PLAYER_Y_DEFAULT (GAME_HEIGHT - 120)
#define VERTICAL_SPEED 5

#define ENEMY_MAX 12
#define BULLET_MAX 96
#define BULLET_RADIUS 5
#define EXPLOSION_MAX 12
#define STAR_MAX 48

/* Invisible touch zones (module keys and these zones share the steering). */
#define CONTROL_TOP 388
#define LEFT_ZONE (Rectangle){8, CONTROL_TOP, 150, 84}
#define RIGHT_ZONE (Rectangle){322, CONTROL_TOP, 150, 84}

typedef enum {
    ENEMY_SCOUT = 0,   /* dives, weaves, aimed singles */
    ENEMY_STRIKER,     /* 3-way aimed volleys */
    ENEMY_TANK,        /* slow, ring bursts */
} enemy_kind_t;

typedef struct {
    bool active;
    enemy_kind_t kind;
    float x, y;
    float vy;
    float base_x;
    float weave_phase;
    int fire_cooldown;
    int fire_interval;
} enemy_t;

typedef struct {
    bool active;
    float x, y;
    float vx, vy;
} bullet_t;

typedef struct {
    bool active;
    float x, y;
    int age;
} explosion_t;

typedef struct {
    float x, y;
    float speed;
} star_t;

/* Leaderboard is process-global: the game module pattern instantiates a
 * single game per process, and the setters have no handle parameter. */
static uint32_t s_leaderboard[GAME_LEADERBOARD_SIZE];
static game_leaderboard_save_cb_t s_save_cb;
static void *s_save_user;

void game_set_leaderboard(const uint32_t *times_centi, int count)
{
    if (!times_centi || count <= 0) return;
    if (count > GAME_LEADERBOARD_SIZE) count = GAME_LEADERBOARD_SIZE;
    for (int i = 0; i < count; ++i) s_leaderboard[i] = times_centi[i];
    for (int i = count; i < GAME_LEADERBOARD_SIZE; ++i) s_leaderboard[i] = 0;
    /* Sort descending (insertion, five entries). */
    for (int i = 1; i < GAME_LEADERBOARD_SIZE; ++i) {
        uint32_t key = s_leaderboard[i];
        int j = i - 1;
        while (j >= 0 && s_leaderboard[j] < key) {
            s_leaderboard[j + 1] = s_leaderboard[j];
            --j;
        }
        s_leaderboard[j + 1] = key;
    }
}

int game_get_leaderboard(uint32_t *out_times, int max)
{
    if (!out_times || max <= 0) return 0;
    if (max > GAME_LEADERBOARD_SIZE) max = GAME_LEADERBOARD_SIZE;
    for (int i = 0; i < max; ++i) out_times[i] = s_leaderboard[i];
    return max;
}

void game_set_leaderboard_save_cb(game_leaderboard_save_cb_t cb, void *user)
{
    s_save_cb = cb;
    s_save_user = user;
}

struct game_t {
    game_config_t config;
    game_snapshot_t state;

    /* Deterministic PRNG keeps headless replays repeatable. */
    uint32_t rng;

    /* Rotation sources: attachment edge angle of the interaction module
     * (0 = none) and the last settled gravity quadrant in the device frame. */
    int32_t module_edge;
    int gravity_quadrant;

    /* Survival run clock in ticks; centiseconds derive from it. */
    uint32_t run_ticks;
    int rain_cooldown;
    int wall_cooldown;
    int dart_cooldown;
    int spawn_cooldown;
    int scroll;

    float player_vx;
    float last_dir;
    int32_t player_y;
    int32_t vertical_target;
    bool camera_vertical;      /* camera wrote the target recently */
    uint32_t camera_target_tick;
    bool key_up;
    bool key_down;

    enemy_t enemies[ENEMY_MAX];
    bullet_t bullets[BULLET_MAX];
    explosion_t explosions[EXPLOSION_MAX];
    star_t stars[STAR_MAX];
};

static uint32_t rng_next(struct game_t *game)
{
    uint32_t x = game->rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    game->rng = x ? x : 0x9e3779b9U;
    return game->rng;
}

static float rand_range(struct game_t *game, float lo, float hi)
{
    return lo + (float)(rng_next(game) % 1000U) / 1000.0f * (hi - lo);
}

static uint32_t run_centis(const struct game_t *game)
{
    return game->run_ticks * 100U / GAME_TICK_HZ;
}

static void update_rotation(struct game_t *game)
{
    /* With the module attached, rotate so its edge lands at the bottom of the
     * view; without it, keep the view upright against gravity. */
    int rotation = game->module_edge
        ? (int)((game->module_edge - 180 + 360) % 360)
        : (game->gravity_quadrant + 180) % 360;
    game->state.rotation = (uint16_t)rotation;
}

static void init_stars(struct game_t *game)
{
    for (int i = 0; i < STAR_MAX; ++i) {
        game->stars[i] = (star_t){
            .x = rand_range(game, 0, GAME_WIDTH),
            .y = rand_range(game, 0, GAME_HEIGHT),
            .speed = rand_range(game, 0.8f, 3.2f),
        };
    }
}

static void clear_field(struct game_t *game)
{
    for (int i = 0; i < ENEMY_MAX; ++i) game->enemies[i].active = false;
    for (int i = 0; i < BULLET_MAX; ++i) game->bullets[i].active = false;
    for (int i = 0; i < EXPLOSION_MAX; ++i) game->explosions[i].active = false;
}

static void spawn_explosion(struct game_t *game, float x, float y)
{
    for (int i = 0; i < EXPLOSION_MAX; ++i) {
        if (!game->explosions[i].active) {
            game->explosions[i] = (explosion_t){.active = true, .x = x, .y = y};
            return;
        }
    }
}

static void start_run(struct game_t *game)
{
    game->state.phase = GAME_PHASE_PLAYING;
    game->state.time_centis = 0;
    game->state.player_x = GAME_WIDTH / 2;
    game->state.player_y = PLAYER_Y_DEFAULT;
    game->vertical_target = PLAYER_Y_DEFAULT;
    game->run_ticks = 0;
    /* Classic opening: the first seconds are almost empty. */
    game->rain_cooldown = 150;   /* rain joins at 5 s */
    game->wall_cooldown = 960;   /* walls join at 32 s */
    game->dart_cooldown = 600;   /* side darts join at 20 s */
    game->spawn_cooldown = 90;   /* first enemy at 3 s */
    game->player_vx = 0;
    clear_field(game);
}

bool game_create(const game_config_t *config, game_handle_t *ret_handle)
{
    if (!ret_handle) return false;
    *ret_handle = NULL;
    if (!config || !config->width || !config->height) return false;
    game_handle_t game = calloc(1, sizeof(*game));
    if (!game) return false;
    game->config = *config;
    game->rng = 0x19451945U;
    game->last_dir = 1;
    init_stars(game);
    game_reset(game);
    *ret_handle = game;
    return true;
}

void game_delete(game_handle_t game)
{
    free(game);
}

void game_reset(game_handle_t game)
{
    if (!game) return;
    const uint16_t rotation = game->state.rotation;
    const uint32_t best = game->state.best_centis;
    game->state = (game_snapshot_t){0};
    game->state.rotation = rotation;
    game->state.best_centis = best;
    game->state.phase = GAME_PHASE_TITLE;
    game->state.player_x = GAME_WIDTH / 2;
    game->state.player_y = PLAYER_Y_DEFAULT;
    game->vertical_target = PLAYER_Y_DEFAULT;
    game->rng = 0x19451945U;
    game->run_ticks = 0;
    game->scroll = 0;
    clear_field(game);
    init_stars(game);
}

void game_set_paused(game_handle_t game, bool paused)
{
    if (game) game->state.paused = paused;
}

static bool pointer_in(const Rectangle zone, int32_t x, int32_t y)
{
    return x >= zone.x && x < zone.x + zone.width &&
           y >= zone.y && y < zone.y + zone.height;
}

void game_set_pointer(game_handle_t game, int32_t x, int32_t y, bool pressed)
{
    if (!game) return;
    /* Pointer events arrive in device coordinates; the game model and its
     * touch zones live in view coordinates. */
    const int rot = game->state.rotation;
    const int c = GAME_WIDTH / 2;
    int vx = x, vy = y;
    switch (rot) {
    case 90:  vx = y; vy = 2 * c - x; break;
    case 180: vx = 2 * c - x; vy = 2 * c - y; break;
    case 270: vx = 2 * c - y; vy = x; break;
    default: break;
    }

    game->state.pointer_x = vx < 0 ? 0 : (vx >= game->config.width ? game->config.width - 1 : vx);
    game->state.pointer_y = vy < 0 ? 0 : (vy >= game->config.height ? game->config.height - 1 : vy);
    game->state.pointer_down = pressed;

    game->state.touch_left = false;
    game->state.touch_right = false;
    if (!pressed) return;

    if (game->state.phase == GAME_PHASE_TITLE ||
        game->state.phase == GAME_PHASE_GAME_OVER ||
        game->state.phase == GAME_PHASE_WIN) {
        start_run(game);
        return;
    }
    if (pointer_in(LEFT_ZONE, vx, vy)) game->state.touch_left = true;
    else if (pointer_in(RIGHT_ZONE, vx, vy)) game->state.touch_right = true;
}

void game_set_action(game_handle_t game, int32_t code, bool pressed)
{
    if (!game) return;
    switch (code) {
    case 0:
        /* Any steering press also starts or restarts a run. */
        if (pressed && game->state.phase != GAME_PHASE_PLAYING) start_run(game);
        game->state.key_left = pressed;
        break;
    case 1:
        if (pressed && game->state.phase != GAME_PHASE_PLAYING) start_run(game);
        game->state.key_right = pressed;
        break;
    case 2: game->key_up = pressed; break;
    case 5: game->key_down = pressed; break;
    case 4:
        if (pressed) {
            game_reset(game);
            if (game->state.phase == GAME_PHASE_TITLE) start_run(game);
        }
        break;
    default: break;
    }
}

void game_set_imu(game_handle_t game, float gx, float gy, float gz)
{
    (void)gz;
    if (!game) return;
    /* Quantize the gravity direction with a dead band around the diagonals so
     * the rotation stays settled while the device is tilted. */
    const float ax = fabsf(gx), ay = fabsf(gy);
    if (ax > ay * 1.25f && ax > 0.45f)
        game->gravity_quadrant = gx > 0 ? 90 : 270;
    else if (ay > ax * 1.25f && ay > 0.45f)
        game->gravity_quadrant = gy > 0 ? 180 : 0;
    update_rotation(game);
}

void game_set_module_edge(game_handle_t game, int32_t edge_angle)
{
    if (!game) return;
    game->module_edge = edge_angle;
    update_rotation(game);
}

void game_set_vertical_target(game_handle_t game, int32_t y)
{
    if (!game) return;
    if (y < 16) y = 16;
    if (y > GAME_HEIGHT - 16) y = GAME_HEIGHT - 16;
    game->vertical_target = y;
    game->camera_vertical = true;
    game->camera_target_tick = game->state.tick;
}

/* ------------------------------------------------------------ bullet hell */

static float difficulty(const struct game_t *game)
{
    /* Classic curve: the first seconds are calm, density and speed ramp
     * gradually — 1.0 at 0 s, 2.0 at 60 s. */
    const float t = game->run_ticks / (float)GAME_TICK_HZ;
    float mult = 1.0f + t / 60.0f;
    if (mult > 2.0f) mult = 2.0f;
    return mult;
}

static void fire_bullet(struct game_t *game, float x, float y, float vx, float vy)
{
    for (int i = 0; i < BULLET_MAX; ++i) {
        bullet_t *b = &game->bullets[i];
        if (b->active) continue;
        *b = (bullet_t){.active = true, .x = x, .y = y, .vx = vx, .vy = vy};
        return;
    }
}

static void enemy_fire_volley(struct game_t *game, enemy_t *enemy)
{
    const float mult = difficulty(game);
    const float dx = game->state.player_x - enemy->x;
    const float dy = game->state.player_y - enemy->y;
    const float len = sqrtf(dx * dx + dy * dy);
    const float speed = 3.1f * mult;
    if (len < 1.0f) {
        fire_bullet(game, enemy->x, enemy->y, 0, speed);
        return;
    }
    const float nx = dx / len, ny = dy / len;
    if (enemy->kind == ENEMY_SCOUT) {
        fire_bullet(game, enemy->x, enemy->y, nx * speed, ny * speed);
    } else if (enemy->kind == ENEMY_STRIKER) {
        fire_bullet(game, enemy->x, enemy->y, nx * speed, ny * speed);
        fire_bullet(game, enemy->x, enemy->y, (nx - 0.35f) * speed, ny * speed);
        fire_bullet(game, enemy->x, enemy->y, (nx + 0.35f) * speed, ny * speed);
    } else {
        /* Tank: a ring of bullets around itself. */
        const int count = 10;
        for (int k = 0; k < count; ++k) {
            const float a = 6.28318f * k / count + enemy->weave_phase;
            fire_bullet(game, enemy->x, enemy->y, cosf(a) * speed * 0.7f,
                        fabsf(sinf(a)) * speed * 0.7f + 0.6f);
        }
    }
}

static void spawn_enemy(struct game_t *game)
{
    enemy_t *e = NULL;
    for (int i = 0; i < ENEMY_MAX; ++i) {
        if (!game->enemies[i].active) { e = &game->enemies[i]; break; }
    }
    if (!e) return;

    const float mult = difficulty(game);
    const uint32_t roll = rng_next(game) % 100U;
    enemy_kind_t kind = ENEMY_SCOUT;
    if (roll >= 80U) kind = ENEMY_TANK;
    else if (roll >= 50U) kind = ENEMY_STRIKER;

    e->active = true;
    e->kind = kind;
    e->base_x = rand_range(game, 40, GAME_WIDTH - 40);
    e->x = e->base_x;
    e->y = -20;
    e->weave_phase = rand_range(game, 0, 6.28f);
    switch (kind) {
    case ENEMY_SCOUT:
        e->vy = rand_range(game, 1.6f, 2.4f) * mult;
        e->fire_interval = (int)(rand_range(game, 70, 120) / mult);
        break;
    case ENEMY_STRIKER:
        e->vy = rand_range(game, 1.2f, 1.8f) * mult;
        e->fire_interval = (int)(rand_range(game, 55, 90) / mult);
        break;
    case ENEMY_TANK:
    default:
        e->vy = rand_range(game, 0.8f, 1.2f) * mult;
        e->fire_interval = (int)(rand_range(game, 40, 70) / mult);
        break;
    }
    if (e->fire_interval < 18) e->fire_interval = 18;
    e->fire_cooldown = e->fire_interval + 60; /* 2 s before its first shot */
}

/* A wall of bullets across the top with one gap; punishes camping. */
static void spawn_wall(struct game_t *game)
{
    const float mult = difficulty(game);
    const float gap_center = rand_range(game, 60, GAME_WIDTH - 60);
    const float gap_half = 52;
    for (float x = 14; x < GAME_WIDTH; x += 26) {
        if (fabsf(x - gap_center) < gap_half) continue;
        fire_bullet(game, x, -10, 0, 2.6f * mult);
    }
}

/* Insert a finished run into the leaderboard; save through the callback. */
static void record_run(struct game_t *game, uint32_t centis)
{
    if (centis == 0) return;
    uint32_t previous[GAME_LEADERBOARD_SIZE];
    for (int i = 0; i < GAME_LEADERBOARD_SIZE; ++i)
        previous[i] = s_leaderboard[i];
    for (int i = 0; i < GAME_LEADERBOARD_SIZE; ++i) {
        if (centis > s_leaderboard[i]) {
            for (int j = GAME_LEADERBOARD_SIZE - 1; j > i; --j)
                s_leaderboard[j] = s_leaderboard[j - 1];
            s_leaderboard[i] = centis;
            break;
        }
    }
    bool changed = false;
    for (int i = 0; i < GAME_LEADERBOARD_SIZE; ++i)
        changed = changed || s_leaderboard[i] != previous[i];
    game->state.best_centis = s_leaderboard[0];
    if (changed && s_save_cb)
        s_save_cb(s_leaderboard, GAME_LEADERBOARD_SIZE, s_save_user);
}

static void end_run(struct game_t *game, float x, float y)
{
    spawn_explosion(game, x, y);
    game->state.phase = GAME_PHASE_GAME_OVER;
    const uint32_t centis = run_centis(game);
    game->state.time_centis = centis;
    record_run(game, centis);
}

static void update_bullets(struct game_t *game)
{
    const Vector2 player = {.x = game->state.player_x, .y = game->state.player_y};

    for (int i = 0; i < BULLET_MAX; ++i) {
        bullet_t *b = &game->bullets[i];
        if (!b->active) continue;
        b->x += b->vx;
        b->y += b->vy;
        if (b->x < -10 || b->x > GAME_WIDTH + 10 || b->y > GAME_HEIGHT + 10) {
            b->active = false;
            continue;
        }
        if (CheckCollisionCircles((Vector2){b->x, b->y}, BULLET_RADIUS,
                                  player, PLAYER_RADIUS)) {
            end_run(game, player.x, player.y);
            return;
        }
    }
}

static void update_enemies(struct game_t *game)
{
    const Vector2 player = {.x = game->state.player_x, .y = game->state.player_y};
    for (int i = 0; i < ENEMY_MAX; ++i) {
        enemy_t *e = &game->enemies[i];
        if (!e->active) continue;
        e->y += e->vy;
        e->weave_phase += 0.05f;
        e->x = e->base_x + sinf(e->weave_phase) * (e->kind == ENEMY_SCOUT ? 46 : 18);
        if (e->y > GAME_HEIGHT + 30) { e->active = false; continue; }

        if (e->y > 10 && e->y < GAME_HEIGHT - 160) {
            if (++e->fire_cooldown >= e->fire_interval) {
                e->fire_cooldown = 0;
                enemy_fire_volley(game, e);
            }
        }

        const float hit_radius = e->kind == ENEMY_TANK ? 18 : 13;
        if (CheckCollisionCircles(player, PLAYER_RADIUS,
                                  (Vector2){e->x, e->y}, hit_radius)) {
            end_run(game, player.x, player.y);
            return;
        }
    }
}

static void update_explosions(struct game_t *game)
{
    for (int i = 0; i < EXPLOSION_MAX; ++i) {
        explosion_t *ex = &game->explosions[i];
        if (!ex->active) continue;
        if (++ex->age > 18) ex->active = false;
    }
}

void game_update(game_handle_t game)
{
    if (!game || game->state.paused) return;
    ++game->state.tick;

    for (int i = 0; i < STAR_MAX; ++i) {
        star_t *s = &game->stars[i];
        s->y += s->speed;
        if (s->y > GAME_HEIGHT) {
            s->y = 0;
            s->x = rand_range(game, 0, GAME_WIDTH);
        }
    }
    game->scroll = (game->scroll + 2) % 48;

    if (game->state.phase != GAME_PHASE_PLAYING) return;

    ++game->run_ticks;
    game->state.time_centis = run_centis(game);

    /* Victory: sixty seconds of dodging. */
    if (game->state.time_centis >= GAME_TARGET_CENTIS) {
        game->state.time_centis = GAME_TARGET_CENTIS;
        game->state.phase = GAME_PHASE_WIN;
        record_run(game, GAME_TARGET_CENTIS);
        return;
    }

    /* Player steering. */
    const bool left = game->state.key_left || game->state.touch_left;
    const bool right = game->state.key_right || game->state.touch_right;
    if (left && !right) {
        game->player_vx = -PLAYER_SPEED;
        game->last_dir = -1;
    } else if (right && !left) {
        game->player_vx = PLAYER_SPEED;
        game->last_dir = 1;
    } else if (left && right) {
        /* Conflict: the two pads are adjacent electrodes, so holding one key
         * can couple a ghost onto the other. Keep the previous course. */
        game->player_vx = PLAYER_SPEED * game->last_dir;
    } else {
        game->player_vx = 0;
    }
    game->state.player_x += (int)game->player_vx;
    if (game->state.player_x < 16) game->state.player_x = 16;
    if (game->state.player_x > GAME_WIDTH - 16) game->state.player_x = GAME_WIDTH - 16;

    /* Vertical: steer toward the target (camera head tracking, or the Host's
     * W/S keys) at a capped speed so dodging stays a skill. */
    if (game->key_up) {
        game->vertical_target = game->state.player_y - VERTICAL_SPEED;
        game->camera_vertical = false;
    } else if (game->key_down) {
        game->vertical_target = game->state.player_y + VERTICAL_SPEED;
        game->camera_vertical = false;
    }
    if (game->camera_vertical &&
        game->state.tick - game->camera_target_tick > GAME_TICK_HZ) {
        game->camera_vertical = false; /* stale camera target ages out */
    }
    if (game->camera_vertical) {
        const int32_t dy = game->vertical_target - game->state.player_y;
        game->state.player_y += dy > VERTICAL_SPEED ? VERTICAL_SPEED :
                                (dy < -VERTICAL_SPEED ? -VERTICAL_SPEED : dy);
    }

    update_bullets(game);
    if (game->state.phase != GAME_PHASE_PLAYING) return;
    update_enemies(game);
    if (game->state.phase != GAME_PHASE_PLAYING) return;
    update_explosions(game);

    /* Escalating rain from the top edge. */
    const float seconds = game->run_ticks / (float)GAME_TICK_HZ;
    if (seconds > 4 && --game->rain_cooldown <= 0) {
        const float mult = difficulty(game);
        game->rain_cooldown = (int)(90 - seconds * 1.5f);
        if (game->rain_cooldown < 12) game->rain_cooldown = 12;
        const float x = rand_range(game, 8, GAME_WIDTH - 8);
        fire_bullet(game, x, -10, rand_range(game, -0.4f, 0.4f), 2.5f * mult);
        if (seconds > 25) {
            const float x2 = rand_range(game, 8, GAME_WIDTH - 8);
            fire_bullet(game, x2, -10, rand_range(game, -0.4f, 0.4f), 2.2f * mult);
        }
    }

    /* Walls with a gap, from 32 s on. */
    if (seconds > 32 && --game->wall_cooldown <= 0) {
        spawn_wall(game);
        game->wall_cooldown = 180;
    }


    int spawn_interval = 90 - (int)(seconds * 1.23f);
    if (spawn_interval < 16) spawn_interval = 16;
    if (--game->spawn_cooldown <= 0) {
        game->spawn_cooldown = spawn_interval;
        spawn_enemy(game);
    }
}

/* ---------------------------------------------------------------- rendering
 * Everything is authored in view coordinates and mapped into device
 * coordinates by an exact 90-degree-step rotation around the canvas center.
 * Rectangles stay axis-aligned under this mapping, so all primitives remain
 * exact; text uses a built-in 5x7 font because the backend text renderer
 * cannot rotate glyphs. */

#define FONT_COLUMNS 5
#define FONT_ROWS 7
#define FONT_ADVANCE 6

typedef struct {
    char ch;
    uint8_t rows[FONT_ROWS];
} glyph_t;

/* Column bits: bit 4 = leftmost column. */
static const glyph_t s_font[] = {
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}},
    {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}},
    {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}},
    {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}},
    {'N', {0x11, 0x19, 0x19, 0x15, 0x13, 0x13, 0x11}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}},
    {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}},
    {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
    {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}},
    {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x1B, 0x11}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}},
    {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'2', {0x0E, 0x11, 0x01, 0x06, 0x08, 0x10, 0x1F}},
    {'3', {0x0E, 0x11, 0x01, 0x06, 0x01, 0x11, 0x0E}},
    {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}},
    {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}},
    {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {0x27, {0x04, 0x04, 0, 0, 0, 0, 0}},
    {'.', {0, 0, 0, 0, 0, 0, 0x04}},
    {'!', {0x04, 0x04, 0x04, 0x04, 0x04, 0, 0x04}},
};

static const glyph_t *font_glyph(char ch)
{
    for (size_t i = 0; i < sizeof(s_font) / sizeof(s_font[0]); ++i)
        if (s_font[i].ch == ch) return &s_font[i];
    return NULL;
}

static Vector2 rot_point(int rot, float x, float y)
{
    const float c = GAME_WIDTH / 2.0f;
    const float dx = x - c, dy = y - c;
    switch (rot) {
    case 90:  return (Vector2){c - dy, c + dx};
    case 180: return (Vector2){c - dx, c - dy};
    case 270: return (Vector2){c + dy, c - dx};
    default:  return (Vector2){x, y};
    }
}

static void rot_rect(int rot, float x, float y, float w, float h,
                     int *ox, int *oy, int *ow, int *oh)
{
    const Vector2 a = rot_point(rot, x, y);
    const Vector2 b = rot_point(rot, x + w, y + h);
    *ox = (int)(a.x < b.x ? a.x : b.x);
    *oy = (int)(a.y < b.y ? a.y : b.y);
    *ow = (int)fabsf(a.x - b.x);
    *oh = (int)fabsf(a.y - b.y);
}

static void rot_fill(int rot, float x, float y, float w, float h, Color color)
{
    int ox, oy, ow, oh;
    rot_rect(rot, x, y, w, h, &ox, &oy, &ow, &oh);
    DrawRectangle(ox, oy, ow, oh, color);
}

static void rot_triangle(int rot, Vector2 a, Vector2 b, Vector2 c, Color color)
{
    DrawTriangle(rot_point(rot, a.x, a.y), rot_point(rot, b.x, b.y),
                 rot_point(rot, c.x, c.y), color);
}

static int text_width(const char *text, int font_size)
{
    const int scale = font_size / 8 < 1 ? 1 : font_size / 8;
    int width = 0;
    for (; *text; ++text) width += FONT_ADVANCE * scale;
    return width > 0 ? width - scale : 0;
}

static void rot_text(int rot, const char *text, float x, float y,
                     int font_size, Color color)
{
    if (!text) return;
    const int scale = font_size / 8 < 1 ? 1 : font_size / 8;
    for (; *text; ++text, x += FONT_ADVANCE * scale) {
        const glyph_t *glyph = font_glyph(*text);
        if (!glyph) continue;
        for (int row = 0; row < FONT_ROWS; ++row) {
            for (int col = 0; col < FONT_COLUMNS; ++col) {
                if (!(glyph->rows[row] & (1U << (4 - col)))) continue;
                rot_fill(rot, x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

static void rot_text_centered(int rot, const char *text, float center_x,
                              float y, int font_size, Color color)
{
    rot_text(rot, text, center_x - text_width(text, font_size) / 2.0f,
             y, font_size, color);
}

/* "12.3" from centiseconds, one decimal. */
static void format_time(char *out, size_t capacity, uint32_t centis)
{
    snprintf(out, capacity, "%" PRIu32 ".%u", centis / 100U,
             (unsigned)((centis % 100U) / 10U));
}

static void draw_player_plane(int rot, float x, float y)
{
    const Color body = SKYBLUE;
    const Color trim = WHITE;
    /* Fuselage */
    rot_triangle(rot, (Vector2){x, y - 22}, (Vector2){x - 9, y + 12},
                 (Vector2){x + 9, y + 12}, body);
    /* Wings */
    rot_triangle(rot, (Vector2){x - 22, y + 14}, (Vector2){x - 4, y - 4},
                 (Vector2){x - 4, y + 14}, body);
    rot_triangle(rot, (Vector2){x + 22, y + 14}, (Vector2){x + 4, y + 14},
                 (Vector2){x + 4, y - 4}, body);
    /* Cockpit */
    const Vector2 cockpit = rot_point(rot, x, y - 4);
    DrawCircleV(cockpit, 3.5f, trim);
    /* Tail */
    rot_fill(rot, x - 8, y + 10, 16, 5, trim);
}

static void draw_enemy_plane(int rot, const enemy_t *e)
{
    const Color body = e->kind == ENEMY_TANK ? MAROON :
                       (e->kind == ENEMY_STRIKER ? ORANGE : RED);
    const float x = e->x, y = e->y;
    rot_triangle(rot, (Vector2){x, y + 18}, (Vector2){x - 8, y - 10},
                 (Vector2){x + 8, y - 10}, body);
    if (e->kind == ENEMY_TANK) {
        rot_triangle(rot, (Vector2){x - 24, y - 10}, (Vector2){x - 4, y + 4},
                     (Vector2){x - 4, y - 10}, body);
        rot_triangle(rot, (Vector2){x + 24, y - 10}, (Vector2){x + 4, y - 10},
                     (Vector2){x + 4, y + 4}, body);
        rot_fill(rot, x - 12, y - 12, 24, 24, body);
    } else {
        rot_triangle(rot, (Vector2){x - 18, y - 8}, (Vector2){x - 3, y + 6},
                     (Vector2){x - 3, y - 8}, body);
        rot_triangle(rot, (Vector2){x + 18, y - 8}, (Vector2){x + 3, y - 8},
                     (Vector2){x + 3, y + 6}, body);
    }
    const Vector2 eye = rot_point(rot, x, y + 2);
    DrawCircleV(eye, 3.0f, YELLOW);
}

static void draw_hud(int rot, const struct game_t *game)
{
    char text[40];
    /* The star of the show: the survival timer, huge and centered. */
    format_time(text, sizeof(text), game->state.time_centis);
    rot_text_centered(rot, text, GAME_WIDTH / 2, 10, 48, WHITE);
    format_time(text, sizeof(text), game->state.best_centis);
    rot_text(rot, "BEST", 12, 16, 16, GRAY);
    rot_text(rot, text, 12, 34, 16, GOLD);
    /* Encouragement in the final stretch. */
    if (game->state.time_centis >= GAME_TARGET_CENTIS - 1000) {
        rot_text_centered(rot, "KEEP GOING!", GAME_WIDTH / 2, 70, 16, YELLOW);
    }
}

static void draw_leaderboard(int rot, float y)
{
    rot_text_centered(rot, "BEST TIMES", GAME_WIDTH / 2, y, 20, GOLD);
    int shown = 0;
    for (int i = 0; i < GAME_LEADERBOARD_SIZE && shown < 3; ++i) {
        if (s_leaderboard[i] == 0) break;
        char text[24];
        format_time(text, sizeof(text), s_leaderboard[i]);
        char row[32];
        snprintf(row, sizeof(row), "%d %s", i + 1, text);
        rot_text_centered(rot, row, GAME_WIDTH / 2, y + 30 + shown * 26, 20,
                          shown == 0 ? WHITE : GRAY);
        ++shown;
    }
}

bool game_render(game_handle_t game)
{
    if (!game) return false;
    BeginDrawing();
    if (!MosaicoFastFrameAvailable()) {
        EndDrawing();
        return false;
    }
    ClearBackground(BLACK);

    const int rot = game->state.rotation;

    /* Scrolling starfield */
    for (int i = 0; i < STAR_MAX; ++i) {
        const star_t *s = &game->stars[i];
        const uint8_t shade = (uint8_t)(90 + s->speed * 40);
        const Vector2 p = rot_point(rot, s->x, s->y);
        DrawPixel((int)p.x, (int)p.y, (Color){shade, shade, shade, 255});
    }

    if (game->state.phase == GAME_PHASE_PLAYING ||
        game->state.phase == GAME_PHASE_GAME_OVER ||
        game->state.phase == GAME_PHASE_WIN) {
        for (int i = 0; i < EXPLOSION_MAX; ++i) {
            const explosion_t *ex = &game->explosions[i];
            if (!ex->active) continue;
            const float radius = 6 + ex->age * 2.2f;
            const Color color = ex->age < 6 ? YELLOW :
                                (ex->age < 12 ? ORANGE : (Color){120, 60, 30, 255});
            const Vector2 p = rot_point(rot, ex->x, ex->y);
            DrawCircleV(p, radius, color);
        }

        for (int i = 0; i < ENEMY_MAX; ++i)
            if (game->enemies[i].active) draw_enemy_plane(rot, &game->enemies[i]);

        for (int i = 0; i < BULLET_MAX; ++i) {
            const bullet_t *b = &game->bullets[i];
            if (b->active) {
                const Vector2 p = rot_point(rot, b->x, b->y);
                DrawCircleV(p, BULLET_RADIUS, WHITE);
                DrawCircleV(p, BULLET_RADIUS - 2, RED);
            }
        }

        if (game->state.phase == GAME_PHASE_PLAYING) {
            draw_player_plane(rot, game->state.player_x, game->state.player_y);
            draw_hud(rot, game);
        }
    }

    if (game->state.phase == GAME_PHASE_TITLE) {
        rot_text_centered(rot, "60'S STRIKER", GAME_WIDTH / 2, 138, 44, GOLD);
        draw_player_plane(rot, GAME_WIDTH / 2, 280);
        if ((game->state.tick / 20) % 2 == 0)
            rot_text_centered(rot, "TAP TO START", GAME_WIDTH / 2, 360, 30, WHITE);
        rot_text_centered(rot, "SURVIVE 60 SECONDS", GAME_WIDTH / 2, 416, 22, RED);
        rot_text_centered(rot, "ARE YOU A MAN?", GAME_WIDTH / 2, 444, 18, GRAY);
    } else if (game->state.phase == GAME_PHASE_GAME_OVER) {
        rot_fill(rot, 60, 120, 360, 250, (Color){0, 0, 0, 200});
        int ox, oy, ow, oh;
        rot_rect(rot, 60, 120, 360, 250, &ox, &oy, &ow, &oh);
        DrawRectangleLines(ox, oy, ow, oh, RED);
        rot_text_centered(rot, "GAME OVER", GAME_WIDTH / 2, 140, 40, RED);
        char text[40];
        format_time(text, sizeof(text), game->state.time_centis);
        char row[64];
        snprintf(row, sizeof(row), "SURVIVED %sS", text);
        rot_text_centered(rot, row, GAME_WIDTH / 2, 200, 24, WHITE);
        draw_leaderboard(rot, 245);
        if ((game->state.tick / 20) % 2 == 0)
            rot_text_centered(rot, "TAP TO RETRY", GAME_WIDTH / 2, 348, 22, WHITE);
    } else if (game->state.phase == GAME_PHASE_WIN) {
        rot_fill(rot, 30, 110, 420, 270, (Color){0, 40, 0, 210});
        int ox, oy, ow, oh;
        rot_rect(rot, 30, 110, 420, 270, &ox, &oy, &ow, &oh);
        DrawRectangleLines(ox, oy, ow, oh, GREEN);
        rot_text_centered(rot, "CONGRATULATIONS", GAME_WIDTH / 2, 130, 32, GREEN);
        rot_text_centered(rot, "YOU ARE A MAN!", GAME_WIDTH / 2, 185, 26, GOLD);
        char text[40];
        format_time(text, sizeof(text), game->state.time_centis);
        char row[64];
        snprintf(row, sizeof(row), "SURVIVED %sS", text);
        rot_text_centered(rot, row, GAME_WIDTH / 2, 235, 24, WHITE);
        draw_leaderboard(rot, 275);
        if ((game->state.tick / 20) % 2 == 0)
            rot_text_centered(rot, "TAP TO RETRY", GAME_WIDTH / 2, 355, 22, WHITE);
    }
    if (game->state.paused && game->state.phase == GAME_PHASE_PLAYING) {
        rot_text_centered(rot, "PAUSED", GAME_WIDTH / 2, 60, 32, YELLOW);
    }

    EndDrawing();
    return true;
}

game_snapshot_t game_read(game_handle_t game)
{
    return game ? game->state : (game_snapshot_t){0};
}
