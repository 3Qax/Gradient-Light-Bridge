# References

Local reference library for Hue/Zigbee reverse-engineering work related to
`argb-to-hue`.

Downloaded papers and slides are local workspace artifacts and are ignored by
git through `references/.gitignore`. Several public Hue/Zigbee papers discuss
or contain key material, so do not commit mirrored PDFs into the repo. Keep real
Hue trust-center keys in local, untracked `trust_center_key.h` files only.

## Local Files

### Papers

- `papers/morgner-2016-all-your-bulbs-are-belong-to-us-arxiv-1608.03732.pdf`
  - Source: https://arxiv.org/abs/1608.03732
  - PDF: https://arxiv.org/pdf/1608.03732
  - Relevance: ZLL security, touchlink attacks, Philips Hue/Osram/GE connected
    lighting behavior. Useful background for commissioning assumptions.

- `papers/morgner-2017-insecure-to-the-touch-wisec.pdf`
  - Source: https://doi.org/10.1145/3098243.3098254
  - PDF mirror used:
    https://www.wim.uni-mannheim.de/media/Lehrstuehle/wim/ths/STAFF/mueller.christian/wisec2017_touchlink.pdf
  - Relevance: Zigbee 3.0 touchlink commissioning security. Useful for
    understanding why touchlink and classical commissioning need to be kept
    separate when reasoning about Hue pairing.

- `papers/morgner-2018-security-economics-iot-standardization-arxiv-1810.12035.pdf`
  - Source: https://arxiv.org/abs/1810.12035
  - PDF: https://arxiv.org/pdf/1810.12035
  - Relevance: background on Zigbee/ZLL standardization incentives and security
    economics. Useful for understanding why deprecated ZLL decisions still
    affect Zigbee 3.0 ecosystems.

- `papers/oflynn-2016-a-lightbulb-worm-whitepaper.pdf`
  - Source:
    https://colinoflynn.com/2016/08/philips-hue-r-e-whitepaper-from-black-hat-2016/
  - PDF:
    https://blackhat.com/docs/us-16/materials/us-16-OFlynn-A-Lightbulb-Worm-wp.pdf
  - Relevance: Hue bridge/bulb hardware reverse engineering, security model,
    firmware update encryption/signing, and attack-surface notes.

- `papers/ronen-2017-iot-goes-nuclear-ieee-sp.pdf`
  - Source: https://www.eyalro.net/project/iotworm.html
  - Local file source:
    https://www.eyalro.net/project/iotworm/IotGoesNuclearIEEESP17.pdf
  - Publication DOI: https://doi.org/10.1109/SP.2017.14
  - Relevance: Philips Hue/ZLL chain-reaction work by Ronen, O'Flynn, Shamir,
    and Weingarten. Useful for OTA/key/security background.

- `papers/shafqat-2021-zleaks-zigbee-smart-homes-arxiv-2107.10830.pdf`
  - Source: https://arxiv.org/abs/2107.10830
  - PDF: https://arxiv.org/pdf/2107.10830
  - Relevance: background on passive inference from Zigbee smart-home traffic.
    Not Hue-certification specific, but useful for thinking about probe logs and
    recognizable command/reporting patterns.

- `papers/rohilla-2024-security-concerns-iot-light-bulbs-arxiv-2408.14613.pdf`
  - Source: https://arxiv.org/abs/2408.14613
  - PDF: https://arxiv.org/pdf/2408.14613
  - Relevance: background smart-light security/covert-channel paper. Lower
    direct implementation value, but related to Hue-light security literature.

### Slides

- `slides/oflynn-2016-a-lightbulb-worm-slides.pdf`
  - Source:
    https://blackhat.com/docs/us-16/materials/us-16-OFlynn-A-Lightbulb-Worm.pdf
  - Relevance: presentation version of Colin O'Flynn's Hue teardown.

### Article PDFs

These are local browser-saved PDFs moved from the NAS into
`references/articles/`. They are ignored by git.

- `articles/colin-oflynn-getting-root-on-philips-hue-bridge-2.0.pdf`
  - Source:
    https://colinoflynn.com/2016/07/getting-root-on-philips-hue-bridge-2-0/
  - Relevance: bridge 2.0 UART/root access context and bridge firmware
    inspection starting point.

- `articles/checkpoint-dont-be-silly-its-only-a-lightbulb.pdf`
  - Source:
    https://research.checkpoint.com/2020/dont-be-silly-its-only-a-lightbulb/
  - Relevance: Hue bridge `ipbridge` reverse engineering and Zigbee-to-main-CPU
    message processing.

- `articles/synacktiv-make-it-blink-philips-hue-bridge.pdf`
  - Source:
    https://www.synacktiv.com/en/publications/make-it-blink-over-the-air-exploitation-of-the-philips-hue-bridge
  - Relevance: recent Hue bridge RE and Zigbee frame processing context.

### Specs And Code Notes

- `specs/bifrost-hue-zigbee-format-fc03.md`
  - Source:
    https://github.com/chrivers/bifrost/blob/master/doc/hue-zigbee-format.md
  - Relevance: best current public documentation found for Hue
    manufacturer-specific cluster `0xFC03`, including gradient colors, effects,
    effect speed, scale, offset, and style fields.

- `specs/hue-gradient-command-wizard-custom-gradient-utils.tsx`
  - Source:
    https://github.com/kjagiello/hue-gradient-command-wizard/blob/main/src/modes/CustomGradient/utils.tsx
  - Relevance: implementation reference for the public Hue Gradient command
    wizard. Useful for comparing command payload generation against Bifrost and
    probe captures.

## Linked-Only Sources

These were not mirrored locally, either because they may include sensitive key
material or because direct automated download was blocked.

- PeeVeeOne, "Breakout breakthrough"
  - https://peeveeone.com/2016/11/breakout-breakthrough/
  - Useful for the classical commissioning vs touchlink distinction. Do not
    mirror this page into the repo because it publishes key material.

- PeeVeeOne, "Custom firmware Hue lights"
  - https://peeveeone.com/2016/11/custom-firmware-hue-lights/
  - Useful for historical custom ZLL/Hue-compatible light firmware behavior.

- Wejn.org, "Project intro: Reversing Philips Hue light driver"
  - https://wejn.org/2024/12/reversing-philips-hue-light-driver/
  - Useful for modern Hue light controller hardware context.

- Wejn.org, "Zigbee: Hue-llo world!"
  - https://wejn.org/2025/01/zigbee-hue-llo-world/
  - Useful because it overlaps with ESP32-C6 Hue-compatible device work.

- Hue Gradient command generator
  - https://kjagiello.github.io/hue-gradient-command-wizard/
  - Interactive UI for building Hue gradient payloads.

## Practical Takeaways For This Repo

- Joining Hue is a classical commissioning problem for this firmware, not a
  touchlink problem.
- `0xFC03` is the main public target for gradient payload correctness.
- `0xFC01` remains important because Hue bridges have been observed sending
  manufacturer-specific command `0x03` there after device announcements.
- The Hue API `capabilities.certified` flag is probably bridge-side product
  classification logic, not something explained by the public ZLL papers.
- If exact certification behavior becomes necessary, the most relevant RE path
  is bridge firmware / `ipbridge` discovery and product-mapping logic.

## Integrity

`SHA256SUMS` contains checksums for the downloaded local files.
