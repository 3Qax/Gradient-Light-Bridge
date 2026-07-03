# 2026-07-03 Basic C0/C1 Metadata Cleanup Regression

Goal: continue the opaque Basic cluster `0xC0` / `0xC1` classifier path after
certification and LCX004 product identity were already working but Hue v2 still
did not expose `gradient` or `entertainment`.

Evidence:

- Real LCX004 probe
  `gradient-probe-lcx004-basic-c0-variants-20260703-live/serial-basic-c0-variants.log`
  captured two Basic `0xC0` requests:
  - request `000000000040` -> C1 response length 64
  - request `003500000040` -> C1 response length 43
- Those two C1 responses are not independent records. They are chunks of one
  85-byte metadata payload:
  - first response: offset `0x00`, chunk length `0x35`
  - second response: offset `0x35`, chunk length `0x20`
- The data decodes visibly as product identity strings plus trailing feature
  bytes:
  - `LCX004`
  - `Signify Netherlands B.V.`
  - `Hue gradient lightstrip`
  - tail bytes `48 66 52 0b 08 0b 12 07 08 b0 09 10 03 18 01`

Firmware change attempted:

- Replaced the two hardcoded C1 packet blobs with one
  `HUE_BASIC_CMD_C1_METADATA` payload and a chunked C1 response builder.
- Verified the generated responses are byte-for-byte identical to the two real
  captured C1 packets for requests `000000000040` and `003500000040`.
- Removed the older custom-cluster Basic `0xC0` responder so only the raw ZCL
  handler owns this command. The old fallback always returned the first chunk,
  which would be wrong for any nonzero-offset request that reached that path.

External cross-check:

- Koenkk zigbee-herdsman-converters `philips.ts` at revision
  `7e7e28affbbd423bd5c6b1a20372c27cfc1066cc` confirms Hue gradient control is
  sent through `manuSpecificPhilips2` / FC03 `multiColor`, and clients bind FC03
  once they already know a device is gradient-capable. It does not document Hue
  bridge product-classifier bits or the Basic `0xC0` metadata format.
  Source:
  https://raw.githubusercontent.com/Koenkk/zigbee-herdsman-converters/7e7e28affbbd423bd5c6b1a20372c27cfc1066cc/src/lib/philips.ts

Build:

- `cd firmware && sg docker -c './in-docker.sh idf.py build'` passed.

Flashed test result:

- Local scratch capture `discovery-capture-20260703-basic-c0-c1-generated/`
  flashed the generated-C1 firmware, deleted fake v1 light `62`, and started
  Hue search. The bridge read only the first standard Basic batch
  `[0x0005, 0x0004, 0x4000, 0x0007]` repeatedly, then sent a leave/reset. It
  never reached the manufacturer-specific Basic `0xC0` reads, and no fake Hue
  light was created afterward.
- Local scratch capture `discovery-capture-20260703-basic-c0-c1-generated-rerun/`
  repeated the search without an existing fake record. The bridge reached
  endpoint 11 Simple Descriptor but again did not progress to the known
  successful ZCL classifier sweep, did not reach `0xC0`, and did not create a
  fake Hue light.

Conclusion:

- The cleanup was discarded from firmware. Even though its generated C1 payloads
  matched the two captured real C1 responses, the flashed build regressed early
  discovery before C0/C1 was exercised.
- Do not reapply the generated C1 / single-owner cleanup as written. Keep the
  last-known-good committed C0/C1 implementation until a safer isolated test can
  explain why the bridge stopped before the manufacturer-specific classifier
  sweep.
