# faces/relay — WebRTC P2P via oracle-relay

ESP32-S3 WebRTC PeerConnection using `esp_peer` + oracle-relay signalling.

## Architecture

```
oracle-relay (:9100)  ←── WebSocket (PeerJS protocol) ──→  ESP32-S3
                              SDP offer/answer + ICE                esp_peer
                                                                    ↕
                                                              DataChannel
                                                                    ↕
                                                          Browser / Oracle
```

## How signalling works

1. ESP32 connects to oracle-relay via WebSocket (`ws://<host>:9100/peerjs?id=jc3248&key=oracle-relay`)
2. oracle-relay sends `OPEN` → ESP32 is registered as peer "jc3248"
3. Browser/Oracle connects to same relay as peer "browser-1"
4. Browser creates offer → relay forwards to ESP32
5. ESP32's `on_msg(ESP_PEER_MSG_TYPE_SDP)` fires → forward SDP to `esp_peer_send_msg()`
6. esp_peer creates answer → `on_msg` fires with answer SDP → forward to relay → browser
7. ICE candidates exchanged same way → WebRTC DataChannel established
8. Data flows P2P (relay no longer involved)

## Dependencies

- `esp_peer` component (from espressif/esp-webrtc-solution)
- `esp_websocket_client` (ESP-IDF built-in)
- WiFi connection

## Status

Seed/scaffold — jc3248-oracle implements the full signalling bridge.
