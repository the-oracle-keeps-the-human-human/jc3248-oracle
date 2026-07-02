/*
 * faces/relay — WebRTC P2P via oracle-relay signalling.
 *
 * SCAFFOLD: shows how esp_peer + relay_signal fit together.
 * jc3248-oracle implements relay_signal.c to complete the bridge.
 */
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_peer.h"
#include "esp_peer_default.h"
#include "relay_signal.h"
#include "display.h"
#include "hud_gfx.h"

static const char *TAG = "RELAY";
static relay_signal_handle_t signal_handle;

/* esp_peer callback: forward SDP/ICE to oracle-relay */
static int on_msg(esp_peer_msg_t *msg, void *ctx)
{
    if (msg->type == ESP_PEER_MSG_TYPE_SDP ||
        msg->type == ESP_PEER_MSG_TYPE_CANDIDATE) {
        relay_signal_send(signal_handle, "browser", msg);
    }
    return 0;
}

/* esp_peer callback: connection state change */
static int on_state(esp_peer_state_t state, void *ctx)
{
    ESP_LOGI(TAG, "peer state: %d", state);
    /* TODO: update display — show connected/disconnected */
    return 0;
}

/* esp_peer callback: data received via DataChannel */
static int on_data(esp_peer_data_frame_t *frame, void *ctx)
{
    ESP_LOGI(TAG, "data received: %d bytes on stream %d",
             frame->size, frame->stream_id);
    /* TODO: display received text on screen */
    return 0;
}

void app_main(void)
{
    ESP_LOGI(TAG, "oracle-relay WebRTC face");

    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }

    /* TODO: WiFi init (use Improv or hardcoded for dev) */

    /* Display */
    display_init();
    display_set_backlight(200);
    uint16_t *fb = display_framebuffer();
    gfx_fill_rect(fb, DISP_W, DISP_H, 0, 0, DISP_W, DISP_H, GFX_BLACK);
    gfx_text(fb, DISP_W, DISP_H, 16, 20, "oracle-relay", 3, GFX_YELLOW, GFX_TRANSPARENT);
    gfx_text(fb, DISP_W, DISP_H, 16, 56, "WebRTC P2P", 2, GFX_WHITE, GFX_TRANSPARENT);
    gfx_text(fb, DISP_W, DISP_H, 16, 90, "connecting...", 1, GFX_GREY, GFX_TRANSPARENT);
    display_flush();

    /* Connect signalling to oracle-relay */
    relay_signal_cfg_t sig_cfg = {
        .relay_url = "ws://white.local:9100",
        .peer_id = "jc3248",
        .relay_key = "oracle-relay",
    };

    /* Open esp_peer with data channel enabled */
    esp_peer_default_cfg_t peer_defaults = {
        .agent_recv_timeout = 100,
        .data_ch_cfg = {
            .recv_cache_size = 4096,
            .send_cache_size = 4096,
        },
    };
    esp_peer_cfg_t cfg = {
        .role = ESP_PEER_ROLE_CONTROLLED,
        .enable_data_channel = true,
        .on_state = on_state,
        .on_msg = on_msg,
        .on_data = on_data,
        .extra_cfg = &peer_defaults,
        .extra_size = sizeof(peer_defaults),
    };

    esp_peer_handle_t peer;
    int rc = esp_peer_open(&cfg, esp_peer_get_default_impl(), &peer);
    ESP_LOGI(TAG, "esp_peer_open: %d", rc);

    sig_cfg.peer = peer;
    relay_signal_start(&sig_cfg, &signal_handle);

    /* Main loop */
    while (1) {
        esp_peer_main_loop(peer);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
