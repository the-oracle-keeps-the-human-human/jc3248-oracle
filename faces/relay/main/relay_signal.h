/*
 * relay_signal.h — WebSocket signalling client for oracle-relay (PeerJS protocol).
 *
 * Bridges esp_peer's on_msg (SDP/ICE) ↔ oracle-relay WebSocket.
 * PeerJS message format: {"type":"OFFER/ANSWER/CANDIDATE","src":"...","dst":"...","payload":{...}}
 */
#pragma once

#include "esp_peer.h"

typedef struct {
    const char *relay_url;
    const char *peer_id;
    const char *relay_key;
    esp_peer_handle_t peer;
} relay_signal_cfg_t;

typedef void *relay_signal_handle_t;

/* Connect to oracle-relay WebSocket and register as peer_id */
int relay_signal_start(const relay_signal_cfg_t *cfg, relay_signal_handle_t *handle);

/* Forward local SDP/ICE to relay (called from esp_peer on_msg callback) */
int relay_signal_send(relay_signal_handle_t handle, const char *dst_peer,
                      esp_peer_msg_t *msg);

/* Stop and disconnect */
int relay_signal_stop(relay_signal_handle_t handle);
