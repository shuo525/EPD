# BLE EPD frame protocol

The EPD service streams two 48,000-byte planes directly to the UC8179. The
nRF52832 does not allocate a complete 96,000-byte framebuffer.

## GATT UUIDs

| Item | UUID | Properties |
|---|---|---|
| Service | `12345678-1234-5678-1234-56789abcdef0` | Primary service |
| Control | `12345678-1234-5678-1234-56789abcdef1` | Write, Write without response |
| Status | `12345678-1234-5678-1234-56789abcdef2` | Read, Notify |
| Frame Data | `12345678-1234-5678-1234-56789abcdef3` | Write with response |

Frame Data intentionally supports only reliable Write with response. Each
write may contain 1 to 244 bytes. The actual limit is the negotiated ATT MTU
minus three bytes. A 23-byte ATT MTU therefore uses 20-byte packets.

## Transfer sequence

1. Subscribe to Status notifications, or read Status after every step.
2. Write ASCII `FRAME:KW` to Control.
3. Wait for `FRAME:KW_READY`.
4. Send exactly 48,000 bytes from `assets/generated/text/text_kw.bin` to Frame Data.
5. Wait for `FRAME:KW_DONE`.
6. Write ASCII `FRAME:RED` to Control.
7. Wait for `FRAME:RED_READY`.
8. Send exactly 48,000 bytes from `assets/generated/text/text_red.bin` to Frame Data.
9. Wait for `FRAME:RED_DONE`.
10. Write ASCII `FRAME:SHOW` to Control.
11. Wait for `FRAME:DONE`. The display has now refreshed.

Do not send `text_frame.bin` as one operation: the two planes require separate
UC8179 commands and a status handshake at the 48,000-byte boundary.

## Control command hexadecimal values

```text
FRAME:KW    46 52 41 4D 45 3A 4B 57
FRAME:RED   46 52 41 4D 45 3A 52 45 44
FRAME:SHOW  46 52 41 4D 45 3A 53 48 4F 57
FRAME:ABORT 46 52 41 4D 45 3A 41 42 4F 52 54
```

`FRAME:KW` also restarts a failed or incomplete transfer. `FRAME:ABORT` stops
the current transfer without refreshing the display.

## Errors

| Status or ATT error | Meaning |
|---|---|
| `ERROR:NEED_KW` | `FRAME:RED` was sent before a complete K/W plane. |
| `ERROR:NEED_RED` | `FRAME:SHOW` was sent before a complete red plane. |
| `FRAME:EPD_ERROR` | SPI or UC8179 busy handling failed. |
| ATT insufficient resources | Sender was faster than the small receive queue; retry that packet. |
| ATT write rejected | Frame Data was written before a `*_READY` status or after 48,000 bytes. |

The sender must not append CR/LF, a packet header, an offset, or a checksum to
Frame Data. Only raw bytes from the corresponding `.bin` file are accepted.
