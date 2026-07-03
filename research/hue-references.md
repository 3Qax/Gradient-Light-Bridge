# Philips Hue Reverse-Engineering References

These notes capture the public references behind the Hue joining and gradient
emulation work in this repo. Do not copy the Hue trust-center link key into
tracked files; keep it only in local `trust_center_key.h` files.

## Joining a Hue Bridge

- Colin O'Flynn, "Getting Root on Philips Hue Bridge 2.0"
  https://colinoflynn.com/2016/07/getting-root-on-philips-hue-bridge-2-0/
  - Useful bridge 2.0 hardware context and UART access notes.
  - Confirms the square bridge is a normal embedded Linux target, but this
    project does not require bridge rooting.

- PeeVeeOne, "Breakout breakthrough"
  https://peeveeone.com/2016/11/breakout-breakthrough/
  - Important distinction: the ZLL master key is for Touchlink, while Hue
    bridge light pairing uses classical commissioning with the Hue/ZLL trust
    center link key.
  - This is the historical source for why the ESP32 firmware must configure a
    Hue-compatible trust-center link key before network steering.

- PeeVeeOne, "Custom firmware Hue lights"
  https://peeveeone.com/2016/11/custom-firmware-hue-lights/
  - Shows a custom Zigbee Light Link endpoint joining Hue as a dimmable or color
    light once flashed with firmware that supports the expected commissioning
    path.
  - Practical behavior matches this project: start "Add light" in the Hue app,
    reset/power-cycle the device, and let the bridge discover it.

## Hue Gradient Emulation

- Colin O'Flynn, "Philips Hue - R.E. Whitepaper from Black Hat 2016"
  https://colinoflynn.com/2016/08/philips-hue-r-e-whitepaper-from-black-hat-2016/
  - Broad background on Hue reverse engineering, update encryption, and Zigbee
    Light Link security assumptions.
  - Useful as security context, not as an implementation recipe for the ESP32
    firmware.

- Hue Gradient command generator
  https://kjagiello.github.io/hue-gradient-command-wizard/
  - Interactive helper for generating Hue gradient commands.
  - Use this as a sanity check for command payload shapes while comparing
    observed `0xFC03` gradient traffic from real Hue gradient products.

## Hue Bridge Product Classification

- The Hue v2 API `hardware_platform_type` field (e.g. `100b-118`) is assembled
  from the Zigbee OTA Upgrade cluster: `manufacturer_code` (`0x100B` for Signify)
  and `image_type` (`0x0118` for LCX004). This was confirmed by comparing real
  LCX004 bridge records (`hardware_platform_type: "100b-118"`) with other Signify
  devices (`100b-112` for LCA001, `100b-11f` for 1743530P7, etc.).
- When the bridge does not recognize a device as certified, the v2 device record
  omits `hardware_platform_type` entirely and the v1 record shows
  `certified: false` and `productname: "Extended color light"`.
- A fake LCX004 that spoofs the Signify OUI and model identifier but does not
  expose the correct OTA metadata is discovered as a generic extended color light,
  even when the FC01/FC03/FC04 Signify clusters are present.
- Confirmed local captures:
  - `research/hue-api-diffs/discovery-capture-20260703-102956-zcl-logging/`
    shows the fake advertising endpoint 11 as HA extended color light with
    server clusters `0x0000,0x0003,0x0004,0x0005,0x0006,0x0008,0x0300,0xFC03,
    0xFC01,0xFC04` and OTA client cluster `0x0019`. The bridge sent FC01 command
    `0x03` with manufacturer `0x100B`, then still classified the device as
    uncertified `Extended color light`.
  - `research/gradient-probe-capture-20260703-1036-real-baseline/` is an active
    probe of real gradient devices, not a passive bridge transcript. It shows
    real gradient endpoints advertising server cluster `0x1000` in addition to
    the normal color/FC clusters and OTA client `0x0019`. It also observed FC01
    reads returning `0x0000=0x0F` on one matched endpoint and
    `0x0000=0x0B, 0x0001=0x00, 0x0002=0x0A, 0x0003=0x04` on another. Treat
    this FC01 data as untrusted until reproduced from a fresh passive real
    LCX004 bridge commissioning transcript.
  - `research/hue-api-diffs/discovery-capture-20260703-103550-touchlink-fc01attrs/`
    shows the fake updated to advertise `0x1000` and FC01 attrs `0x0002/0x0003`.
    The bridge still created v1 light 62 as uncertified `Extended color light`;
    v2 still omitted `hardware_platform_type` and `gradient`.
  - `research/hue-api-diffs/discovery-capture-20260703-105427-long-passive/`
    repeated the post-fix fake join with a 180 second passive serial capture.
    The bridge sent FC01 command `0x03` to the fake, but no FC01 read-attribute
    requests were observed. The bridge still created v1 light 63 as uncertified
    `Extended color light`; v2 still omitted `hardware_platform_type` and
    `gradient`.
  - `research/hue-api-diffs/discovery-capture-20260703-110225-endpoint242/`
    tested adding the real LCX004 Green Power endpoint shape as a normal
    client-only endpoint 242: profile `0xA1E0`, device `0x0061`, client cluster
    `0x0021`, no server clusters. The descriptor printed correctly, but the
    device crashed in the Zigbee stack while handling the bridge's ZDO Active
    Endpoint request. The decoded backtrace landed in `zdo_active_ep_res`, so
    this registration path is not usable as-is.
  - `research/hue-api-diffs/discovery-capture-20260703-110430-endpoint242-gateway/`
    repeated endpoint 242 using `esp_zb_ep_list_add_gateway_ep()`. This avoided
    the crash and printed both endpoint 11 and endpoint 242 descriptors, but the
    Hue bridge did not create any new v1 light during the 180 second capture and
    no uncertified fake appeared afterward. Endpoint 242 is therefore not a
    simple missing-descriptor fix; the ESP-Zigbee/ZDO representation still
    differs enough to break Hue discovery.
  - `research/hue-api-diffs/discovery-capture-20260703-native-gppb-endpoint242/`
    tested endpoint 242 using ZBOSS's native Green Power Proxy Basic descriptor
    (`profile=0xA1E0`, `device=0x0061`, client cluster `0x0021`) by
    re-registering the AF device context with endpoint 11 plus endpoint 242. The
    firmware built and printed the expected descriptors, but ZBOSS asserted at
    startup in `zcl/zcl_common.c:103` before network steering. The capture was
    stopped after preserving the serial crash log, so this directory has
    before-snapshots and `serial-discovery.log`, but no after-snapshots.
  - `research/hue-api-diffs/discovery-capture-20260703-restore-no-endpoint242/`
    flashed the firmware back with endpoint 242 disabled and erased the ESP32-C6
    flash first. The device joined the Hue network with only endpoint 11
    advertised and no crash occurred, but this 120 second bridge search did not
    create a new v1 light. No v1/v2 bridge record for the spoofed EUI
    `00:17:88:01:0b:ff:fe:05` was present before or after the run. Treat this
    as a cleanup/restore capture, not proof that endpoint 11 stopped working.
  - `research/hue-api-diffs/discovery-capture-20260703-113156-zdo-override/`
    bypassed ESP-Zigbee's endpoint table for early ZDO discovery. The fake
    answered the Hue bridge's Active Endpoint request with `[11,242]`, then
    answered Simple Descriptor for endpoint 11 with a 32-byte LCX004 descriptor
    and endpoint 242 with a 10-byte Green Power descriptor. The bridge created
    v1 light 57, but still reported `modelid=LCX004`,
    `productname=Extended color light`, and `capabilities.certified=false`. The
    v2 device record also had `product_data.certified=false`, generic
    `product_name=Extended color light`, `metadata.archetype=classic_bulb`, and
    the v2 light record had no `gradient` object. This rules out early ZDO
    endpoint 11/242 descriptor parity as sufficient by itself.
  - `research/hue-api-diffs/discovery-capture-20260703-114011-zcl-raw-logger/`
    kept the ZDO override and added a passive raw ZCL logger. After Simple
    Descriptor, the bridge read Basic attrs `0x0005,0x0004,0x4000,0x0007`,
    Level `0x0000`, Color Control `0x400b,0x400c,0x0007`,
    `0x0032,0x0033,0x0036,0x0037,0x003a,0x003b`, `0x0003,0x0004`,
    `0x4000,0x0001`, and `0x0008`; then it sent FC03 Discover Attributes
    Extended with manufacturer `0x100B`, start attr `0x0032`, max `3`. The fake
    still joined as v1 light 58, uncertified `Extended color light`, with no v2
    `gradient`.
  - `research/hue-api-diffs/discovery-capture-20260703-114828-color-points-fc03-discover/`
    added the exact LCX004/gamut C Color Control color points
    `0x0032/0x0033`, `0x0036/0x0037`, `0x003a/0x003b`, and minimal FC03 attrs
    `0x0032..0x0034` so the bridge's FC03 Discover Attributes Extended window
    was no longer empty. The bridge then imported the expected gamut values into
    v1/v2 (`red 0.6915/0.3083`, `green 0.1700/0.7000`,
    `blue 0.1532/0.0475`), but still created v1 light 59 with
    `capabilities.certified=false`, `productname=Extended color light`,
    `productid=null`, `swconfigid=null`, and v2 still had
    `product_data.certified=false`, `product_name=Extended color light`,
    `metadata.archetype=classic_bulb`, and no `gradient`. The late FC01 command
    `0x03` with payload `0001afb40700` was still observed on the fake, but the
    real LCX004 response to that bridge command has not been verified in a
    passive commissioning capture.
  - `research/hue-api-diffs/discovery-capture-20260703-115906-fc01-frame-control/`
    added explicit fake-side logging for the bridge's FC01 command. The bridge
    again sent FC01 command `0x03`, payload `0001afb40700`, manufacturer
    `0x100B`; the parsed command had `tsn=0xA2`, frame control `0x0C`, and
    `disable_default_resp=0`, so the bridge permits a default response. The
    bridge still created v1 light 60 as uncertified `Extended color light` with
    no v2 `gradient`. This confirms the FC01 request shape on the fake, but not
    the actual over-the-air response bytes sent by a real LCX004.
  - `research/hue-api-diffs/discovery-capture-20260703-120344-fc01-explicit-default-response/`
    tested intercepting FC01 command `0x03` in the raw ZCL handler and calling
    `zb_zcl_send_default_handler(..., ZB_ZCL_STATUS_SUCCESS)` explicitly. The
    firmware logged `FC01_EXPLICIT_DEFAULT_RESP`, then ZBOSS asserted in
    `common/zb_bufpool_mult_storage.c:105`, apparently because the command still
    reached the custom-cluster callback after the raw handler touched the buffer.
    After reboot the bridge still created v1 light 61 as uncertified
    `Extended color light`. The crash artifact is useful, but this first attempt
    mishandled buffer ownership and should not be treated as proof that an
    explicit FC01 default response is invalid.
  - `research/hue-api-diffs/discovery-capture-20260703-120928-fc01-explicit-default-response-consume/`
    fixed the raw-handler ownership bug by sending the FC01 default response and
    returning `true` so the custom-cluster callback does not process the same
    buffer. The firmware logged `FC01_EXPLICIT_DEFAULT_RESP` for command `0x03`
    and did not assert. The bridge still created v1 light 62 as uncertified
    `Extended color light`; v2 `product_data.certified=false` and the v2 light
    had no `gradient`. Keep this as the non-crashing FC01 default-response
    baseline, but treat it as insufficient by itself.
- 2026-07-03 follow-up after `capabilities.certified=true`:
  - `research/hue-api-diffs/discovery-capture-20260703-basic-write-success/`
    added explicit raw handling for the bridge's manufacturer-specific Basic
    writes to attrs `0x0051`, `0x0053`, and `0x0054`. The firmware parsed all
    three writes and returned successful Write Attributes responses without a
    ZBOSS buffer assertion. The bridge still left v1
    `capabilities.streaming.proxy/renderer=false`; v2 still omitted
    `hardware_platform_type`, `entertainment`, `motion_area_candidate`,
    `gradient`, `effects`, and `effects_v2`.
  - `research/hue-api-diffs/discovery-capture-20260703-basic-read-raw-productid/`
    added explicit raw handling for the bridge's manufacturer-specific Basic
    read batch `[0x0000,0x0001,0x0003,0x0040,0x0041,0x0050]` and corrected the
    known real `0x0021` value used by the normal attribute table. This fixed the
    v1 identity fields: the fake now reports
    `swconfigid=C4C1C739` and `productid=Philips-LCX004-1-GALSECLv1`, matching
    the real LCX004. It still did not enable v1 streaming or any v2 gradient /
    effects objects.
  - The same capture confirms the remaining API delta against a real LCX004:
    fake v1 has the full LCX004 identity and certified product name, but no
    `mindimlevel` and streaming remains false; fake v2 has no
    `hardware_platform_type`, no `entertainment` service, no
    `motion_area_candidate`, and no `gradient`/effects/content configuration.
  - `research/hue-api-diffs/gradient-probe-lcx004-basic-bridge-batch-20260703-live/`
    attempted to use `firmware/gradient_probe` to read the exact Basic
    manufacturer batch from the real `00:17:88:01:0b:89:54:2f` LCX004 without
    deleting it. The EUI-to-network-address lookup failed with ZDO status 133,
    so it did not capture the missing real responses for Basic attrs
    `0x0000/0x0001/0x0003` under manufacturer `0x100B`. The temporary probe
    join created an uncertified Hue record, which was deleted afterward.
  - `research/hue-api-diffs/discovery-capture-20260703-basic-0001-lcx004-rerun/`
    changed the raw Basic manufacturer read response for attr `0x0001` to match
    a real LCX004 / Play gradient tube response: success, type `U32`, value
    `0x00000000`. The bridge still created the fake as certified LCX004, but v1
    streaming stayed false and v2 still omitted `hardware_platform_type`,
    `entertainment`, and `gradient`.
  - `research/hue-api-diffs/discovery-capture-20260703-identify-raw-lcx004/`
    added a raw response for the bridge's manufacturer-specific Identify read
    `cluster=0x0003 attr=0x0000 manuf=0x100B`, matching the real LCX004 response
    `type=0x19 value=0x000B`. This moved the bridge far enough to send the later
    FC01 command `0x03` with payload `0001afb40700`, but v2 still had no
    hardware platform or gradient object.
  - `research/hue-api-diffs/discovery-capture-20260703-fc01-updates-basic21-lcx004/`
    tested whether the post-FC01 Basic `0x0021` value gates streaming. The
    firmware updated `0x0021` to `0x00152CA8`, matching one real LCX004 direct
    FC01-command capture. Hue still created a certified LCX004 without streaming
    or v2 gradient resources.
  - `research/hue-api-diffs/discovery-capture-20260703-realish-eui-lcx004/`
    changed the spoofed EUI from the synthetic-looking
    `00:17:88:01:0b:ff:fe:05` to the real-looking unused adjacent address
    `00:17:88:01:0b:89:54:30`. Hue treated it as a new certified LCX004 and
    wrote a different Basic `0x0054` value, but v1 streaming remained false and
    v2 still omitted `hardware_platform_type`, `entertainment`, and `gradient`.
  - `research/hue-api-diffs/real-headboard-vs-fake-readonly-20260703/` is the
    clean read-only API baseline after removing extra strips and adding the real
    LCX004 back as `Headboard`. Real v1 id `60`
    (`00:17:88:01:0b:89:54:2f-0b`) and fake v1 id `61`
    (`00:17:88:01:0b:ff:fe:05-0b`) are both certified LCX004 and both report
    `productname=Hue gradient lightstrip`. The remaining API gate is now very
    narrow: real v1 has `streaming.proxy=true` and `streaming.renderer=true`,
    while fake streaming is false; real v2 device has
    `hardware_platform_type=100b-118`, `entertainment`, and
    `motion_area_candidate`, while fake v2 has none of those; real v2 light has
    `gradient`, `effects`, `effects_v2`, and `content_configuration`, while fake
    v2 light has none of those.
  - `research/hue-api-diffs/gradient-probe-lcx004-fc03-missing-attrs-20260703-live/`
    actively read the bridge-observed FC03 attrs from the real `Headboard`
    LCX004. Direct reads of FC03 `0x0030`, `0x0031`, `0x0032`, `0x0033`,
    `0x0034`, `0x0037`, and `0x0038` all returned status `0x86` unsupported,
    even though the real FC03 Discover Attributes Extended response advertises
    `0x0032..0x0034`. The fake was updated to intercept the bridge's FC03 read
    batch and return success only for `0x0001`, `0x0002`, `0x0010`, `0x0011`,
    `0x0012`, and `0x0013`, with unsupported for the `0x0030+` direct-read
    attrs.
  - `research/hue-api-diffs/discovery-capture-20260703-fc03-read-unsupported-parity/`
    verified the new FC03 raw read handler. Hue still created a certified
    LCX004 with v1 streaming false, no v2 `hardware_platform_type`, and no
    gradient/effects resources. FC03 direct-read parity is therefore not
    sufficient by itself.
  - `research/hue-api-diffs/discovery-capture-20260703-ota-query-next-image-0118/`
    added an OTA client response path: when Hue sends OTA Image Notify
    (`cluster=0x0019`, command `0x00`, direction to client), the fake sends
    Query Next Image Request with manufacturer `0x100B`, image type `0x0118`,
    and file version `0x01002000`. Hue replied with a one-byte OTA response
    payload `0x98` and then created the fake v2 device with
    `hardware_platform_type=100b-118`. This proves the missing hardware
    platform field was gated by the OTA command exchange, not static OTA
    attributes. However, v1 streaming still remained false and v2 still omitted
    `entertainment`, `motion_area_candidate`, `gradient`, `effects`,
    `effects_v2`, and `content_configuration`.
- We still do not have a passive real LCX004 bridge-commissioning transcript.
  The current evidence rules out the simple descriptor `0x1000` gap, the
  currently spoofed FC01 `0x0002/0x0003` attrs, early ZDO endpoint 11/242
  descriptor parity, missing standard color-point attrs, and an empty FC03
  discovery window as sufficient fixes by themselves. Do not treat the FC01
  command/response behavior as understood yet. The next useful artifact is still
  a real LCX004 passive commissioning transcript, focused on the ZCL responses
  and manufacturer-specific commands that happen after Simple Descriptor.

## Repo Mapping

- `firmware/main/trust_center_key.h.in` documents where to place the local
  Hue-compatible trust-center link key.
- `firmware/main/argb_to_hue.c` configures network steering and currently
  spoofs a Signify/LCX004 identity to expose Hue gradient UI behavior.
- `firmware/gradient_probe/main/gradient_probe.c` is the exploratory firmware
  for observing real gradient device behavior, especially the Signify
  manufacturer-specific clusters `0xFC01`, `0xFC03`, and `0xFC04`.
- `firmware/sniffer_probe/` is a standalone, build-verified ESP32-C6 ZBOSS
  sniffer target for the missing passive real LCX004 commissioning transcript.
  It prints `MAC_RAW` and `ZBOSS_SNIFF` serial lines on Hue channel 25.
- `tools/capture_sniffer_probe.py` records `firmware/sniffer_probe` serial
  output into timestamped `research/hue-api-diffs/sniffer-capture-*`
  directories. It is passive by default, but can optionally delete a specific
  Hue v1 light id and start bridge search after opening serial when passed
  `--delete-light-id`, `--i-understand-delete-real-device`, and
  `--start-search`. Use `--list-candidates` to print certified LCX004 v1 ids
  before choosing a real light to delete/rejoin.
- `tools/capture_real_lcx004_sniffer.sh` is the guarded end-to-end wrapper for
  the real LCX004 capture: build/flash `firmware/sniffer_probe`, run the
  coordinated delete/search sniffer capture, and restore the main fake-light
  firmware afterward unless `--no-restore` is passed.
- `tools/sniffer_log_to_pcap.py` converts complete `MAC_RAW` lines from a
  sniffer serial log into an IEEE 802.15.4 pcap for Wireshark inspection.
- `tools/summarize_sniffer_log.py` produces `mac-summary.csv` and
  `mac-summary.md` from `MAC_RAW` lines, decoding the 802.15.4 MAC header and
  flagging visible candidate markers like `fc01_le`, `fc03_le`, and
  `signify_mfg_100b_le`.
- `research/hue_bridge_lights.json` contains Hue bridge API observations from
  the local network, including model identifiers, product IDs, uniqueid suffixes,
  and gradient-capable product records.
- `research/zdo-lcx004-vs-fake.md` compares the real LCX004 Active Endpoint and
  Simple Descriptor responses against the fake endpoint registrations and should
  be the starting point for the next descriptor-level experiment.
