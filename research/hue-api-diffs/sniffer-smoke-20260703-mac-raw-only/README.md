# Sniffer Capture

- port: `/dev/ttyACM0`
- baud: `115200`
- duration_seconds: `20`
- hue_actions: `none`
- expected use: flash `firmware/sniffer_probe`, then commission or rejoin a real LCX004 while this runs.
- note: the sniffer is passive; Hue API actions only remove/search from the bridge side.
