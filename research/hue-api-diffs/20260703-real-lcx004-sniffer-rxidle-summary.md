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

## FC03 Gradient Classifier Frames

The gradient-specific evidence in this passive capture supersedes earlier
active-probe assumptions:

- frames `499/503`: the bridge's large manufacturer-specific FC03 read returns
  success for `0x0001/0x0002/0x0010/0x0011/0x0012/0x0013`, unsupported for
  `0x0030/0x0037`, success for `0x0038=10`, and status `0x89` for
  `0x0033/0x0032`.
- frames `507/511`: a focused FC03 read of `0x0032/0x0033` immediately returns
  success, type `0x20`, value `0`.
- frames `595/599`: FC03 Discover Attributes Extended (`0x15`) start `0x0032`,
  max `3`, is answered by command `0x16` advertising attrs `0x0032` type
  `0x20` access `0x1c` and `0x2000` type `0x07` access `0x1c`.

## Fake Parity Result

`research/hue-api-diffs/discovery-capture-20260703-fc03-read-and-extdisc-parity/`
is the successful fake-light validation against this passive real-device
baseline. The bridge recreated the fake as v1 id `59`, `modelid=LCX004`,
`productname=Hue gradient lightstrip`, and `capabilities.certified=true`.

The decisive firmware behavior was request-shape-specific FC03 parity:

- for the large frame-499 discovery read, return the real short `0x0002` octet
  string and status `0x89` for tail attrs `0x0033/0x0032`;
- for the immediate focused read of `0x0032/0x0033`, still return success,
  type `0x20`, value `0`;
- for FC03 Discover Attributes Extended start `0x0032`, advertise only
  `0x0032` type `0x20` access `0x1c` and `0x2000` type `0x07` access `0x1c`.

The v2 light for `/lights/59` now exposes `gradient`, `effects`,
`effects_v2`, and `content_configuration`. The gradient object reports
`pixel_count=10`, `points_capable=5`, and the expected palette modes. v1
`capabilities.streaming` remains `proxy=false`, `renderer=false`, so Hue
Entertainment streaming appears to be a separate classifier from gradient UI
exposure.
