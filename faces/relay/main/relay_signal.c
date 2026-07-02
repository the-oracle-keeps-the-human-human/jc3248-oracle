/*
 * relay_signal.c — WebSocket signalling client for oracle-relay.
 *
 * Speaks PeerJS protocol: OPEN/OFFER/ANSWER/CANDIDATE/HEARTBEAT
 * over WebSocket to oracle-relay server.
 *
 * SCAFFOLD — jc3248-oracle fills in the implementation.
 */
#include "relay_signal.h"
#include "esp_log.h"
#include "esp_websocket_client.h"
#include "cJSON.h"

static const char *TAG = "RELAY_SIG";

typedef struct {
    esp_websocket_client_handle_t ws;
    esp_peer_handle_t peer;
    char peer_id[32];
    char relay_key[32];
    bool connected;
} relay_ctx_t;

/* TODO: jc3248-oracle implements these:
 *
 * 1. ws_event_handler — parse incoming PeerJS messages:
 *    - "OPEN" → relay accepted us, we're registered
 *    - "OFFER" → remote peer wants to connect, extract SDP,
 *                call esp_peer_send_msg(peer, &sdp_msg) to feed to esp_peer
 *    - "ANSWER" → response to our offer, same esp_peer_send_msg flow
 *    - "CANDIDATE" → ICE candidate, same flow
 *    - "HEARTBEAT" → respond with HEARTBEAT
 *
 * 2. relay_signal_send — package esp_peer_msg_t into PeerJS JSON:
 *    ESP_PEER_MSG_TYPE_SDP → {"type":"OFFER"/"ANSWER","src":"jc3248","dst":"browser","payload":{"sdp":{"type":"offer","sdp":"..."}}}
 *    ESP_PEER_MSG_TYPE_CANDIDATE → {"type":"CANDIDATE","src":"jc3248","dst":"browser","payload":{"candidate":{...}}}
 *
 * 3. relay_signal_start — connect WebSocket to:
 *    ws://<relay_url>/peerjs?id=<peer_id>&token=<random>&key=<relay_key>
 */

int relay_signal_start(const relay_signal_cfg_t *cfg, relay_signal_handle_t *handle)
{
    ESP_LOGI(TAG, "TODO: connect to %s as peer '%s'", cfg->relay_url, cfg->peer_id);
    /* jc3248-oracle implements the WebSocket connection here */
    return -1;
}

int relay_signal_send(relay_signal_handle_t handle, const char *dst_peer,
                      esp_peer_msg_t *msg)
{
    ESP_LOGI(TAG, "TODO: forward %s to peer '%s' via relay",
             msg->type == ESP_PEER_MSG_TYPE_SDP ? "SDP" : "ICE", dst_peer);
    /* jc3248-oracle packages msg into PeerJS JSON and sends via WS */
    return -1;
}

int relay_signal_stop(relay_signal_handle_t handle)
{
    ESP_LOGI(TAG, "TODO: disconnect from relay");
    return -1;
}
