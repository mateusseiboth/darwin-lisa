# Bluetooth (lisa, iOS 26): WCN6750 over UART

`dlisa.bt.c` brings up the Bluetooth half of lisa's WCN6750 from XNU's BSD side, polling the UART (this kernel has no
interrupt for it), and in `bt=4` hands the link to user space as raw H4. Entry point: `void dlisa_bt_init(void);`, called
from the input init chain after devfs is up. `bt-wire.py` wires the files and the option. The init call is added by
hand, not by the wire.

## Hardware

Sources: the pmOS lisa DTB (`serial@99c000` / `bluetooth`), Linux 7.2.9 (`qcom_geni_serial.c`, `qcom-geni-se.c`,
`gcc-sc7280.c`, `clk-rcg2.c`, `clk-alpha-pll.c`, `btqca.c`, `hci_qca.c`, `qcom-rpmh-regulator.c`,
`qcom_spmi-regulator.c`) and Xiaomi's lisa `btpower.c`.

| | |
| --- | --- |
| UART | uart7 = QUP0 SE7 at `0x99c000` (wrapper `0x9c0000`), gpio28 CTS, 29 RTS, 30 TX, 31 RX, function qup07 (1) |
| control | gpio85 BT_EN (high = on), gpio86 SW_CTRL (input). GPIO 84 (Wi-Fi) is never read or written |
| clocks | GCC `0x52008` bits 6 m_ahb, 7 s_ahb, 8 core, 9 core_2x, 17 s7; CBCRs `0x17004/0x17008/0x23000/0x23008/0x1785c`; RCG `0x17860` (DFS-capable: `+0x14`) |
| PLL | GPLL0 at GCC `+0`, read only: must be locked at about 600 MHz with its even output (/2) on |
| supplies | l19b 1.8 V, s7b 0.952 V, s1b 1.856 V, s8b 1.256 V, s1c 2.2 V, l11c 2.8 V. State from SPMI (PM7325 slave 1, PM8350C slave 2; Sn at `0x14+3(n-1)`, Ln at `0x40+(n-1)`): status reg `0x08` bit 7. A vote (RPMh, `hakios_rsc_lock`) only when a read says off |
| firmware | `msbtfw11.mbn` (202508 B, else `msbtfw11.tlv` 190296 B), `msnv11.bin` (5899 B), via `dlisa_periph_file()` |

UART clock: the engine's clock is 3 Mbit/s times the oversampling (16 when the QUP is 2.5 or later, else 32), so
48 MHz (or 96 MHz) from GPLL0 even × 4/25 (× 8/25). 3 Mbit/s is exact (divider 1). 115200 uses divider 26, which gives
115384 baud (+0.16 %). With DFS on, the preset level with that exact rate is selected through `GENI_CLK_SEL`. If no
level has it, the bring-up stops.

What `msnv11.bin` holds, checked offline: one type-2 TLV. BD_ADDR tag = `00:00:00:00:5A:AD` (a placeholder). HCI tag
`15 11 ...` (in-band sleep off, baud code 0x11 = 3.2 M). Deep-sleep tag already 1. As btqca.c does, the HCI tag becomes
`95 <our baud code>` (`bt_ibs=0` keeps `15`).

## Boot-args

Nothing happens without `bt=`.

| boot-arg | what |
| --- | --- |
| `bt=1` | Read-only look: pins, GCC votes/CBCRs/RCG (and DFS levels), GPLL0, the six supplies. SE7's `GENI_FW_REVISION_RO` and the QUP version only if SE7's clocks read on |
| `bt=2` | Also: supplies (votes only for one that reads off), GPLL0 check, RCG, votes and branches, UART pins, GENI UART (FIFO, 8N1, RTS/CTS, 115200, RX started), BT_EN low 150 ms then high, 100 ms, SW_CTRL read, QCA get version, then HCI reset and Read Local Version in ROM. Chip left powered |
| `bt=3` | Like bt=2 without the ROM reset (Linux does none), then Linux's WCN6750 setup: baud change to 3 M (verified by a second version request), patch config, rampatch, 10 ms, NVM, SoC logging off, HCI reset, build info, BD_ADDR, Read Local Version. Then an LE scan and a classic inquiry, logging every device |
| `bt=4` | Like bt=3, then user space gets the link (below) |
| `bt_scan_secs=N` | LE scan length at bring-up (1..60, default 10) |
| `bt_inquiry=0` | skip the classic inquiry |
| `bt_fast=0` | stay at 115200 |
| `bt_ibs=0` | leave the chip's in-band sleep off (NVM HCI tag byte 0 not ORed with 0x80) |

The order is Linux's: the UART is configured and receiving before BT_EN goes high (serdev is open before
`qca_power_on`), not after.

Any unexpected answer or timeout stops at that step. On giving up, the driver drives BT_EN low (if it drove it),
cancels the engine's commands and puts back the UART pins, clock votes, AHB CBCRs and RCG as it found them. A supply
this driver voted on is left on, and the log says so.

## User space (bt=4)

- `/dev/hakios-bt` and `/dev/uart.bluetooth` (same device, 0666): raw H4.
  - **write**: one or more whole packets (type 1 command, 2 ACL, 3 SCO, 5 ISO). All packets are checked before any is
    sent. A partial or unknown packet gets `EINVAL`, a write over 4096 B gets `EMSGSIZE`, and a write during a kernel
    scan gets `EBUSY`.
  - **read**: blocks (or `EWOULDBLOCK` with `O_NONBLOCK`) and returns whole packets, type byte first. Use a buffer of
    at least 1200 B: a smaller one gets `EINVAL`. In-band-sleep bytes (`0xFD/0xFE/0xFC`) are handled in the kernel and
    never delivered.
- kern_ctl `com.apple.uart.bluetooth` (datagram, not privileged, one client): bluetoothd's
  `H4:POSIX:com.apple.uart.bluetooth` transport.
  - The client's byte stream is reassembled into H4 packets.
  - `TIOCGETA` (opt `0x40487413`) returns a raw 8N1 CRTSCTS termios at the current baud. `TIOCSETA` (`0x80487414`) is
    logged and ignored.
  - While a client is connected, RX packets go to it and not to `/dev/hakios-bt`.
  - Vendor commands (OGF 0x3F) that are not Qualcomm's own (`fc00 fc0b fc0c fc14 fc17 fd70`) are answered locally
    with a Command Complete, status 0. This includes Broadcom's VSCs and the baud change, because the kernel owns the
    link rate.
  - The first 64 commands are logged in full. After that, each opcode's count is logged at powers of two.
- `kern.hakios.bt_status`: level, state (and the failed step), chip versions, firmware, build label, baud, BD_ADDR,
  in-band sleep, packets in/out, errors, drops, scans, devices, bridge counters.
- `kern.hakios.bt_scan` (RW int, any user, bt=3 or 4): write N (1..60) to run an LE scan of N s in a kernel thread.
  Results are logged. Read it to get the devices found by the last scan.
- RX polling: every 1 ms within 200 ms of traffic, 10 ms when quiet, 20 ms after 10 s of silence (`msleep` with a
  timeout; a send wakes the poller). RTS/CTS stops the chip if the FIFO fills.
- In-band sleep, as `hci_qca` does once setup is over: before the first packet the host sends WAKE_IND and waits up to
  500 ms for WAKE_ACK. It answers every WAKE_IND with WAKE_ACK and never sends SLEEP_IND, so the link stays awake.

## Log markers (`grep 'bt: '`)

| level | success | failure |
| --- | --- | --- |
| all | `bt: level N (WCN6750 ...` then numbered steps `bt: K: ...` | `bt: K: FAILED: <why>` then `bt: giving up (...)` |
| 1 | `bt: SE7: fw revision ... (protocol 2 = UART)`, `bt: GPLL0: ... locked, running`, `bt: supply ... in regulation`, `bt: look done` | `SE7's clocks are not all on: its registers are not read` (a finding, not a failure) |
| 2 | `bt: UART ready`, `bt: BT_EN reads 1, SW_CTRL reads 1`, `bt: QCA product ... firmware set 11`, `bt: local version: ... manufacturer 29 (Qualcomm)`, `bt: level 2 done` | `does not answer the version request` |
| 3 | `bt: the chip answers at 3000000 baud`, `msbtfw11.mbn downloaded in N ms`, `msnv11.bin downloaded`, `bt: firmware build:`, `bt: BD_ADDR`, `bt: QCA setup complete`, `bt: found LE ...`, `bt: LE scan done`, `bt: inquiry complete`, `bt: level 3 done` | `segment N of M failed`, `HARDWARE ERROR` |
| 4 | `bt: serving:`, `bt: bridge: kern_ctl com.apple.uart.bluetooth registered (0)`, `bt: bridge: client connected`, `bt: bridge: command 0x....` | `device nodes cannot be made` |

## Verified offline vs needs the phone

Verified offline:

- `ksyntax.py` against the tree's flags: 0 errors.
- `bt-wire.py` dry run on a scratch copy: idempotent; one `files` block, one `MASTER` option, the tag on `BSD_DEV`.
- The firmware file formats (mbn is ELF; tlv download mode 3 = only the last segment acknowledged; the NVM tags above).
- The register offsets and bit meanings against the Linux sources listed.
- Supply peripheral addresses follow `qcom_spmi-regulator.c`'s PMIC layout and are checked at run time by the type
  register.

Needs the phone:

- Whether SE7 carries UART firmware (protocol 2) and the FIFO interface (not GSI-only, as SE13 turned out to be).
- Whether SE7 is accessible to this EE.
- GPLL0's state and whether the s7 RCG is in DFS mode with a 48 MHz level.
- The QUP version, which decides the oversampling.
- Whether the AHB branches behave as on wrap1 (`hakios.goodix.c`).
- The rails' real state, and whether status bit 7 is a reliable "on".
- The chip's timing after BT_EN: 150 ms low and 100 ms high are margins over Linux's 50/50 ms.
- The vendor baud command at 3 M and the chip's behaviour around it.
- Whether the chip sends anything for the unacknowledged segments.
- In-band sleep with a host that never sleeps.
- bluetoothd's behaviour on the bridge.
