## Hue Bridge API

- Use `tools/hue_light_cycle.py` for Hue bridge inspection, stale-light deletion,
  rediscovery, and state testing. Do not rediscover the Hue v1 API flow from
  scratch. Main commands:
  - `tools/hue_light_cycle.py config`
  - `tools/hue_light_cycle.py list`
  - `tools/hue_light_cycle.py get <light-id>`
  - `tools/hue_light_cycle.py set-state <light-id> '{"on":true,"bri":254}'`
  - `tools/hue_light_cycle.py delete --id <light-id>`
  - `tools/hue_light_cycle.py search --poll-seconds 90`
  - `tools/hue_light_cycle.py cycle --modelid LCX004 --manufacturer 'Signify Netherlands B.V.' --uncertified-only`

## ESP-IDF Docker Builds

- Build firmware through `firmware/in-docker.sh`; do not assume host ESP-IDF is
  installed.
- Main firmware:
  - `cd firmware`
  - `./in-docker.sh idf.py build`
  - If Docker group switching is needed: `sg docker -c './in-docker.sh idf.py build'`
- Flash on Linux:
  - `cd firmware`
  - `PORT=/dev/ttyACM0 ./in-docker.sh idf.py -p /dev/ttyACM0 flash`
- Gradient probe:
  - `cd firmware/gradient_probe`
  - `../in-docker.sh idf.py build`
- Override ESP-IDF image version with `IDFVER`, e.g. `IDFVER=v5.3.2 ./in-docker.sh idf.py build`.

## Reference PDFs

- Downloaded and browser-saved reference PDFs under `references/papers/`,
  `references/slides/`, and `references/articles/`.
- Keep Hue trust-center keys and Hue API credentials out of tracked files.

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
