# ATS 1.62.1.1s compatibility validation

Validated locally on Windows on 2026-11-03 against the user's running ATS build. Process IDs, full memory addresses, save/profile names and game binaries are excluded from the repository.

## Exact executable profile

| Item | Value |
| --- | --- |
| PE timestamp | `0x6AE990FB` |
| Image size | `0x039DB000` |
| Root code RVA | `0x005F730A` |
| RIP-relative root global RVA | `0x03719CA8` |
| Root code bytes | `48 8B 1D 97 29 12 03 48 8B F9 48 8B 9B B0 31 00 00` |
| Engine vtable RVA | `0x023273C0` |
| Fuel formula RVA | `0x0063FD40` |
| Fuel formula bytes | `F3 0F 10 B3 C0 01 00 00 F3 0F 58 B1 58 11 00 00` |
| Context fuel correction | `+0x1158` |

The root-to-driver/context/truck chain remains `+0x31B0 -> +0x18 -> +0x1F8`. The accessory container remains at context `+0x150`. Engine runtime torque and RPM fields retain the upstream offsets.

An eight-byte insertion changes the truck's tail fields: permanent engine/transmission/cabin wear is at `+0x1A8/+0x1AC/+0x1B0`, current wear at `+0x1B4/+0x1B8/+0x1BC`, fuel ratio at `+0x1C0`. Chassis and wheel fields and fuel capacity `+0x190` are unchanged. A per-profile shift preserves older builds.

## Live evidence

- RenCloud telemetry revision 12 loaded successfully; game ID 2 (ATS).
- Fuel capacity 1136 L and about 899.79 L remaining matched the truck memory fields.
- Five component wear values matched telemetry.
- Exactly one engine object passed the new vtable and field signature checks: torque 2779 Nm, idle 650 rpm, limits 2100 rpm.
- The trainer's production EngineTuner applied 1.25x torque: 2779 -> 3473.75 Nm. Reading again after 1.5 seconds confirmed the multiplier.
- Exactly three expected torque fields changed. Restore returned torque to 2779 Nm, with zero differences remaining in the bounded container snapshot.

## Limits

This validates engine memory changes and restoration, not measured acceleration or a displayed HP label. A live truck-switch test and ETS2 1.62 validation were not performed. The upstream vehicle-following/restore logic is retained. ATS 1.62 economy layouts are unverified; automatic money/XP editing refuses this build. Optional local chassis catalogs and cockpit assets are not bundled.

Offline regression: **496 passed, 0 failed**. The English interface smoke test created its window and rendered 30 frames successfully (exit 0). Full machine-specific diagnostic reports remain local.
