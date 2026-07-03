## Hue Bridge API

- Use `tools/hue_light_cycle.py` for Hue bridge inspection, stale-light deletion,
  rediscovery, and state testing. Do not rediscover the Hue v1 API flow from
  scratch. Main commands:
  - `tools/hue_light_cycle.py config`
  - `tools/hue_light_cycle.py list`
  - `tools/hue_light_cycle.py get <light-id>`
  - `tools/hue_light_cycle.py set-state <light-id> '{"on":true,"bri":254}'`
  - `tools/hue_light_cycle.py delete --id <light-id>`
  - `tools/hue_light_cycle.py search --poll-seconds 90`
  - `tools/hue_light_cycle.py cycle --modelid LCX004 --manufacturer 'Signify Netherlands B.V.' --uncertified-only`

## ESP-IDF Docker Builds

- Build firmware through `firmware/in-docker.sh`; do not assume host ESP-IDF is
  installed.
- Main firmware:
  - `cd firmware`
  - `./in-docker.sh idf.py build`
  - If Docker group switching is needed: `sg docker -c './in-docker.sh idf.py build'`
- Flash on Linux:
  - `cd firmware`
  - `PORT=/dev/ttyACM0 ./in-docker.sh idf.py -p /dev/ttyACM0 flash`
- Gradient probe:
  - `cd firmware/gradient_probe`
  - `../in-docker.sh idf.py build`
- Override ESP-IDF image version with `IDFVER`, e.g. `IDFVER=v5.3.2 ./in-docker.sh idf.py build`.

## Reference PDFs

- Downloaded and browser-saved reference PDFs under `references/papers/`,
  `references/slides/`, and `references/articles/`.
- Keep Hue trust-center keys and Hue API credentials out of tracked files.
