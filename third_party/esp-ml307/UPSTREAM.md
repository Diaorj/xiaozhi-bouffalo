Source: https://github.com/78/esp-ml307
v3.7.6, commit bf62ab3daf30965c18cde113e6c93df3cada6363 (satisfies ~3.7.5).
Pinned interface headers, src/web_socket.cc and license are unmodified.
Bouffalo CMake generates C++17 expected-name substitutions and atomic connection
flags; prepare_websocket.py generates the WebSocket compatibility output with
verified Upgrade, SDK random masking, bounded messages and callback-safe teardown.
ESP-IDF continues to use its normal managed dependency.
