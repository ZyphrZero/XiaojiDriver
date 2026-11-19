# HIDMaestro attribution

The files under [`driver/`](../../driver/) are derived from the HIDMaestro UMDF2 HID
driver:

- Project: https://github.com/hifihedgehog/HIDMaestro
- Upstream commit: `d9199785d09a3c206dd30caf163b33295a5ad0a1`
- Upstream license: MIT (see `LICENSE` in this directory)

The xiaoji package renames the driver binary, INF, hardware ID, registry
paths, named shared objects and version resource. The HID report handling,
shared-memory frame protocol and UMDF2 IOCTL paths are retained from the
upstream implementation.
