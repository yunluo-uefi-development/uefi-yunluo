# Repository rules

`TRACKING.md` is the authoritative engineering log: hardware-confirmed facts,
exclusions, and build recipes. Read it before changing anything.

- **Provenance.** `README.md` records where the baseline came from (a snapshot
  with no git ancestry) and which fixes were merged from
  `yunluo-uefi-development/uefi-yunluo`. Do not invent upstream history.
- **Host checks are not device observations.** A build passing, or a FAT
  partition being readable, proves nothing about Windows or XNU booting. Only
  state what was seen on the panel.
- **Two memory maps, not interchangeable.** The 8 GiB map must never run on a
  6 GiB unit: describing RAM that does not exist is what makes Windows die in
  Phase 0. Select with `./build-yunluo-mu.sh DEBUG [8g|6g]`.
- **Boot chain is load-bearing.** The BootShim keeps the fake Linux header's
  Text Offset / Image Size at zero; the upstream variant that fills them in does
  not reach UEFI on this device. `boot_a` only — no `fastboot boot`, and stock
  `vendor_boot` stays byte-identical.
- **Debug scaffolding is not product.** `HandoffAuditDxe` / `HandoffQuiesceDxe`
  and the GICR/DTB audits exist to answer one question each; they are to be
  stripped from release images.
- Device writes (flashing, partition edits) require explicit human authorization.
  Do not publish private vendor inputs (stock images, extracted firmware).
