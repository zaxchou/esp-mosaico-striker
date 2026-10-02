// SPDX-License-Identifier: Apache-2.0
#define GSP_BUNDLE_ENABLE_RAW_IDS 1
#include "bundle_gsp.h"
#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "game.h"
#include "game_config.h"
#include "mosaico_game_app.h"
#include "mosaico_module_interact.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "raylib_screen_mirror.h"

static const char *TAG = GAME_NAME;

/* Owned by app_main for the application lifetime. Engine callbacks execute on
 * that same task; touch samples reach it through the engine event queue. */
static game_handle_t s_game;

/* Device event values used on MOSAICO_DEVICE_EVENT_BUTTON:
 * 0/1 are the module's left/right keys; 2/3 report on which slot edge the
 * interaction module attached; 4 reports detach. */
enum {
    MODULE_KEY_LEFT = 0,
    MODULE_KEY_RIGHT = 1,
    MODULE_ATTACHED_LEFT_SLOT = 2,
    MODULE_ATTACHED_RIGHT_SLOT = 3,
    MODULE_DETACHED = 4,
};

static void post_module_event(int32_t value, bool pressed)
{
    mosaico_device_event_t event = {
        .type = MOSAICO_DEVICE_EVENT_BUTTON,
        .value = value,
        .pressed = pressed,
    };
    (void)MosaicoGamePostDeviceEvent(&event);
}

static void on_event(const mosaico_device_event_t *event)
{
    if (!event) return;
    switch (event->type) {
    case MOSAICO_DEVICE_EVENT_POINTER:
        game_set_pointer(s_game, event->x, event->y, event->pressed);
        break;
    case MOSAICO_DEVICE_EVENT_IMU:
        /* IMU arrives as milli-g; the model works in g. */
        game_set_imu(s_game, event->x / 1000.0f, event->y / 1000.0f,
                     event->value / 1000.0f);
        break;
    case MOSAICO_DEVICE_EVENT_BUTTON:
        switch (event->value) {
        case MODULE_KEY_LEFT:
            ESP_LOGI(TAG, "module key left=%d", event->pressed);
            game_set_action(s_game, 0, event->pressed);
            break;
        case MODULE_KEY_RIGHT:
            ESP_LOGI(TAG, "module key right=%d", event->pressed);
            game_set_action(s_game, 1, event->pressed);
            break;
        case MODULE_ATTACHED_LEFT_SLOT:
            game_set_module_edge(s_game, GAME_MODULE_LEFT_EDGE);
            break;
        case MODULE_ATTACHED_RIGHT_SLOT:
            game_set_module_edge(s_game, GAME_MODULE_RIGHT_EDGE);
            break;
        case MODULE_DETACHED:
        default:
            game_set_module_edge(s_game, GAME_MODULE_NONE);
            break;
        }
        break;
    default:
        break;
    }
}

static void on_update(void)
{
    game_update(s_game);
}

static void on_render(void)
{
    (void)game_render(s_game);
}

/* Discover the interaction module, then stream its keys into the game's
 * event queue. Detach is inferred from repeated input failures; discovery
 * retries until the module is plugged back in.
 *
 * The charged-pad read in the interact component is solid, so release is
 * accepted from a single sample (snappy stop); a press still needs two
 * consecutive samples to reject glitches. */
#define KEY_POLL_MS 20

typedef struct {
    bool stable;
    bool last_raw;
} key_state_t;

/* Returns true when the stable level changes; updates *level with it. */
static bool debounce_key(key_state_t *deb, bool raw, bool *level)
{
    bool accept;
    if (raw != deb->stable) {
        accept = !raw || raw == deb->last_raw; /* release: now; press: 2x */
    } else {
        accept = false;
    }
    deb->last_raw = raw;
    if (accept) {
        deb->stable = raw;
        *level = raw;
        return true;
    }
    return false;
}

static void module_task(void *arg)
{
    (void)arg;
    mosaico_interact_handle_t interact = NULL;
    key_state_t left = {false, false};
    key_state_t right = {false, false};
    bool left_level = false, right_level = false;
    int failures = 0;
    for (;;) {
        mosaico_interact_config_t config = MOSAICO_INTERACT_DEFAULT_CONFIG();
        /* GPIO charge-read mode: the key pads have no usable pull-up, so the
         * component charges each pad and reads the held level (verified best
         * playability after A/B against touch mode on this hardware). */
        config.button_mode = MOSAICO_INTERACT_BUTTON_MODE_GPIO;
        if (mosaico_interact_open(&config, &interact) != ESP_OK || !interact) {
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        mosaico_interact_info_t info;
        if (mosaico_interact_get_info(interact, &info) == ESP_OK) {
            post_module_event(info.slot == MOSAICO_MODULE_MGR_SLOT_LEFT
                                  ? MODULE_ATTACHED_LEFT_SLOT
                                  : MODULE_ATTACHED_RIGHT_SLOT,
                              true);
            ESP_LOGI(TAG, "interaction module attached on %s slot",
                     info.slot == MOSAICO_MODULE_MGR_SLOT_LEFT ? "left" : "right");
        }
        left = (key_state_t){false, false};
        right = (key_state_t){false, false};
        left_level = right_level = false;
        failures = 0;
        while (true) {
            mosaico_interact_inputs_t inputs;
            if (mosaico_interact_read_inputs(interact, &inputs) == ESP_OK) {
                failures = 0;
                if (debounce_key(&left, inputs.left_pressed, &left_level)) {
                    ESP_LOGI(TAG, "module key left=%d", left_level);
                    post_module_event(MODULE_KEY_LEFT, left_level);
                }
                if (debounce_key(&right, inputs.right_pressed, &right_level)) {
                    ESP_LOGI(TAG, "module key right=%d", right_level);
                    post_module_event(MODULE_KEY_RIGHT, right_level);
                }
            } else if (++failures >= 8) {
                ESP_LOGW(TAG, "interaction module lost");
                post_module_event(MODULE_DETACHED, true);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(KEY_POLL_MS));
        }
        mosaico_interact_close(interact);
        interact = NULL;
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static const mosaico_game_app_config_t s_config = {
    .tag = GAME_NAME,
    .window_title = GAME_NAME,
    .canvas_bind = GSP_GAME_BIND_GAME_CANVAS,
    .touch_points = 1,
    .enable_imu = true,
    .imu_sample_ms = 20,
    .target_fps = GAME_TICK_HZ,
    .gsp_bundle = gsp_bundle_config,
    .register_mirror = raylib_screen_mirror_register,
    .on_event = on_event,
    .on_update = on_update,
    .on_render = on_render,
};

/* Survival-time leaderboard, persisted in NVS. The game calls the save
 * callback whenever a run enters the board; the table is seeded once at
 * boot. */
#define LB_NAMESPACE "stk45"
#define LB_KEY "lb"

static void leaderboard_save_cb(const uint32_t *times, int count, void *user)
{
    (void)user;
    nvs_handle_t handle;
    if (nvs_open(LB_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) return;
    if (nvs_set_blob(handle, LB_KEY, times,
                     sizeof(uint32_t) * (size_t)count) == ESP_OK)
        nvs_commit(handle);
    nvs_close(handle);
}

static void leaderboard_load(void)
{
    uint32_t times[GAME_LEADERBOARD_SIZE] = {0};
    size_t length = sizeof(times);
    nvs_handle_t handle;
    if (nvs_open(LB_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) return;
    if (nvs_get_blob(handle, LB_KEY, times, &length) == ESP_OK)
        game_set_leaderboard(times, GAME_LEADERBOARD_SIZE);
    nvs_close(handle);
}

void app_main(void)
{
    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    }
    leaderboard_load();
    game_set_leaderboard_save_cb(leaderboard_save_cb, NULL);

    const game_config_t config = {GAME_WIDTH, GAME_HEIGHT};
    ESP_ERROR_CHECK(game_create(&config, &s_game) ? ESP_OK : ESP_ERR_NO_MEM);
    if (xTaskCreate(module_task, "module_keys", 4096, NULL, 4, NULL) != pdPASS)
        ESP_LOGW(TAG, "could not start module key task; touch only");
    /* The engine starts iris_ota_support_start() and marks the image healthy
     * after presenting its first frame; the OTA writer remains in Recovery. */
    esp_err_t result = mosaico_game_app_run(&s_config);
    game_delete(s_game);
    s_game = NULL;
    ESP_ERROR_CHECK(result);
}
