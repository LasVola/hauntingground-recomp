# Haunting Ground (USA) disc identity

Checked 2026-10-18 against the image this port was built from.

| File | Size | SHA-256 | SHA-1 | MD5 |
|---|---|---|---|---|
| `Haunting Ground (USA).iso` | 4,589,453,312 | `3F8C658EEE1B35147672C597C0DBE8E36552172ABF83571C52753EEC9A50BCCA` | `45B9DABE26A0B9082B13635E85BA4635C8F91A31` | `CAEDD8FE523B9EE0C9F3775BDFFB0FAA` |
| `SLUS_210.75` (boot ELF on the disc) | 3,650,608 | `3B374D53A499D2C17B205274EE9EB34280768F294F970EBF6AE6731F6A2DACB8` | `E0867D2EC83E6E2FCCF147AD9E645A9A662D1C15` | |

The boot ELF SHA-1 equals the OpenHuey baserom (`config/SLUS_210.75.yaml` sha1).

SYSTEM.CNF: `BOOT2 = cdrom0:\SLUS_210.75;1`, `VER = 1.01`, `VMODE = NTSC`.

A version string is not a hash.

- `build.py generate` and the port at startup (`src/hg/game_disc.cpp`) check the boot ELF SHA-256 and refuse any
  other disc.
- Put the ISO in `game\` (or pass `--iso`, set `HG_ISO`, or set `[Game] Iso=` in `hauntingground.ini`).
- Recompute (PowerShell): `Get-FileHash "<file>" -Algorithm SHA256` (also SHA1 or MD5).
