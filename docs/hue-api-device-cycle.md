# Hue API Device Remove/Re-Add Cycle

Use this when iterating on firmware identity, endpoints, Basic attributes, or
manufacturer-specific clusters. It removes the ESP32 light record from the Hue
bridge, starts a fresh light search, and captures JSON snapshots for comparison.

The script uses the local Hue v1 API:

- `GET /api/<username>/lights`
- `GET /api/<username>/lights/<id>`
- `PUT /api/<username>/lights/<id>/state`
- `GET /api/<username>/config`
- `DELETE /api/<username>/lights/<id>`
- `POST /api/<username>/lights`
- `GET /api/<username>/lights/new`

Kimi's resumed transcript for this project is stored under
`/home/jakub/.kimi-code/sessions/wd_argb-to-hue_23024effe1ef/session_163469f9-097b-4b9c-bfd9-b40db1f7527f/agents/main/wire.jsonl`.
The useful bridge API patterns found there were:

```bash
# Inspect bridge Zigbee config.
tools/hue_light_cycle.py config

# Inventory lights and inspect one full record.
tools/hue_light_cycle.py list
tools/hue_light_cycle.py get 61

# Exercise a discovered fake light through Hue.
tools/hue_light_cycle.py set-state 61 '{"on":true,"bri":254}'
tools/hue_light_cycle.py set-state 61 '{"xy":[0.7,0.3]}'
tools/hue_light_cycle.py set-state 61 '{"on":false}'

# Remove the stale bridge light record and start discovery.
tools/hue_light_cycle.py delete --id 61
tools/hue_light_cycle.py search --poll-seconds 90
```

## Credentials

Set these in your shell, not in tracked files:

```bash
export HUE_BRIDGE_HOST=192.168.1.2
export HUE_API_KEY='your-hue-v1-username'
```

The script also auto-loads a local untracked `.env` in the repo root:

```bash
HUE_BRIDGE_HOST=192.168.1.2
HUE_API_KEY=your-hue-v1-username
```

For this workspace, `.env` was recovered from Kimi's resumed transcript and is
ignored by git.

If you do not have an API key yet, press the Hue bridge link button and run:

```bash
curl -k -X POST "https://$HUE_BRIDGE_HOST/api" \
  -H 'Content-Type: application/json' \
  -d '{"devicetype":"argb-to-hue#dev"}'
```

Save the returned `success.username` value as `HUE_API_KEY`.

## Safe Listing

Kimi often used raw `curl` plus small Python filters while debugging. The
script equivalents are:

```bash
# Raw: curl -k -s "https://$HUE_BRIDGE_HOST/api/$HUE_API_KEY/config"
tools/hue_light_cycle.py config

# Raw: curl -k -s "https://$HUE_BRIDGE_HOST/api/$HUE_API_KEY/lights"
tools/hue_light_cycle.py list

# Raw: curl -k -s "https://$HUE_BRIDGE_HOST/api/$HUE_API_KEY/lights/61"
tools/hue_light_cycle.py get 61
```

List only likely fake/uncertified LCX004 records:

```bash
tools/hue_light_cycle.py list \
  --manufacturer 'Signify Netherlands B.V.' \
  --modelid LCX004 \
  --uncertified-only
```

If the firmware is using the spoofed Signify OUI, this is also useful:

```bash
tools/hue_light_cycle.py list \
  --uniqueid-prefix '00:17:88:01:0b' \
  --uncertified-only
```

## Delete And Re-Add

Run this after flashing firmware changes and before putting the Hue app into a
fresh discovery state:

```bash
mkdir -p research/hue-api-cycles

tools/hue_light_cycle.py cycle \
  --manufacturer 'Signify Netherlands B.V.' \
  --modelid LCX004 \
  --uncertified-only \
  --snapshot-dir "research/hue-api-cycles/$(date +%Y%m%d-%H%M%S)" \
  --poll-seconds 90
```

The script prints the matched light(s) and asks for `DELETE` before removing
anything. Add `-y` only after verifying the filters:

```bash
tools/hue_light_cycle.py cycle \
  --manufacturer 'Signify Netherlands B.V.' \
  --modelid LCX004 \
  --uncertified-only \
  --snapshot-dir "research/hue-api-cycles/$(date +%Y%m%d-%H%M%S)" \
  --poll-seconds 90 \
  -y
```

The snapshot directory can contain:

- `lights-before.json`
- `new-lights.json`
- `lights-after.json`

## Delete Only

Use this if you want to remove the bridge record, erase/reflash the ESP32
manually, and start discovery later:

```bash
tools/hue_light_cycle.py delete \
  --manufacturer 'Signify Netherlands B.V.' \
  --modelid LCX004 \
  --uncertified-only
```

To repeat the exact transcript-proven stale-light deletion:

```bash
tools/hue_light_cycle.py delete --id 61
```

## Search Only

Use this when the device was already removed:

```bash
tools/hue_light_cycle.py search --poll-seconds 90
```

## State Testing

Kimi used direct Hue state writes to verify that a discovered fake light was
controllable and that the firmware emitted serial state updates:

```bash
tools/hue_light_cycle.py set-state 61 '{"on":true,"bri":254}'
tools/hue_light_cycle.py set-state 61 '{"xy":[0.7,0.3]}'
tools/hue_light_cycle.py set-state 61 '{"on":false}'
```

## Agent Notes

- Do not use unfiltered `delete` or `cycle`.
- Prefer `--uncertified-only` when working on the fake device, because real Hue
  gradient devices report `capabilities.certified: true`.
- The v1 API delete removes the bridge's light record. It does not erase the
  ESP32 Zigbee/NVS state.
- If firmware identity changed but discovery still looks stale, erase or reset
  the ESP32 Zigbee storage before running the search again.
- If the bridge still reports `"productname": "Extended color light"` and
  `certified: false`, preserve the snapshots for diffing against real Hue
  gradient devices.
