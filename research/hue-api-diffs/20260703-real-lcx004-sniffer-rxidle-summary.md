# 2026-07-03 Real LCX004 Sniffer Capture With rx_when_idle

## Artifact

- Capture directory:
  `research/hue-api-diffs/sniffer-capture-20260703-real-headboard-rejoin-rxidle/`
- Real device before deletion:
  - Hue v1 id: `63`
  - uniqueid: `00:17:88:01:0b:89:54:2f-0b`
  - modelid: `LCX004`
  - productname: `Hue gradient lightstrip`
  - certified: `true`
  - streaming: `proxy=true`, `renderer=true`
- Real device after rejoin:
  - Hue v1 id: `25`
  - name: `Hue gradient lightstrip 1`
  - uniqueid: `00:17:88:01:0b:89:54:2f-0b`
  - modelid: `LCX004`
  - certified: `true`
  - streaming: `proxy=true`, `renderer=true`

## Capture Quality

- Sniffer firmware was the fixed direct IEEE 802.15.4 sniffer with
  `esp_ieee802154_set_rx_when_idle(true)`.
- Bridge channel was `25`.
- Capture duration was `306` seconds.
- Raw serial log contains `8467` `MAC_RAW` marker lines.
- `tools/sniffer_log_to_pcap.py` produced `mac-raw.pcap` with `8466` frames.
- Exactly one malformed/interleaved serial line was skipped by the parser.
- `tools/summarize_sniffer_log.py` produced `mac-summary.csv` and
  `mac-summary.md` with `8466` decoded MAC summaries.

## Files

- `serial-sniffer.log`: raw ESP32-C6 sniffer serial stream.
- `mac-raw.pcap`: IEEE 802.15.4 no-FCS pcap for Wireshark.
- `pcap-summary.md`: frame count, skipped-line count, channel, timestamp range.
- `mac-summary.csv`: decoded 802.15.4 MAC header summaries.
- `mac-summary.md`: frame-type counts, top addresses, visible marker hits.
- `zigbee-transcript.csv`: tshark-derived Zigbee/MAC transcript with key
  material redacted.
- `zigbee-decode-summary.md`: decode counts and target join summary.
- `before-v1-lights.json`: Hue v1 snapshot before deleting the real strip.
- `after-v1-new-lights.json`: Hue v1 `/lights/new` result after search.
- `after-v1-lights.json`: Hue v1 snapshot after the real strip rejoined.

## Interpretation

This is the first usable passive real LCX004 rejoin capture. It should be used
as the real-device baseline for bridge discovery timing, MAC addressing, and
visible manufacturer/OUI markers.

The pcap was decoded with tshark via `tools/decode_zigbee_pcap.py`. The ZLL
commissioning trust-center link key and byte-reversed form did not decrypt this
rejoin capture by themselves; this was an already-known strip, so no useful
fresh network-key transport was recovered from the join.

With the actual Hue network key loaded from local `.env` as
`HUE_ZIGBEE_NWK_KEY`, the same pcap now decodes:

- encrypted NWK rows: `4674`
- ZCL rows: `1017`
- target rows: `295`
- association-response short address: `0xadeb`

The useful decoded evidence is the MAC join:

- frame `96`: target `00:17:88:01:0b:89:54:2f` sent an Association Request.
- frame `102`: the parent assigned short address `0xadeb`.

This was a rejoin of an already-known real strip, so the bridge/device appear to
have reused the existing network key. The ZLL commissioning trust-center link
key is useful for decrypting a fresh Transport Key command when one is present,
but it is not the network key and does not decrypt ordinary post-join NWK/ZCL
traffic by itself.

The decrypted ZCL transcript should be used as the real-device baseline. The
generated artifacts redact key-like ZCL values such as Basic attr `0x0054` type
`0xf1`.
