# 2026-07-03 Groups, Scenes, and FC03 Explicit Responses

Goal: determine whether the remaining Hue gradient gate is caused by unanswered
post-classification ZCL commands after LCX004 product/platform certification.

Firmware changes tested:

- Scenes cluster `0x0005` Get Scene Membership (`cmd=0x06`) now returns a
  standard success response for the requested group with no scene ids.
- FC03 manufacturer cluster `0xfc03` Discover Attributes Extended
  (`cmd=0x15`, manufacturer `0x100b`, start `0x0032`, max `3`) now returns the
  real LCX004 response bytes:
  `00 32 00 20 07 33 00 20 07 34 00 20 07`.
- Groups cluster `0x0004` Add Group (`cmd=0x00`) now returns a standard success
  response for the requested group.
- Scenes cluster `0x0005` Remove Scene (`cmd=0x02`, non-manufacturer-specific)
  is a standard command, despite sharing the numeric command id with global
  Write Attributes and the manufacturer-specific scene-store command. On scene
  deletion the bridge sends `group_id` + `scene_id`; firmware should evict the
  matching FC03 scene-cache entry, persist the cache, and respond with the
  standard Remove Scene response.

Evidence captures:

- `discovery-capture-20260703-scenes-membership-response/`: first Scenes
  membership attempt used response command `0x07`; bridge rejected it with
  default response status `0x81` (`MALFORMED_COMMAND`).
- `discovery-capture-20260703-scenes-membership-response-cmd06-rerun/`:
  corrected Scenes response command `0x06`; bridge accepted it and then read
  Basic manufacturer attrs `0x0021` and `0x0020`.
- `discovery-capture-20260703-scenes-membership-response-cmd06/` and
  `discovery-capture-20260703-fc03-discext-explicit-after-scenes/`: intermediate
  runs where the bridge joined the node but did not complete the descriptor/ZCL
  classifier sweep.
- `discovery-capture-20260703-fc03-discext-explicit-after-scenes-rerun/`:
  explicit FC03 extended-discovery response fired cleanly.
- `discovery-capture-20260703-groups-scenes-fc03-explicit/`: Groups Add Group,
  Scenes membership, and FC03 extended-discovery were all answered explicitly.

Result:

- Fake still classifies as certified LCX004 / Hue gradient lightstrip.
- Fake v1 `capabilities.streaming.proxy` and `renderer` remain `false`.
- Fake v2 device still lacks `entertainment` and `motion_area_candidate`
  services.
- Fake v2 light still lacks `gradient`, `effects`, `effects_v2`, and
  `timed_effects`.

Conclusion:

The bridge accepts the standard Groups/Scenes responses and the exact FC03
extended-discovery response, but those are not sufficient to unlock the
entertainment/gradient classifier. The remaining gate is likely outside this
post-classification Groups/Scenes/FC03 discovery path.
