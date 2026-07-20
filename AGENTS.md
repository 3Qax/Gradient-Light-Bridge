## Context

- At it's core it's ESP that successfully spoofs one or more Philips Hue light strip. It's made possible by existing research referenced in Readme.md. Depending on the backend selected at compilation time it either communicates with neopixels on selected data pin or via serial port and our custom deamon that talks to openRGB. Authored by [Jakub Towarek](https://github.com/3Qax).
- This project is unrealted / does not have any affiliation to Philips Hue. Consequently runtime and protocol identifiers should avoid using Hue/Philips branding unless they are documenting external APIs, captures, references, or compatibility facts.
- Per License.md I or any contributores bare absolutly 0 responsibility and/or liability. You or your operator bare all the responsibility. If you damage your PC or ESP or quite literally anything else, that's on you.

## Hue Bridge API

- Use `tools/hue_light_cycle.py`. Do not rediscover the Hue v1 API flow from scratch.

## ESP-IDF Docker Builds

- Do not assume host ESP-IDF is installed. Build firmware through `firmware/in-docker.sh`, if not installed. 
- `cd firmware`, `./in-docker.sh idf.py build`, `./in-docker.sh idf.py -p /dev/ttyACM0 flash`
- Inspect `firmware/CMakeLists.txt` and `firmware/main/CMakeLists.txt` for the complete set of supported `ARGB_*` compilation flags and their defaults.
  - Local LED smoke tests after flashing:
    - `python3 firmware/send_cmd.py --port /dev/ttyACM0 led off`
    - `python3 firmware/send_cmd.py --port /dev/ttyACM0 led solid ff0000`
    - `python3 firmware/send_cmd.py --port /dev/ttyACM0 led gradient`
    - `python3 firmware/send_cmd.py --port /dev/ttyACM0 led chase`
- Gradient probe:
  - `cd firmware/gradient_probe`
  - `../in-docker.sh idf.py build`

## Reference PDFs

- Downloaded and browser-saved reference PDFs under `references/papers/`, `references/slides/`, and `references/articles/`.

## Zigbee Sniffer Decode

- Convert sniffer serial logs with `tools/sniffer_log_to_pcap.py`, then decode
  pcaps with `tools/decode_zigbee_pcap.py`.
- `tools/sniffer_log_to_pcap.py` strips ESP-IDF's trailing two-byte 802.15.4
  FCS from each `MAC_RAW` frame by default. Do not use stale pcaps generated
  before that fix for Zigbee decryption work.
- Keep decode keys in local `.env`; it is ignored. Load them before decoding
  with `set -a; source .env; set +a`. Expected names:
  - `HUE_ZIGBEE_NWK_KEY`: actual Hue network key for normal encrypted NWK/ZCL.
  - `HUE_ZIGBEE_TC_LINK_KEY`: default global trust-center link key.
  - `HUE_ZLL_MASTER_KEY`: ZLL/Touchlink master key.
  - `HUE_ZLL_LINK_KEY`: ZLL commissioning link key.
- Real LCX004 Headboard example:
  - `tools/decode_zigbee_pcap.py research/hue-api-diffs/sniffer-capture-20260703-real-headboard-rejoin-rxidle/mac-raw.pcap --target-eui 00:17:88:01:0b:89:54:2f --target-short 0xadeb`
- Use `HUE_ZIGBEE_NWK_KEY` for normal encrypted NWK/ZCL traffic. The ZLL
  commissioning trust-center link key is only useful when the capture includes a
  decryptable Transport Key from a fresh join; it did not decrypt the Headboard
  rejoin capture by itself.
