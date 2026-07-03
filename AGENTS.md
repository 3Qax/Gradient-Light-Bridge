# Agent Notes

## Hue Bridge API

- Use `tools/hue_light_cycle.py` for Hue bridge inspection, stale-light deletion,
  rediscovery, and state testing. Do not rediscover the Hue v1 API flow from
  scratch.
- The script auto-loads the repo-local untracked `.env` file. In this workspace
  it contains `HUE_BRIDGE_HOST` and `HUE_API_KEY` recovered from Kimi's resumed
  transcript. Do not print or commit the API key.
- Main commands:
  - `tools/hue_light_cycle.py config`
  - `tools/hue_light_cycle.py list`
  - `tools/hue_light_cycle.py get <light-id>`
  - `tools/hue_light_cycle.py set-state <light-id> '{"on":true,"bri":254}'`
  - `tools/hue_light_cycle.py delete --id <light-id>`
  - `tools/hue_light_cycle.py search --poll-seconds 90`
  - `tools/hue_light_cycle.py cycle --modelid LCX004 --manufacturer 'Signify Netherlands B.V.' --uncertified-only`
- Full workflow notes are in `docs/hue-api-device-cycle.md`.
- Kimi's transcript is at
  `/home/jakub/.kimi-code/sessions/wd_argb-to-hue_23024effe1ef/session_163469f9-097b-4b9c-bfd9-b40db1f7527f/agents/main/wire.jsonl`.

## Reference PDFs

- Downloaded and browser-saved reference PDFs under `references/papers/`,
  `references/slides/`, and `references/articles/` are local-only and ignored
  by git.
- Keep Hue trust-center keys and Hue API credentials out of tracked files.
