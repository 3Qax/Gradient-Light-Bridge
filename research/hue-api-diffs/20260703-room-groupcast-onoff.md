# Hue Room On/Off Groupcast

Observed symptom: toggling the whole room or whole house in the Hue app produced
no `ZCL_RAW` serial output and no OpenRGB reaction, even though direct device
commands and scene activation worked.

Interpretation: room/house actions are likely Zigbee groupcast commands. If the
endpoint is not in the ZBOSS APS group table, the frame is filtered before the
ZCL raw logger and parser ever see it.

Evidence from earlier discovery captures:

```text
ZCL_RAW ep=64->11 cluster=0x0004 cmd=0x00/read_attr payload_len=3
ZCL_READ_ATTRS: cluster=0x0004 attrs=[0xe9d5]
GROUPS_ADD_GROUP_RESP_RAW: tsn=... to=0x0001 ep=11 group=0xe9d5
```

The old handler responded success to Hue's Groups cluster `Add Group` command,
but it did not add endpoint 11 to the local APS group table. That means Hue could
believe the light accepted group `0xe9d5` while the ESP still dropped later
group-addressed room On/Off commands.

Fix: `handle_groups_add_group_raw()` now calls `hue_join_group()`, which submits
`zb_zdo_add_group_req()` for the requested group and endpoint before sending the
Hue-facing Add Group Response. The serial CLI also supports:

```text
group <group-id> [endpoint]
```

This lets an already-paired device join a known group without forcing Hue to
resend `Add Group`.

Post-fix local test after flashing:

```text
group 0xe9d5
GROUPS_APS_ADD: requesting group=0xe9d5 ep=11
GROUPS_APS_ADD_CONFIRM: group=0xe9d5 ep=11 status=0 in_group=1
```

Standard On/Off cluster attribute changes already call `emit_state_json()`, so
once groupcast On/Off frames reach the endpoint they should produce `DATA` events
for the daemon.
