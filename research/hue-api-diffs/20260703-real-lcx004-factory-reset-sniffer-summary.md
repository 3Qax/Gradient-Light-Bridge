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

## Scenes Ground Truth

The real LCX004 does not report stored scene IDs in Get Scene Membership, even
after the bridge sends scene definitions:

- frame `1075`: bridge sends standard Scenes cluster server command `0x06`
  (Get Scene Membership) for group `0xe9d5`.
- frame `1077`: LCX004 replies with Get Scene Membership Response:
  `status=0x00`, `capacity=50`, `group_id=0xe9d5`, `scene_count=0`.

So firmware should not invent a non-empty scene-membership list. The useful
parity point is the manufacturer-specific scene-store acknowledgement:

- frame `2090`: bridge sends manufacturer-specific Scenes cluster command
  `0x02`, manufacturer `0x100b`, payload
  `d5e9821b0001fe909f085c0400`.
  - `d5e9` is group `0xe9d5`.
  - `82` is scene id `0x82`.
  - the remaining bytes are the embedded FC03 state payload.
- frame `2092`: LCX004 replies on the same manufacturer command with payload
  `00d5e982`: success status, same group id, same scene id.

Implication for the ESP firmware: cache the embedded FC03 payload for fast
Recall Scene handling, keep membership count at zero with capacity `50`, and
explicitly respond to manufacturer-specific Scenes command `0x02` with
`00 <group_lo> <group_hi> <scene_id>`.

The capture also shows manufacturer-specific Scenes command `0x00` as a
compact scene/update form. Example frame `1656` is a bridge groupcast with
payload `d5e90e0c1350000000dbfd59866c6387cc6c49bc765c0a82ba`.
The first three bytes are still `group_id=0xe9d5`, `scene_id=0x0e`; the body
starts with `0c`, followed by a normal FC03 gradient color block
(`13 50 ...`) and one trailing compact byte. This command has default response
disabled, so the useful behavior is to translate the compact gradient body into
the normal FC03 renderer/cache format and apply it, not to send an explicit
response.

Later labelled app-action logs while editing dynamic-scene speed show that the
trailing compact byte matches the last speed-only FC03 `0x0090` update. For
example, speed update `90000400d0` is followed by compact scene payload
`d5e92a0c1350000000dbfd59866c6387cc6c49bc765c0a82d0` when saving the scene.
That compact payload should therefore be applied live as FC03 flags `0x01d0`
(`fade + gradient colors + effect speed + gradient params`). It should not be
persisted in that dynamic form for Recall Scene: labelled app logs show Stop and
static scene selection use standard Scenes Recall `cmd=0x05`, so the recall
cache must stay static and omit `FC03_FLAG_EFFECT_SPEED`.
