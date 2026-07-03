# 2026-07-03 Real LCX004 Factory-Reset Join Sniffer Capture

## Artifact

- Capture directory:
  `research/hue-api-diffs/sniffer-capture-20260703-real-headboard-factory-reset-zll-key/`
- Real device:
  - uniqueid: `00:17:88:01:0b:89:54:2f-0b`
  - modelid: `LCX004`
  - productname: `Hue gradient lightstrip`
- Hue result after dimmer-switch reset and re-add:
  - new Hue v1 id: `37`
  - name: `Hue gradient lightstrip 1`
  - certified: `true`
  - streaming: `proxy=true`, `renderer=true`

## Capture Quality

- Sniffer was running before Hue search and before the dimmer-switch reset.
- Capture duration before manual stop was about `229` seconds.
- `serial-sniffer.log` converted to `mac-raw.pcap` with `7931` frames.
- One malformed/interleaved serial line was skipped.
- Important converter fix from this capture: ESP-IDF's direct receive buffer
  includes the two-byte 802.15.4 FCS. `tools/sniffer_log_to_pcap.py` now strips
  those two bytes before writing the IEEE 802.15.4 no-FCS pcap. Keeping them in
  the pcap breaks Zigbee security MIC/decryption checks.

## Decode Result

The corrected pcap shows the fresh join:

- frame `162`: target `00:17:88:01:0b:89:54:2f` sent Association Request.
- frame `172`: parent `00:17:88:01:0c:b9:2f:63` assigned short `0x9dd8`.
- frame `174`: visible APS-security command candidate from short `0x09de` to
  `0x9dd8`, Key Id `0x02`, security source `00:17:88:01:0c:b9:2f:63`.
- frame `176` onward: target traffic decrypts when the actual Hue network key
  is loaded as `HUE_ZIGBEE_NWK_KEY`.

The Hal9k/Wireshark three-key approach was tested against the corrected pcap
using:

- default global trust-center link key (`ZigBeeAlliance09`)
- ZLL master key
- ZLL commissioning key
- reversed variants
- local gitignored trust-center key files

`tshark` did not recover a decoded APS Transport Key from frame `174` with the
public/preconfigured key set. With the actual Hue network key loaded from local
`.env`, `tools/decode_zigbee_pcap.py` now produces:

- encrypted NWK rows: `4266`
- ZCL rows: `634`
- target rows: `159`
- association-response short address: `0x9dd8`

The generated `zigbee-transcript.csv` and `zigbee-decode-summary.md` redact
key-like ZCL values such as Basic attr `0x0054` type `0xf1`.

## Interpretation

The Hal9k article remains the right workflow for devices whose join uses one of
the public/preconfigured transport keys. It worked there for an IKEA Tradfri
join to a Hue bridge. This official Philips LCX004 factory-reset join appears to
be different: the transport-key frame was sent by a Hue router/parent, not
directly by the bridge, and did not decrypt with the public key set.

Next useful comparison work is no longer transport-key recovery. Use the
decrypted real transcript as the baseline for fake-vs-real ZCL parity,
especially Basic manufacturer writes `0x0051/0x0053/0x0054`, FC03 read
responses, FC03 command `0x15/0x16`, and OTA cluster behavior.

This capture independently confirms the FC03 extended-discovery classifier:
frame `537` is command `0x16` advertising attrs `0x0032` type `0x20` access
`0x1c` and `0x2000` type `0x07` access `0x1c`.
