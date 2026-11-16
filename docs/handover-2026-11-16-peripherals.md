# Handover 2026-11-16: the remaining peripherals (host only)

Nothing was run on the phone and nothing was written to it. Details, hardware, iOS paths and costs:
[peripherals.md](peripherals.md).

## Built, NOT written

| Piece | sha256 |
| --- | --- |
| kernel `research/hackios-xnu-kernel/kernel-16k-periph1.bak` (buttons, torch, haptic, bt, audio; `periph-build.sh`) | `40815677f8d56468e31549d8c58cdbdbeae59f057de6cd24bd1f28d2dd5754e0` |
| root `userland-26/out/system-26.apfs.full-9a91ed47` (int3 + bluetoothd + the conf with bluetoothd) | `9a91ed478bbd3d7d6a6439f148192ea8e6b14b0116c028d83184fe74e817706f` |

## State per item

| Item | Kernel | iOS | First run |
| --- | --- | --- | --- |
| Keys | `dlisa.buttons.c`, on with `lisa=1` | done (libdlisa, existing) | P1 |
| Vibration (AW8624) | `dlisa.haptic.c`, haptic=1..5 | plan: `vibrator` IOService + `closed-loop`=0 in the registry | P1 |
| Flashlight | `dlisa.torch.c`, torch=1..3 | done (AVFlashlight in SpringBoard) | P1 (driver), then with root 9a91ed47 (Control Center) |
| Brightness | no | plan: DSI DCS 0x51 + BrightnessSystem hook | |
| Bluetooth | `dlisa.bt.c`, bt=1..4 + bluetoothd bridge | virtual controller (dlisa_btvirt=1) done; real path planned | P2 (bt=1, 2, 3), P3 (Settings) |
| Speaker | `dlisa.sound.c` + TFA9873, sound=1..4 with adsp=1 | plan: HAL plug-in in /Library/Audio/Plug-Ins/HAL | P3 (sound=2), then sound=3 sound_amps=2 |
| NFC, fingerprint, modem | survey | | |

## Commits

3c73a31 (selector keeps bluetoothd), e672e8e (bluetoothd virtual controller), 3d66fde (keys, torch, haptic),
d2b828b (periph-build.sh), 7ed24f0 + 02a6b16 (flashlight in SpringBoard; 35563ba between them was a mistake undone by
02a6b16), 1963144 (Bluetooth and speaker drivers), this document.

## Incident

The first build (15:05) used the old `.work/rebuild26.sh`, whose `apply.sh` half-applied `sched-prefer-select.patch` to
the shared `osfmk/kern/sched_prim.c`; the scheduler worker restored it (3c766b2f) and `sched_prim.o` was rebuilt by the
camera worker. `periph-build.sh` replaced that flow: lock, no `apply.sh`, and no copy unless the linked kernel is new
and has every driver's symbol.
