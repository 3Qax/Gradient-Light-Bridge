# LCX004 ZDO Descriptor Comparison

This note compares the ZDO-level descriptor evidence we have for real Hue
gradient devices against the current fake LCX004 firmware. The current focus is
the bridge's early discovery/classification path and the ZCL reads it unlocks
after accepting the descriptor surface.

## Real LCX004 Evidence

Source:
`research/gradient-probe-capture-20260703-1036-real-baseline/serial-scan.log`

`gradient_probe` actively queried two real LCX004-class gradient devices and
received the same high-level shape from both:

```text
PROBE: DISCOVER active_ep nwk=0x0c67 count=2 eps=[11,242]
PROBE: DISCOVER active_ep nwk=0x48ad count=2 eps=[11,242]
```

Endpoint 11 is the HA extended color light endpoint:

```text
ep=11 profile=0x0104 device=0x010d ver=1 in=11 out=1
in clusters:
  0x0000, 0x0003, 0x0004, 0x0005, 0x0006, 0x0008,
  0x1000, 0xfc03, 0x0300, 0xfc01, 0xfc04
out clusters:
  0x0019
```

Observed ordering differs between real devices, but the set is stable. One real
device returned `0xfc04, 0xfc01, 0x0300` near the end instead of
`0x0300, 0xfc01, 0xfc04`, so cluster order should not be treated as a strong
classification signal yet.

Endpoint 242 is the Green Power endpoint:

```text
ep=242 profile=0xa1e0 device=0x0061 ver=0 in=0 out=1
in clusters:
  none
out clusters:
  0x0021
```

## Fake Evidence

### Default / Discoverable Shape

Source:
`research/hue-api-diffs/discovery-capture-20260703-105427-long-passive/`

The fake firmware advertised endpoint 11 as:

```text
FAKE_DESCRIPTOR: endpoint=11 profile=0x0104 device=0x010d version=1
server_clusters=[
  0x0000, 0x0003, 0x0004, 0x0005, 0x0006, 0x0008,
  0x1000, 0xfc03, 0x0300, 0xfc01, 0xfc04
]
client_clusters=[0x0019]
```

The bridge created v1 light 63, but classified it as uncertified
`Extended color light`. During the 180 second passive capture, the bridge sent
FC01 command `0x03`, but did not send the FC01 read-attribute sequence that the
probe could read from real devices.

Important limitation: this is the descriptor registered by the fake and printed
by firmware, not an independent ZDO query of the fake from a second Zigbee
device. With the hardware currently available, the ESP32-C6 is either the fake
or the probe, not both.

### Endpoint 242 As A Normal Endpoint

Source:
`research/hue-api-diffs/discovery-capture-20260703-110225-endpoint242/`

The fake was configured to print the real-looking endpoint 242 shape:

```text
FAKE_DESCRIPTOR: endpoint=242 profile=0xa1e0 device=0x0061 version=0
server_clusters=[]
client_clusters=[0x0021]
```

It joined the network, then crashed:

```text
Guru Meditation Error: Core 0 panic'ed (Load access fault).
MEPC: 0x42047978  RA: 0x4204793e  MCAUSE: 0x00000005
```

The decoded backtrace from this image landed in:

```text
zdo_active_ep_res
zdo_active_ep_res
zb_zdo_data_indication
esp_zb_task at firmware/main/argb_to_hue.c
```

That makes Active Endpoint response generation the first concrete failure after
adding endpoint 242 as a normal client-only endpoint.

### Endpoint 242 As A Gateway Endpoint

Source:
`research/hue-api-diffs/discovery-capture-20260703-110430-endpoint242-gateway/`

Using `esp_zb_ep_list_add_gateway_ep()` avoided the crash and printed both
endpoint descriptors. The bridge did not create any new v1 light during the
180 second capture, and no uncertified fake was present afterward.

This means the Green Power endpoint cannot simply be added through the gateway
helper and considered equivalent to a real LCX004. Its ZDO representation still
differs enough to alter bridge discovery behavior.

### Endpoint 242 As Native ZBOSS Green Power Proxy Basic

Source:
`research/hue-api-diffs/discovery-capture-20260703-native-gppb-endpoint242/`

The fake was configured with ZBOSS's native Green Power Proxy Basic descriptor
instead of the generic ESP-Zigbee custom-cluster wrapper. The firmware built and
printed the expected endpoint 242 descriptor:

```text
FAKE_DESCRIPTOR: endpoint=242 profile=0xa1e0 device=0x0061 version=0
server_clusters=[]
client_clusters=[0x0021] source=zboss_gppb
```

It then asserted during Zigbee stack startup, before network steering:

```text
Zigbee stack assertion failed zcl/zcl_common.c:103
abort() was called at PC 0x42019c43 on core 0
```

The capture was stopped after preserving the serial crash log, so this artifact
has before-snapshots and `serial-discovery.log`, but no after-snapshots. This
mode did not reach the bridge classification stage.

### ZDO Descriptor Override For Endpoint 242

Source:
`research/hue-api-diffs/discovery-capture-20260703-113156-zdo-override/`

The fake kept only endpoint 11 registered with ESP-Zigbee, but installed an APS
indication override for ZDO discovery. During Hue commissioning the bridge
asked for the same early descriptors as the real LCX004 probe path:

```text
ZDO override: Active_EP_req nwk=0x5117 from=0x0001
ZDO override: Active_EP_rsp nwk=0x5117 eps=[11,242] to=0x0001
ZDO override: Simple_Desc_req nwk=0x5117 ep=11 from=0x0001
ZDO override: Simple_Desc_rsp nwk=0x5117 ep=11 len=32 to=0x0001
ZDO override: Simple_Desc_req nwk=0x5117 ep=242 from=0x0001
ZDO override: Simple_Desc_rsp nwk=0x5117 ep=242 len=10 to=0x0001
```

The bridge created v1 light 57, but still classified it as uncertified:

```text
uniqueid=00:17:88:01:0b:ff:fe:05-0b
modelid=LCX004
productname=Extended color light
capabilities.certified=false
```

The v2 device record also stayed generic:

```text
product_data.certified=false
product_data.manufacturer_name=Signify Netherlands B.V.
product_data.model_id=LCX004
product_data.product_name=Extended color light
metadata.archetype=classic_bulb
```

The v2 light record for `/lights/57` had no `gradient` object. This is the first
confirmed run where the fake answered the bridge's early Active Endpoint and
Simple Descriptor reads with the real LCX004 endpoint 11/242 shape and still did
not unlock certification.

### ZDO Node Descriptor Override

Sources:

- Real Node Descriptor:
  `research/hue-api-diffs/gradient-probe-lcx004-node-desc-20260703-live/serial-node-desc.log`
- Fake discovery run:
  `research/hue-api-diffs/discovery-capture-20260703-node-desc/serial-discovery.log`

The real Headboard LCX004 Node Descriptor read as:

```text
flags=0x4001 mac=0x8e manufacturer=0x100b max_buf=82
max_in=128 server_mask=0x2c00 max_out=128 desc_cap=0x00
```

After the fake overrode its Node Descriptor response with those real values, the
bridge followed a different classifier path than earlier fake runs. It requested
Node Descriptor, Active Endpoint, and Simple Descriptor, then immediately issued
a much deeper ZCL sweep:

```text
ZDO override: Node_Desc_req
ZDO override: Node_Desc_rsp flags=0x4001 mac=0x8e server=0x2c00
ZDO override: Active_EP_rsp eps=[11,242]
ZDO override: Simple_Desc_rsp ep=11
ZDO override: Simple_Desc_rsp ep=242
```

The newly unlocked reads included manufacturer-specific Basic, Level, Color,
FC03, Scenes, Identify, and FC04 attributes:

```text
Basic 0x0000 manuf=0x100b attrs=[0x0000,0x0001,0x0003,0x0040,0x0041,0x0050]
OnOff 0x0006 attrs=[0x4003]
Level 0x0008 manuf=0x100b attrs=[0x0003,0x0004]
Color 0x0300 attrs=[0x4010]
Color 0x0300 manuf=0x100b attrs=[0x0003,0x0004]
FC03 0xfc03 manuf=0x100b attrs=[0x0001,0x0002,0x0010,0x0011,0x0012,0x0013,0x0030,0x0038,0x0037,0x0033,0x0032]
Scenes 0x0005 manuf=0x100b attrs=[0x0001]
Identify 0x0003 manuf=0x100b attrs=[0x0000]
FC04 0xfc04 manuf=0x100b attrs=[0x0000]
```

It also sent repeated manufacturer-specific Basic cluster command `0xc0` with
payload `000000000040`, then sent a ZDO leave with reset:

```text
cluster=0x0000 cmd=0xc0 manuf=0x100b payload=000000000040
ZDO leave: with reset
```

This is significant because the deeper manufacturer-specific sweep did not
appear until the Node Descriptor matched the real LCX004. That makes Node
Descriptor a confirmed gate into a different bridge classifier path. The failure
after that point should be treated as evidence that one or more newly requested
manufacturer-specific attributes or the Basic `0xc0` command behavior still
differs from the real LCX004.

## Side-By-Side

| Item | Real LCX004 | Fake default | Fake endpoint 242 normal | Fake endpoint 242 gateway | Fake endpoint 242 native GPPB | Fake ZDO override | Fake ZDO override + FC01 default response | Fake ZDO + Node Descriptor override |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Active endpoints | `[11,242]` | effectively endpoint 11 only | intended `[11,242]`, crashes in Active EP response path | intended `[11,242]`, no light created | intended `[11,242]`, asserts before steering | answered `[11,242]` | answered `[11,242]` | answered `[11,242]` |
| EP 11 profile/device/ver | `0x0104 / 0x010d / 1` | same | same | same | same | same | same | same |
| EP 11 server clusters | `0000,0003,0004,0005,0006,0008,1000,fc03,0300/fc04,fc01,fc04/0300` | same set | same set | same set | same set | same set, response len 32 | same set, response len 32 | same set, response len 32 |
| EP 11 client clusters | `0019` | `0019` | `0019` | `0019` | `0019` | `0019` | `0019` | `0019` |
| EP 242 profile/device/ver | `0xa1e0 / 0x0061 / 0` | missing | configured same | configured same | configured same | answered same, response len 10 | answered same, response len 10 | answered same, response len 10 |
| EP 242 clusters | no input, output `0x0021` | missing | configured same | configured same | configured same | no input, output `0x0021` | no input, output `0x0021` | no input, output `0x0021` |
| Node Descriptor | `flags=0x4001 mac=0x8e manufacturer=0x100b server=0x2c00` | stack default | stack/default path | not classified | not reached | not overridden | not overridden | overridden to real values |
| FC01 command `0x03` handling | default success in active probe | stack/default behavior unknown | not reached | not classified | not reached | observed/logged only | explicit default response sent and buffer consumed without assertion | explicit default response still present |
| Bridge result | certified gradient device with v2 `gradient` | uncertified generic light | crash before classification | no v1 light created | no classification; startup assertion | uncertified generic light; no v2 `gradient` | uncertified generic light; no v2 `gradient` | deeper mfg read sweep, then bridge sends ZDO leave/reset and keeps no fake light |

## Current Interpretation

The fake now matches the real LCX004 endpoint 11 descriptor set closely enough
that the next gap is no longer the simple descriptor itself. Do not treat the
existing FC01 active-probe data as proof of real bridge-commissioning behavior:
the bridge sends FC01 command `0x03` to the fake, but we still do not have a
fresh passive real LCX004 transcript showing the real response to that exact
bridge command.

Endpoint 242 / ZDO behavior was a real compatibility gap, but the override run
shows it is not sufficient by itself:

- Real LCX004 devices answer Active Endpoint with `[11,242]`.
- The fake without endpoint 242 can be classified only as a generic uncertified
  light.
- Adding endpoint 242 through the normal endpoint API trips the stack while
  constructing or sending the Active Endpoint response.
- Adding endpoint 242 through the gateway API avoids the crash but changes the
  ZDO/descriptor surface enough that Hue does not create a light.
- Adding endpoint 242 through a native ZBOSS Green Power Proxy Basic descriptor
  also is not enough; mixed ESP-Zigbee endpoint registration plus native GP AF
  context registration asserts in ZCL startup before joining.
- Overriding ZDO Active Endpoint and Simple Descriptor responses to report the
  real `[11,242]` shape lets the device join and be created again, but Hue still
  marks it uncertified and omits v2 `gradient`.
- Sending an explicit FC01 `0x03` default success response is viable only when
  the raw handler returns `true` after `zb_zcl_send_default_handler()`. That
  avoids the buffer-pool assertion seen in the first attempt, but Hue still
  marks the fake uncertified, so FC01 default response behavior is not the whole
  classifier.
- Overriding the Node Descriptor to the real LCX004 values is the first change
  that visibly unlocks a deeper Hue classifier path. The bridge then reads a
  large set of manufacturer-specific attributes and sends Basic command `0xc0`;
  after the fake does not satisfy that path, the bridge sends a ZDO leave/reset
  and does not keep the fake light.

Next experiments should focus on the attributes and command behavior newly
exposed by the Node Descriptor override:

1. Actively read the exact newly requested attributes from a real LCX004:
   Basic manufacturer-specific `0x0000,0x0001,0x0003,0x0040,0x0041,0x0050`,
   OnOff `0x4003`, Level manufacturer-specific `0x0003,0x0004`, Color `0x4010`
   and manufacturer-specific `0x0003,0x0004`, FC03 manufacturer-specific
   `0x0001,0x0002,0x0010,0x0011,0x0012,0x0013,0x0030,0x0038,0x0037,0x0033,0x0032`,
   Scenes manufacturer-specific `0x0001`, Identify manufacturer-specific
   `0x0000`, and FC04 manufacturer-specific `0x0000`.
2. Capture the real LCX004 behavior for Basic cluster manufacturer-specific
   command `0xc0` with payload `000000000040`.
3. Patch the fake to return the real values/behavior, then rerun discovery with
   Node Descriptor override still enabled.
4. If those values match real behavior and the bridge still rejects the fake,
   revisit passive real commissioning or install-code / device-auth material.

## Certification Breakthrough

Sources:

- Real node-path attributes:
  `research/hue-api-diffs/gradient-probe-lcx004-nodepath-attrs-20260703-live/serial-nodepath-attrs.log`
- Real Basic `0xc0` variants:
  `research/hue-api-diffs/gradient-probe-lcx004-basic-c0-variants-20260703-live/serial-basic-c0-variants.log`
- Successful fake discovery:
  `research/hue-api-diffs/discovery-capture-20260703-basic-c0-raw-c1-variants/`

After adding the real newly requested attributes and raw Basic-cluster
manufacturer-specific `0xc0 -> 0xc1` behavior, the Hue bridge certified the
fake LCX004:

```text
v1 id=62
uniqueid=00:17:88:01:0b:ff:fe:05-0b
modelid=LCX004
productname=Hue gradient lightstrip
capabilities.certified=true
```

The decisive Basic `0xc0` behavior is payload-dependent:

```text
request 000000000040 -> c1 payload length 64
request 003500000040 -> c1 payload length 43
```

The first raw `0xc1` implementation used a global/common response frame, and
Hue answered with default response `status=0x84` for command `0xc1`. Changing it
to a cluster-specific manufacturer response made Hue accept the exchange and
continue to the next classifier stage.

In the successful run, Hue moved past the old leave/reset point and then wrote
Basic manufacturer-specific attrs `0x0051`, `0x0053`, and `0x0054`, performed
FC03 extended discover, read OTA client attr `0x6400`, performed normal state
reads, and created the retained certified light.

Current caveat: the v2 device record is certified and product-classified as
`Hue gradient lightstrip`, but the captured v2 light record still does not show
a `gradient` object. Treat `certified=true` as solved; exposing the Hue app
Gradient tab may require one more v2/gradient-specific behavior after
certification.

## Active OTA Metadata Probe

`firmware/gradient_probe` now has a focused, non-destructive metadata mode for
real LCX004 devices. It joins the Hue network as a probe and sends ordinary ZCL
Read Attributes to the two known certified LCX004 endpoints. It does not delete
or re-add real lights.

Build:

```sh
cd firmware/gradient_probe
sg docker -c '../in-docker.sh idf.py build'
```

Flash and monitor it when the board can temporarily stop running the fake-light
firmware:

```sh
cd firmware/gradient_probe
sg docker -c 'PORT=/dev/ttyACM0 ../in-docker.sh idf.py -p /dev/ttyACM0 flash monitor'
```

After join, the probe automatically starts the OTA/basic metadata scan for the
known real LCX004 devices:

- `Headboard LCX004`: `00:17:88:01:0b:89:54:2f`
- `Floor LCX004`: `00:17:88:01:0b:e4:38:82`

Manual serial commands:

```text
metadata
otasweep <nwk-short-addr-hex> [endpoint-hex]
readattr <nwk-short-addr-hex> <cluster-hex> <attr-hex> [manufacturer-code-hex]
```

The metadata sweep reads:

- Basic `0x0004`, `0x0005`, `0x000a`, `0x000c`, `0x000e`, `0x4000`
- OTA `0x0002`, `0x0007`, `0x0008`, `0x000a`
- OTA `0x6400` with no manufacturer code and with Signify manufacturer code
  `0x100b`

The important output pair is:

```text
PROBE: metadata_read label=ota.hue_attr_6400 ...
PROBE: read_attr_resp cluster=0x0019 ...
  attr=0x6400 status=0x.. type=0x.. size=.. data=...
```

If the real LCX004 returns a concrete value for OTA `0x6400`, the next fake
firmware experiment should add that attribute to the fake OTA client cluster and
rerun `tools/capture_hue_discovery.py` to check whether v1
`capabilities.certified` flips to `true` and v2 exposes `gradient`.

### LCX004 Metadata Results

Do not use `0xd938` / Basic model `915005987201` as LCX004 evidence. That
device is the certified Signe gradient floor, not either LCX004 lightstrip. The
failed experiment based on that wrong sample is:

```text
research/hue-api-diffs/gradient-probe-otasweep-20260703-124017-d938-to-cli/
research/hue-api-diffs/discovery-capture-20260703-real-basic-ota-identity/
```

Actual LCX004 metadata from the headboard lightstrip was captured at:

```text
research/hue-api-diffs/gradient-probe-lcx004-metadataeui-20260703-124721/
research/hue-api-diffs/gradient-probe-lcx004-basic-mfgattrs-20260703-124813-48ad/
```

Real Headboard LCX004 endpoint `11`, short address `0x48ad`, returned:

```text
Basic 0x0004 ManufacturerName: "Signify Netherlands B.V."
Basic 0x0005 ModelIdentifier: "LCX004"
Basic 0x000a ProductCode: empty char string
Basic 0x000c ManufacturerVersionDetails: unsupported
Basic 0x000e ProductLabel: unsupported
Basic 0x4000 SWBuildID: "1.129.5"
Basic 0x0020, manuf 0x100b: "0:PWRON@1"
Basic 0x0021, manuf 0x100b: U32 0x001517ec
OTA 0x0002 FileVersion, dir to client: 0x01002000
OTA 0x0007 ManufacturerID, dir to client: 0x100b
OTA 0x0008 ImageType, dir to client: 0xffff
OTA 0x000a ImageStamp, dir to client: unsupported
OTA 0x6400, dir to client: unsupported
OTA 0x6400, dir to client, manuf 0x100b: unsupported
```

The fake now emulates those Basic/OTA values, including the Signify-specific
Basic attributes. Clean validation still failed:

```text
research/hue-api-diffs/discovery-capture-20260703-lcx004-basic-mfgattrs-clean/
```

Result:

```text
matched-after: id=57 uniqueid=00:17:88:01:0b:ff:fe:05-0b modelid=LCX004 certified=False productname=Extended color light
```

That rules out Basic manufacturer/model/SW build, ProductCode, OTA
manufacturer/image type/file version, OTA `0x6400`, and Signify-specific Basic
`0x0020/0x0021` as sufficient by themselves. The next comparison should read
the rest of the bridge-observed attributes from a real LCX004 and compare them
with fake responses, especially:

```text
Basic: 0x0007
Level: 0x0000
OnOff: 0x0000
Color Control: 0x400b, 0x400c, 0x0007, 0x0032, 0x0033, 0x0036, 0x0037,
               0x003a, 0x003b, 0x0003, 0x0004, 0x4000, 0x0001, 0x0008,
               0x4002
FC03: discover_attr_ext from 0x0032, max 3
Groups: 0xe9d5
```

### Bridge-Read Attribute Alignment

The remaining bridge-observed reads were captured from the real Headboard
LCX004 at:

```text
research/hue-api-diffs/gradient-probe-lcx004-bridge-attrs-20260703-125751-48ad/
```

Important real values:

```text
Basic 0x0007 PowerSource: enum8 0x01
OnOff 0x0000: bool false
Level 0x0000 CurrentLevel: uint8 0xfe
Color 0x400b: uint16 0x0099
Color 0x400c: uint16 0x01f4
Color 0x0007 ColorTemperature: uint16 0x01f4
Color points: 0x0032=0xb105, 0x0033=0x4eec, 0x0036=0x2b85,
              0x0037=0xb333, 0x003a=0x2738, 0x003b=0x0c2c
Color 0x0003/0x0004 CurrentX/Y: 0x9f8f / 0x5c06
Color 0x4000 EnhancedCurrentHue: 0x0d10
Color 0x0001 CurrentSaturation: 0xfe
Color 0x0008 ColorMode: enum8 0x01
Color 0x4002: uint8 0x00
Groups 0xe9d5: unsupported
```

The fake was updated to align these values and retested at:

```text
research/hue-api-diffs/discovery-capture-20260703-lcx004-bridge-read-attrs-rerun/
```

Result remained:

```text
matched-after: id=58 uniqueid=00:17:88:01:0b:ff:fe:05-0b modelid=LCX004 certified=False productname=Extended color light
```

The real LCX004 FC03 state attribute `0x0002` was then read directly:

```text
research/hue-api-diffs/gradient-probe-lcx004-fc03-readcheck-20260703-131042-48ad/
```

Real value:

```text
attr=0x0002 status=0x00 type=0x41 size=31
data=1e4b0100fe8f9f065c1350000000922d6d922d6d922d6d922d6d922d6d2800
```

The fake FC03 initial state was updated to that value and retested:

```text
research/hue-api-diffs/discovery-capture-20260703-real-fc03-state/
```

Result remained:

```text
matched-after: id=59 uniqueid=00:17:88:01:0b:ff:fe:05-0b modelid=LCX004 certified=False productname=Extended color light
```

This narrowed the unresolved gate further. The bridge does not read FC03
`0x0002` during classification; it sends FC03 `discover_attr_ext` from `0x0032`
with max `3`. The active probe's ordinary `discattr` command did not produce a
response from the real LCX004 because it sent the regular Discover Attributes
command (`0x0c`), not Discover Attributes Extended (`0x15`).

The probe now has a `discext` serial command that sends global ZCL command
`0x15` and logs response command `0x16`. Against the real Headboard LCX004:

```text
research/hue-api-diffs/gradient-probe-lcx004-fc03-discext-20260703-live/serial-fc03-discext.log
```

Real response:

```text
disc_attr_ext_resp cluster=0xfc03 completed=0 raw=00320020073300200734002007
  ext_attr=0x0032 type=0x20 access=0x07
  ext_attr=0x0033 type=0x20 access=0x07
  ext_attr=0x0034 type=0x20 access=0x07
```

The fake FC03 attrs `0x0032`, `0x0033`, and `0x0034` were updated from
read-only to read/write/reporting (`0x07`) and retested:

```text
research/hue-api-diffs/discovery-capture-20260703-fc03-discext-access/
```

Result still remained:

```text
matched-after: id=60 uniqueid=00:17:88:01:0b:ff:fe:05-0b modelid=LCX004 certified=False productname=Extended color light
```

The same capture showed the bridge later sends FC01 command `0x03` with payload
`0001afb40700`. A real LCX004 was queried with that exact payload:

```text
research/hue-api-diffs/gradient-probe-lcx004-fc01-cmd3-bridge-payload-20260703-live/serial-fc01-cmd3.log
```

Real response:

```text
default_resp cluster=0xfc01 resp_to_cmd=0x03 status=0x00 (0)
```

That matches the fake's explicit FC01 default-success response, so this FC01
command is no longer a useful mismatch by itself. The next strongest artifact
is still a real passive commissioning capture, because active reads now match
the bridge-observed FC03 and FC01 command behavior but the bridge still marks
the fake uncertified.

## Passive Sniffer Probe

`firmware/sniffer_probe` is a build-verified ESP32-C6 ZBOSS sniffer target for
the real LCX004 commissioning transcript. It is separate from the fake-light
firmware because `zboss_start_in_sniffer_mode()` cannot later commission the
same boot as a normal Zigbee device.

Build:

```sh
cd firmware/sniffer_probe
sg docker -c '../in-docker.sh idf.py build'
```

Flash when ready to use the ESP32-C6 as a sniffer:

```sh
cd firmware/sniffer_probe
sg docker -c 'PORT=/dev/ttyACM0 ../in-docker.sh idf.py -p /dev/ttyACM0 flash'
```

The guarded one-command wrapper for a real LCX004 capture is:

```sh
tools/capture_real_lcx004_sniffer.sh \
  --light-id <real-lcx004-v1-id> \
  --i-understand-delete-real-device
```

That wrapper builds and flashes `firmware/sniffer_probe`, runs
`tools/capture_sniffer_probe.py` with `--snapshot-hue`, `--delete-light-id`,
and `--start-search`, then restores the main fake-light firmware when the
capture exits. Use `--no-restore` only if the board should remain flashed as a
sniffer.

Capture serial output while removing/re-adding or factory-resetting a real
LCX004 on the Hue bridge:

```sh
python3 tools/capture_sniffer_probe.py --port /dev/ttyACM0 --seconds 300
```

By default the sniffer capture is passive only: it does not change the Hue
network and it does not delete or re-add the real device. For a coordinated
bridge-side capture, open the sniffer first, then have the script delete a
specific v1 light id and start search:

```sh
python3 tools/capture_sniffer_probe.py --list-candidates
```

As of the latest read-only Hue inventory check, real certified LCX004 candidates
were:

- v1 id `25`: `Floor lamp`
- v1 id `37`: `Headboard light strip`

The current fake was v1 id `62` and only appears with:

```sh
python3 tools/capture_sniffer_probe.py --list-candidates --no-candidate-certified
```

```sh
python3 tools/capture_sniffer_probe.py \
  --port /dev/ttyACM0 \
  --seconds 300 \
  --snapshot-hue \
  --delete-light-id <real-lcx004-v1-id> \
  --i-understand-delete-real-device \
  --start-search
```

The delete/search actions use `HUE_BRIDGE_HOST` and `HUE_API_KEY` from the
environment or repo-local `.env`. The script opens serial before deleting or
starting search so the early bridge traffic is not missed. Deleting via the Hue
API removes the bridge record; if the real LCX004 does not immediately rejoin,
factory-reset or otherwise put that light into pairing while the capture is
still running.

The capture script writes `serial-sniffer.log`, `interesting-lines.log`,
`mac-raw.pcap`, `mac-raw.summary.md`, `mac-summary.csv`, `mac-summary.md`, and
a short README under `research/hue-api-diffs/sniffer-capture-*-real-lcx004/`.
The pcap conversion uses IEEE 802.15.4 no-FCS link type `230` by default. If
later evidence shows the ESP callback includes FCS bytes, rerun the converter
with `--with-fcs`.
The firmware currently prints two raw markers:

- `MAC_RAW`: raw MAC callback frames from `esp_zb_mac_raw_frame_handler_register()`.
- `ZBOSS_SNIFF`: buffers delivered by `zboss_sniffer_start()`.

Manual conversion for an existing log:

```sh
python3 tools/sniffer_log_to_pcap.py \
  research/hue-api-diffs/sniffer-capture-YYYYMMDD-HHMMSS-real-lcx004/serial-sniffer.log \
  --out research/hue-api-diffs/sniffer-capture-YYYYMMDD-HHMMSS-real-lcx004/mac-raw.pcap
```

Manual MAC summary for an existing log:

```sh
python3 tools/summarize_sniffer_log.py \
  research/hue-api-diffs/sniffer-capture-YYYYMMDD-HHMMSS-real-lcx004/serial-sniffer.log
```

`mac-summary.md` is the first check after a real capture. It should show data
frames on Hue channel 25, active source/destination addresses, and any cleartext
candidate markers such as `fc01_le`, `fc03_le`, `ota_0019_le`,
`signify_mfg_100b_le`, or `model_lcx004_ascii`. Lack of marker hits does not
prove absence of those clusters, because normal Zigbee NWK/APS/ZCL payloads may
be encrypted; it only means those marker bytes were not visible in captured MAC
payloads.

This probe overwrites the fake-light firmware on the board. Restore the current
fake after sniffing with:

```sh
cd firmware
sg docker -c './in-docker.sh idf.py build'
sg docker -c 'PORT=/dev/ttyACM0 ./in-docker.sh idf.py -p /dev/ttyACM0 flash'
```
