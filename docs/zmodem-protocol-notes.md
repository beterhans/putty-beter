# ZMODEM protocol — byte-exact implementation notes

Engineering reference for writing a ZMODEM sender + receiver from scratch in C
(for PuTTY). Everything below is derived from the **specification** and from the
**original public-domain `rzsz`** package; no GPL code was consulted.

## Sources

| # | Source | What it was used for | Licence status |
|---|--------|----------------------|----------------|
| S1 | *The ZMODEM Inter Application File Transfer Protocol*, Chuck Forsberg, Omen Technology Inc., Rev Oct-14-88 (`zmodem.txt`). Fetched from `http://cristal.inria.fr/~doligez/zmodem/zmodem.txt` (104047 bytes, byte-identical to `reference/zmodem.txt` in `FGasper/zmodemjs`, sha256 prefix `da1a6c39…`). A re-typeset copy (J.G.Harston, 08-08-96) is at `https://b0b.com/ZMODEM.TXT`; same text, different pagination. | All protocol prose: header formats, frame-type semantics, ZFILE layout, streaming/abort/Attn rules, CRC-32 description | The document states verbatim: *"This file may be redistributed without restriction provided the text is not altered."* → redistributable, no restriction |
| S2 | Original `rzsz` by Chuck Forsberg: `ZMODEM.H` (05-23-87), `ZM.C` (05-09-88), `RZ.C` (v2.03 05-17-88), `SZ.C`, `CRCTAB.C`, `CRC.C`. Fetched as `https://raw.githubusercontent.com/FGasper/zmodemjs/master/reference/rzsz.zip` (mirror of `RZSZ.ARC`) | Exact constant *values*, exact CRC algorithm parameters + table, exact escape set, exact ZFILE field formatting, exact handshake byte sequences | Package described in the ZMODEM FAQ/READMEs as public domain / no restrictions. Precise notices present in the archive: `CRC.C`/`CRCTAB.C` carry *"Copyright (C) 1986 Gary S. Brown. You may use this program, or code or tables extracted from it, as desired without restriction."*; the `updcrc` macro carries *"Programmers may incorporate any or all code into their programs, giving proper credit within the source"* (Satchell/Forsberg); `RZ.C` TAR helpers are *"Public Domain TAR by John Gilmore"*. The `RZ.C`/`SZ.C`/`ZM.C` files themselves carry **no** licence notice. The sentence *"No licensing, trademark, or copyright restrictions apply to the use of the protocol, the Unix rz/sz source code and the ZMODEM name."* is **not** in this original archive — it appears in the later `lrzsz` README, which was deliberately **not** consulted. |
| S3 | `lrzsz` | **NOT consulted** (GPL-2.0) | excluded |
| S4 | `roscopeco/mbzm` | **NOT consulted** (CRC code GPL) | excluded |
| S5 | NuttX `apps/system/zmodem` | **NOT consulted** (not needed; spec + rzsz sufficed) | Apache-2.0, usable but unused |

Facts taken from S2 because the spec is ambiguous or silent, all named here:

1. the exact numeric values of the frame types and of every flag bit (S1 chapter 11
   contains several typos: it lists `CANCRY` twice, as `8` and `010`);
2. the exact CRC-16 accumulation (`updcrc` with two trailing zero bytes) and the
   CRC-32 residual constant `0xDEBB20E3`;
3. the exact escape set and the "escape CR only after `@`" rule;
4. the exact ZFILE numeric field string emitted by `sz` (`"%lu %lo %o 0 %d %ld"`);
5. the exact handshake bytes (`rz\r` + ZRQINIT; ZRINIT retry count; `canistr`).

---

## 1. Control bytes and constants

Octal is the notation used in the C sources (`ZMODEM.H`); hex is what you need in code.

| Name | Oct | Dec | Hex | Char | Meaning |
|------|-----|-----|-----|------|---------|
| `ZPAD` | 052 | 42 | 0x2A | `*` | frame padding / start of header |
| `ZDLE` | 030 | 24 | 0x18 | `CAN` (^X) | data-link escape |
| `ZDLEE` | 0130 | 88 | 0x58 | `X` | ZDLE escaped as transmitted (`ZDLE ^ 0100`) |
| `ZBIN` | 0101 | 65 | 0x41 | `A` | binary frame indicator (CRC-16) |
| `ZHEX` | 0102 | 66 | 0x42 | `B` | hex frame indicator (CRC-16) |
| `ZBIN32` | 0103 | 67 | 0x43 | `C` | binary frame indicator (CRC-32) |
| `ZCRCE` | 0150 | 104 | 0x68 | `h` | CRC next, frame ends, header follows |
| `ZCRCG` | 0151 | 105 | 0x69 | `i` | CRC next, frame continues non-stop |
| `ZCRCQ` | 0152 | 106 | 0x6A | `j` | CRC next, frame continues, ZACK expected |
| `ZCRCW` | 0153 | 107 | 0x6B | `k` | CRC next, ZACK expected, end of frame |
| `ZRUB0` | 0154 | 108 | 0x6C | `l` | decodes to 0x7F |
| `ZRUB1` | 0155 | 109 | 0x6D | `m` | decodes to 0xFF |
| `XON` | 021 | 17 | 0x11 | ^Q | flow control |
| `XOFF` | 023 | 19 | 0x13 | ^S | flow control |
| `CAN` | 030 | 24 | 0x18 | ^X | same value as `ZDLE`; 5 in a row = abort |
| `CR` / `LF` / `BS` | 015 / 012 / 010 | 13 / 10 / 8 | 0x0D / 0x0A / 0x08 | | hex-header trailer, cancel string |

### Frame types (header type byte)

| Value | Name | Direction / meaning |
|-------|------|---------------------|
| 0 | `ZRQINIT` | sender → receiver: "send me your ZRINIT". `ZF0` = `ZCOMMAND` (1) if a command follows, else 0 |
| 1 | `ZRINIT` | receiver → sender: capabilities. `ZF0`/`ZF1` = flags, `ZP0`/`ZP1` = receiver buffer size (0 = non-stop I/O) |
| 2 | `ZSINIT` | sender → receiver (optional): `ZF0` = flags, followed by a ZCRCW subpacket holding the NUL-terminated Attn string |
| 3 | `ZACK` | acknowledgement of ZSINIT / ZCHALLENGE / ZCRCQ / ZCRCW. `ZP0..ZP3` = file offset |
| 4 | `ZFILE` | sender → receiver: file metadata; `ZF0` conv, `ZF1` mgmt, `ZF2` transport, `ZF3` extended; followed by ZCRCW subpacket |
| 5 | `ZSKIP` | receiver → sender: skip this file |
| 6 | `ZNAK` | last header was garbled |
| 7 | `ZABORT` | receiver → sender: terminate batch (user request). Sender replies with the ZFIN sequence |
| 8 | `ZFIN` | sender → receiver: end of session. Receiver answers with its own ZFIN |
| 9 | `ZRPOS` | receiver → sender: resume at `ZP0..ZP3` |
| 10 | `ZDATA` | sender → receiver: `ZP0..ZP3` = file offset, one or more data subpackets follow |
| 11 | `ZEOF` | sender → receiver: end of file, `ZP0..ZP3` = ending offset |
| 12 | `ZFERR` | fatal read/write error — protocol equivalent of ZABORT |
| 13 | `ZCRC` | receiver → sender: send CRC of first `ZP0..ZP3` bytes; sender → receiver: the CRC value |
| 14 | `ZCHALLENGE` | receiver → sender: echo `ZP0..ZP3` back in a ZACK |
| 15 | `ZCOMPL` | command completed (`ZP0..ZP3` = exit status) |
| 16 | `ZCAN` | pseudo-type: returned by the header reader after a CAN*5 abort sequence |
| 17 | `ZFREECNT` | sender → receiver: reply with free bytes in a ZACK (`ZP0..ZP3`; 0 = unlimited) |
| 18 | `ZCOMMAND` | command download; `ZF0` = 0 or `ZCACK1` (1) |
| 19 | `ZSTDERR` | output to standard error, data follows |

Unused header bytes must be 0. `ZMODEM.H` defines `FRTYPES 22` = number of
non-negative entries in its name table (the table also holds 3 negative
pseudo-entries: `-3` Carrier Lost, `-2` TIMEOUT, `-1` ERROR).

### Header byte positions

| Symbol | Index into the 4-byte header | Role |
|--------|------------------------------|------|
| `ZF0` | 3 | flags, least significant byte |
| `ZF1` | 2 | |
| `ZF2` | 1 | |
| `ZF3` | 0 | flags, most significant byte |
| `ZP0` | 0 | file position, least significant byte |
| `ZP1` | 1 | |
| `ZP2` | 2 | |
| `ZP3` | 3 | file position, most significant byte |

The 4 data bytes are transmitted **in array order** `hdr[0] hdr[1] hdr[2] hdr[3]`,
i.e. `P0 P1 P2 P3` (LSB first) / `F3 F2 F1 F0`. File positions are 32-bit
little-endian (`pos = P0 | P1<<8 | P2<<16 | P3<<24`).

### ZRINIT / ZSINIT capability flags (all in `ZF0` unless stated)

| Flag | Oct | Hex | Meaning |
|------|-----|-----|---------|
| `CANFDX` | 01 | 0x01 | receiver can send *and* receive (true full duplex) |
| `CANOVIO` | 02 | 0x02 | receiver can write to disk while receiving |
| `CANBRK` | 04 | 0x04 | receiver can send a break |
| `CANCRY` | 010 | 0x08 | receiver can decrypt |
| `CANLZW` | 020 | 0x10 | receiver can uncompress |
| `CANFC32` | 040 | 0x20 | receiver can use 32-bit frame check sequence |
| `ESCCTL` / `TESCCTL` | 0100 | 0x40 | control characters are escaped (same bit both directions) |
| `ESC8` / `TESC8` | 0200 | 0x80 | 8th bit is escaped |

> Warning: the spec's chapter 11 prints these as `8`, `01`, `02`, `04`, `010`,
> `020`, `040`, `0100`, `0200` — the first four are decimal, the rest octal.
> `CANFC32` is **0x20**, not 0x40. `ZMODEM.H` is authoritative.

### ZFILE option bytes

| Byte | Value | Name | Meaning |
|------|-------|------|---------|
| `ZF0` | 1 | `ZCBIN` | binary transfer, inhibit all conversion (overrides everything except `ZCRESUM`) |
| | 2 | `ZCNL` | convert NL to the local end-of-line convention |
| | 3 | `ZCRESUM` | resume interrupted transfer |
| `ZF1` | 0x80 | `ZMSKNOLOC` | skip file if not present at receiver (OR-able) |
| | mask 0x1F | `ZMMASK` | low 5 bits select one of: `ZMNEWL` 1, `ZMCRC` 2, `ZMAPND` 3, `ZMCLOB` 4, `ZMNEW` 5, `ZMDIFF` 6, `ZMPROT` 7 |
| `ZF2` | 1 | `ZTLZW` | Lempel-Ziv compression |
| | 2 | `ZTCRYPT` | encryption |
| | 3 | `ZTRLE` | run-length encoding |
| `ZF3` | 64 (decimal) | `ZXSPARS` | sparse-file / selective-retransmission encoding (bit) |

`ZATTNLEN` = 32 (max Attn string length *including* the terminating NUL).

---

## 2. Header formats, exactly

All three forms carry the same payload: **1 type byte + 4 data bytes**.

### 2.1 Binary header, 16-bit FCS

```
ZPAD  ZDLE  ZBIN  TYPE  F3/P0  F2/P1  F1/P2  F0/P3  CRC-1  CRC-2
 *    0x18    A                                    (hi)   (lo)
```
* `TYPE`, the four data bytes and the two CRC bytes are **all ZDLE-escaped**
  (`ZPAD` and the `ZDLE ZBIN` pair are sent raw).
* CRC-16 covers `TYPE` + the 4 data bytes only. It does **not** cover `ZBIN`,
  `ZDLE` or `ZPAD`.
* `CRC-1` is the **high** byte, `CRC-2` the low byte (big-endian).

### 2.2 Binary header, 32-bit FCS

```
ZPAD  ZDLE  ZBIN32  TYPE  F3/P0  F2/P1  F1/P2  F0/P3  CRC-1 CRC-2 CRC-3 CRC-4
 *    0x18     C                                              (LSB)      (MSB)
```
* Same coverage (`TYPE` + 4 data bytes), but CRC-32 and **4** CRC bytes,
  transmitted **least significant byte first**, each ZDLE-escaped.
* May only be used if the receiver set `CANFC32` in its ZRINIT. If used for a
  header, the following data subpackets must also use 32-bit FCS.

### 2.3 HEX header

```
ZPAD  ZPAD  ZDLE  ZHEX  TYPE  F3/P0  F2/P1  F1/P2  F0/P3  CRC-1  CRC-2  CR  LF  XON
 *     *    0x18    B    hh    hh     hh     hh     hh     hh     hh
```
* Each of `TYPE`, the 4 data bytes and the 2 CRC bytes is sent as **two
  lowercase hex digits** (`0123456789abcdef` only — **upper case is forbidden**,
  it false-triggers XMODEM/YMODEM detection).
* CRC is always CRC-16, big-endian, over the same 5 bytes. **A 32-bit-CRC hex
  header is not defined.** `zshhdr()` forces the 16-bit path.
* Always followed by `CR LF`.
* `XON` (0x11) is appended **except** after `ZACK` and after `ZFIN`.
  Reason: XON un-corks a sender that was stopped by a noise-generated XOFF;
  after ZACK it would break flow control in streaming mode, and after ZFIN it
  would disturb clean session teardown.
* Receiver accepts 1..N `ZPAD` bytes before `ZDLE` (a run of `ZPAD`s is
  skipped), and also accepts `ZPAD|0x80`.

### 2.4 "No CRC" / zero-CRC cases

There is **no header format without a CRC** in the spec or in `rzsz`. The
frequently-quoted magic prologue `**\x18B00` is simply the beginning of a
normal hex `ZRQINIT`, whose CRC happens to be exactly `0000`:

```
**\x18B 00 00000000 0000 \r\n \x11
  |      |  |        |    |    └ XON
  |      |  |        |    └ CRC-16 over 00 00 00 00 00  = 0x0000  ← genuinely zero
  |      |  |        └ ZP0..ZP3 = 0
  |      |  └ type 0 = ZRQINIT
  |      └ ZHEX
  └ ZPAD ZPAD ZDLE
```
Full literal byte string: `72 7a 0d` (`"rz\r"`) then
`2a 2a 18 42` + 14 × `30` + `0d 0a 11`
(i.e. `**\x18B` + `00` + `00000000` + `0000` + CR LF XON).
Auto-download detectors scan for `**\x18B00` (or just `\x18B00`) — see §9.

Note that a *single* `ZPAD ZDLE` is also accepted by the receiver; only the
sender is specified to emit the extra `ZPAD` (it lets a program already
scanning the stream notice that an asynchronous/error header is arriving).

---

## 3. CRC algorithms

### 3.1 CRC-16 (used by `ZBIN`, `ZHEX`, and by 16-bit data subpackets)

| Property | Value |
|----------|-------|
| Polynomial | `x^16 + x^12 + x^5 + 1` = **0x1021** (CCITT / XMODEM) |
| Initial value | **0x0000** |
| Reflection | **none** (MSB-first, non-reflected in, non-reflected out) |
| Final XOR | **none** |
| Trailing | **two zero bytes are fed through the register after the data** |
| Transmit order | **high byte first** (big-endian) |
| Table | 256 × `uint16_t`, first entries `0000 1021 2042 3063 4084 50a5 …` |
| Standard name | equivalent to **CRC-16/XMODEM** (init 0, no reflection, no xorout) |

Reference implementation (this is exactly what `rzsz` does):

```c
/* table-driven, poly 0x1021, MSB-first */
#define updcrc16(cp, crc) \
    (crc16tab[((crc) >> 8) & 0xFF] ^ (((crc) << 8) & 0xFFFF) ^ (cp))

uint16_t crc = 0;
for (each byte b) crc = updcrc16(b, crc);
crc = updcrc16(0, updcrc16(0, crc));   /* the two trailing zero bytes */
/* transmit: high byte, then low byte */
```

The two trailing zero bytes are part of the algorithm: `CRC-16/XMODEM` is
defined that way, so a stock `crc16_xmodem()` implementation needs no special
handling. Do **not** use `CRC-16/CCITT-FALSE` (that is init `0xFFFF`) and do
**not** use `CRC-16/KERMIT`/`ARC` (those are reflected).

Known-answer tests:

| Input | CRC-16 |
|-------|--------|
| `"123456789"` (9 bytes) | **0x31C3** |
| `00 00 00 00 00` (ZRQINIT payload) | **0x0000** |
| empty | 0x0000 |

### 3.2 CRC-32 (used by `ZBIN32` and 32-bit data subpackets)

| Property | Value |
|----------|-------|
| Polynomial | `x^32+x^26+x^23+x^22+x^16+x^12+x^11+x^10+x^8+x^7+x^5+x^4+x^2+x^1+1` = **0x04C11DB7** |
| Table polynomial | **0xEDB88320** (the reflected form) |
| Initial value | **0xFFFFFFFF** |
| Reflection | **reflected** (LSB-first in and out) |
| Final XOR | **0xFFFFFFFF** (i.e. the value is complemented before transmission) |
| Transmit order | **low byte first** (little-endian) |
| Standard name | **CRC-32/ISO-HDLC**, i.e. exactly `zlib.crc32()` |
| Receiver residual | after processing data + frameend + the 4 transmitted CRC bytes with init `0xFFFFFFFF` and **no** final XOR, the register must equal **`0xDEBB20E3`** |

```c
uint32_t crc = 0xFFFFFFFF;
for (each byte b) crc = crc32tab[(crc ^ b) & 0xFF] ^ (crc >> 8);
crc = ~crc;                       /* final XOR */
/* transmit: (crc) & 0xFF, (crc>>8) & 0xFF, (crc>>16) & 0xFF, (crc>>24) & 0xFF */
```

Known-answer tests:

| Input | CRC-32 | transmitted bytes (LSB first) |
|-------|--------|-------------------------------|
| `"123456789"` | **0xCBF43926** | `26 39 F4 CB` |
| `"abc"` | **0x352441C2** | `C2 41 24 35` |
| `"abc"` + its 4 CRC bytes, register without final XOR | **0xDEBB20E3** | — |

`crc32tab[1] = 0x77073096`, `crc32tab[0] = 0x00000000`.

---

## 4. ZDLE escaping rules

Applied to **every byte of a binary header** (type + 4 data + CRC) and to
**every byte of a binary data subpacket** (data + the 2/4 CRC bytes). The
`ZPAD ZDLE` prefix, the frame indicator (`ZBIN`/`ZBIN32`), and the `ZDLE`
that introduces the subpacket terminator are sent **raw**. Hex headers are
never escaped (they are already printable).

Decision procedure for byte `c`, where `lastsent` is the previously
transmitted byte (before escaping):

```
if (c & 0x60) != 0            -> send c raw                 (0x20..0x7F, 0xA0..0xFF)
else if c == 0x18  (ZDLE)     -> send ZDLE, 0x58            (c ^ 0x40)
else if c in {0x10,0x11,0x13,0x90,0x91,0x93}
                              -> send ZDLE, c ^ 0x40        (DLE,XON,XOFF + 8-bit forms)
else if c in {0x0D,0x8D}      -> if (lastsent & 0x7F) == 0x40 ('@')
                                   send ZDLE, c ^ 0x40      (protects Telenet CR-@-CR)
                                 else send c raw
else                          -> send c raw  (or, when ESCCTL/ESC8 mode is on:
                                              send ZDLE, c ^ 0x40)
```

So the always-escaped set is exactly: **`ZDLE`, `020`, `021`, `023`, `0220`,
`0221`, `0223`** (= 0x18, 0x10, 0x11, 0x13, 0x90, 0x91, 0x93), plus
**`015`/`0215` (0x0D/0x8D) only when the preceding byte was `@`** (0x40 or 0xC0).

Escaped form is always `ZDLE` followed by `c ^ 0x40`.

Examples:

| Original | On the wire |
|----------|-------------|
| `0x18` (ZDLE) | `18 58` |
| `0x11` (XON) | `18 51` |
| `0x13` (XOFF) | `18 53` |
| `0x10` (DLE) | `18 50` |
| `0x0D` after `A` | `0D` (raw) |
| `0x0D` after `@` | `18 4D` |
| `0x7F` | `7F` (raw — has bit 6 set) |
| `0xFF` | `FF` (raw) |

### 4.1 Decoding

```
read c
if (c & 0x60) return c                        /* raw */
if c == 0x18:  /* ZDLE follows */
    read d
    if d == CAN and the next 4 bytes are also CAN -> ABORT (GOTCAN)
    if d in {0x68,0x69,0x6A,0x6B} -> frameend, return it
    if d == 0x6C (ZRUB0) -> return 0x7F
    if d == 0x6D (ZRUB1) -> return 0xFF
    if d in {0x11,0x91,0x13,0x93} -> discard, read again
    if (d & 0x60) == 0x40 -> return d ^ 0x40    /* 0x40..0x5F, 0xC0..0xDF */
    otherwise -> bad escape sequence (error)
else if c in {0x11,0x91,0x13,0x93} -> discard (stray XON/XOFF), read again
else return c
```

**Why `ZRUB0`/`ZRUB1` exist:** `0x7F ^ 0x40 = 0x3F` and `0xFF ^ 0x40 = 0xBF`
fall outside the decodable `0x40..0x5F`/`0xC0..0xDF` window, so those two bytes
cannot be escaped by the ordinary rule and get dedicated escapes. `rzsz`'s
transmitter never emits them (it sends `0x7F`/`0xFF` raw), but a conforming
receiver **must** decode them.

### 4.2 Interaction with XON/XOFF flow control

* `0x11`/`0x13` are escaped inside binary data, so a real XON/XOFF seen by the
  receiver between frames is unambiguously flow control.
* The receiver's byte reader **discards** stray `XON`/`XOFF` (and `0x91`/`0x93`)
  when looking for a header (it also strips the 8th bit while doing so).
* The sender appends `XON` after every hex header except `ZACK`/`ZFIN`, and
  sends `XON` + flush after every `ZCRCW` data subpacket.
* The receiver sends `XON` after the `ZACK` that answers a `ZCRCW`.
* A noise-generated XOFF can stall a sender; the receiver's 10 s timeout and
  its Attn sequence (which ends with XON in `rzsz`) recover from it.

---

## 5. Data subpackets and their terminators

Layout: `0..1024` data bytes, then `ZDLE <frameend>`, then 2 (16-bit FCS) or
4 (32-bit FCS) escaped CRC bytes. **The CRC accumulates the data bytes and the
frameend byte** (but not the `ZDLE`). No padding is used.

Recommended subpacket sizes: 256 bytes below 2400 bps, 512 at 2400 bps,
1024 above 4800 bps or on a clean link.

| Terminator | Hex | Meaning | Sender uses it when | Receiver must |
|------------|-----|---------|---------------------|---------------|
| `ZCRCG` | 0x69 | CRC next, frame **continues** | normal streaming, more subpackets immediately follow | append data, send **nothing** |
| `ZCRCQ` | 0x6A | CRC next, continues, **ZACK expected** | sender is managing a sliding window (`Txwindow`/`Txwspac`) and wants an offset sample | append data, send `ZACK` with the new offset (or `ZRPOS` on error), then keep reading subpackets |
| `ZCRCW` | 0x6B | CRC next, **ZACK expected**, end of frame | receiver lacks `CANOVIO`, or a buffer/window limit was hit, or the link is suspect, or after a resend | append data, flush to disk, send `ZACK` with the new offset, then wait for a header. Sender sends `XON` + flushes after it |
| `ZCRCE` | 0x68 | CRC next, frame **ends**, header follows | last subpacket of a file (EOF hit inside the frame), or aborting a streaming burst | append data, send **nothing**, then read the next header |

A zero-length data frame may be used as an idle subpacket. A zero-length
`ZCRCE` subpacket is the standard "stop the bubble machine" reaction when the
sender samples a `ZPAD`/`CAN` in the reverse stream: it terminates the burst
without provoking the ping-pong a `ZCRCW` would (important on high-latency
links, where a `ZCRCW` could allow several frames to already be in flight).

---

## 6. The ZFILE subpacket, byte for byte

Sent as the payload of a `ZCRCW` data subpacket immediately after the `ZFILE`
header. Structure:

```
<pathname> NUL <length> SP <moddate> [SP <mode> [SP <serial>
                                   [SP <files-remaining> [SP <bytes-remaining>
                                   [SP <file-type>]]]]] NUL
```

| Field | Encoding | Notes |
|-------|----------|-------|
| pathname | NUL-terminated ASCII, **no spaces** | mandatory. `/` as separator; `subdir\foo` is not acceptable. Only the file-name stem is normally sent (no directory) unless the sender's "full path" option is on |
| length | ASCII **decimal**, number of data bytes | optional; estimate only |
| moddate | ASCII **octal**, seconds since 1970-01-01 UTC | optional; `0` = unknown, receiver keeps its own timestamp |
| mode | ASCII **octal** | optional; `0` unless the file came from a Unix-like system. `rz` tests the `S_IFREG` bit (`0100000`); `rzsz` actually tests `st_mode & S_IFMT` (`0xF000`) |
| serial | ASCII **octal** | optional; transmitting program's serial number, `0` if none |
| files-remaining | ASCII **decimal**, **includes the current file** | optional, estimate |
| bytes-remaining | ASCII **decimal**, **includes the current file** | optional, estimate |
| file-type | decimal; `0` = sequential | optional |

Rules:
* **Exactly one space** separates each numeric field from the previous one. The
  first numeric field follows the filename NUL with **no** leading space.
* The whole thing is terminated by a **NUL**, and the subpacket is closed with
  **`ZCRCW`**. Total subpacket length **including** the trailing NUL must not
  exceed **1024 bytes**; a typical value is < 64.
* Fields may not be skipped (if `mode` is present, `length` and `moddate` must be too).
* A receiver should only require the first three fields: `rzsz` parses with
  `sscanf(p, "%ld%lo%o", &size, &mtime, &mode)` and ignores the rest.

### Worked example

`foo.txt`, 1234 bytes, mtime `1000000000` decimal (`0o7346545000` = 2001-09-09
01:46:40 UTC), mode `0100644`, serial 0, 1 file left, 1234 bytes left:

```
66 6f 6f 2e 74 78 74 00                     "foo.txt" NUL
31 32 33 34                                  "1234"                length
20                                           " "
37 33 34 36 35 34 35 30 30 30                "7346545000"          moddate (octal)
20                                           " "
31 30 30 36 34 34                            "100644"              mode    (octal)
20 30                                        " 0"                  serial
20 31                                        " 1"                  files remaining
20 31 32 33 34                               " 1234"               bytes remaining
00                                           NUL terminator
```

ASCII form of the whole 40-byte subpacket:

```
"foo.txt\0" "1234 7346545000 100644 0 1 1234\0"
```

On the wire the subpacket is ZDLE-escaped (nothing in this example needs
escaping) and closed with `ZDLE 'k'` plus the CRC:

```
66 6f 6f 2e 74 78 74 00 31 32 33 34 20 37 33 34 36 35 34 35 30 30 30 20
31 30 30 36 34 34 20 30 20 31 20 31 32 33 34 00 18 6b 52 09
                                                       ^^^^^ ^^^^^
                                                       ZDLE k  CRC-16 = 0x5209 (hi,lo)
```

(CRC-16 = 0x5209 is the value over the 40 subpacket bytes plus the frameend
byte `0x6B`.)

---

## 7. Receive-side state machine

### 7.1 Idle / startup

| Step | Receiver sends | Notes |
|------|----------------|-------|
| start | `ZRINIT` (hex, flags, buffer size) | send immediately, then **resend every `Rxtimeout` = 10 s**, up to ~15 attempts (spec: "40 seconds total"), then fall back to YMODEM |
| see `ZRQINIT` | resend `ZRINIT` | also resend when the sender's `ZRQINIT` is an echo of our own — receiving a `ZRINIT` from the peer means the peer is not running the protocol |
| see `ZCHALLENGE` | `ZACK` with the same 4 bytes | optional security check |

### 7.2 Per-frame reactions

| Incoming | Receiver does |
|----------|---------------|
| `ZSINIT` | read the following subpacket (must be `ZCRCW`); store the Attn string; set `Zctlesc` from `TESCCTL` in `ZF0`; reply `ZACK` (serial `1` in `ZP0..ZP3`). If the subpacket is not `ZCRCW` → `ZNAK` |
| `ZFILE` | read the `ZCRCW` subpacket; parse the pathname/fields; if not `ZCRCW` → `ZNAK`. Then reply `ZRPOS` with the start offset (`0`, or the existing file length for resume), **or** `ZSKIP` (user said no / file exists and no clobber), **or** `ZCRC` with a byte count to ask the sender for a file CRC |
| `ZRPOS` (from sender) | not valid at a receiver; treat as protocol error |
| `ZDATA` | check `ZP0..ZP3` equals the number of bytes already stored. If **not**: emit the Attn sequence (or just ignore), and go back to reading a header (the sender will be told the right offset by our `ZRPOS`). If yes: read subpackets |
| `ZEOF` | if `ZP0..ZP3` equals the byte count actually received → close the file and reply `ZRINIT`; if it does **not** match, ignore it (a fresh `ZDATA` is coming) and keep reading. If the file cannot be closed → `ZFERR` |
| `ZFIN` | reply with our own `ZFIN` (hex, **no XON**), then wait briefly for the `"OO"` bytes and exit whether or not they arrive |
| `ZABORT` | not sent by the receiver; a receiver sends it to abort a batch |
| `ZCOMPL` | command completed |
| `ZNAK` | the peer says our last header was garbled: resend it |
| `ZCAN` / `CAN*5` | abort the session |
| `ZFERR` | abort the session |
| `ERROR` (bad CRC) | the header reader returns ERROR; emit the Attn sequence and keep reading headers, up to a retry budget (rzsz allows 20 consecutive errors before giving up) |

### 7.3 Reading data subpackets (inside `ZDATA`)

| Terminator seen | Action |
|-----------------|--------|
| `ZCRCG` | append, add to offset, immediately read the next subpacket (no response) |
| `ZCRCQ` | append, add to offset, send `ZACK` (offset), immediately read the next subpacket |
| `ZCRCW` | append, flush to disk, add to offset, send `ZACK` (offset), send `XON`, then read a header |
| `ZCRCE` | append, add to offset, read a header |
| CRC error | emit the Attn sequence (interrupts the sender), then read the header the sender sends back (normally `ZRPOS` with our last good offset); do **not** write the bad bytes |
| timeout | emit the Attn sequence, keep waiting |

On any error the receiver resets its 20-error budget whenever a good
subpacket is received.

### 7.4 Abort

The Cancel sequence is **5 CANs minimum**; `rzsz` and the spec's sample send
**10 × CAN (0x18) followed by 10 × BS (0x08)**:

```c
static char canistr[] = {
  24,24,24,24,24,24,24,24,24,24,8,8,8,8,8,8,8,8,8,8,0
};
```
The backspaces try to erase the CANs if a shell or command interpreter sees
them. The header reader counts consecutive CANs and returns `ZCAN` as soon as
it has seen 5; a CAN immediately followed by `ZCRCW` is treated as an error
(not an abort). `rzsz` also sends the Attn string before the CANs when it is
aborting out of a streaming transfer.

---

## 8. Send-side state machine

### 8.1 Startup

| Step | Sender sends | Notes |
|------|--------------|-------|
| invoke receiver | `"rz\r"` (plain ASCII) | optional; wakes a command-mode receiver |
| invite | `ZRQINIT` (hex) with all-zero data | prologue is `**\x18B00…0000` — see §2.4. May be repeated; `rzsz` sends it once and then retries `ZNAK`-style inside its wait loop |
| wait | — | `Rxtimeout` for the sender is **60 s** (spec: abort if no header for about a minute) |

### 8.2 Reactions to incoming headers

| Incoming | Sender does |
|----------|-------------|
| `ZRINIT` | record `Rxflags = ZF0`, `Rxbuflen = ZP0 \| ZP1<<8`, `Txfcs32 = want32 && (Rxflags & CANFC32)`, `Zctlesc \|= Rxflags & ESCCTL`. If `!(Rxflags & CANFDX)` → `Txwindow = 0` (no sliding window: must use `ZCRCW` framing). Then optionally `ZSINIT`, then `ZFILE` |
| `ZCHALLENGE` | `ZACK` with the same 4 bytes in `ZP0..ZP3` |
| `ZCOMMAND` | the peer did not see our `ZRQINIT`: resend `ZRQINIT` |
| `ZRPOS` (after ZFILE) | seek to that offset, start a `ZDATA` at it, and make the **first** subpacket a `ZCRCW` (so the network is flushed before the burst) |
| `ZRPOS` (mid-stream) | purge output buffers, seek, restart the `ZDATA` at that offset, first subpacket `ZCRCW`. After 5 identical resync requests, halve the subpacket length |
| `ZACK` | mid-stream: update the "receiver has this offset" watermark and continue. After a `ZCRCW`: proceed. After `ZEOF`: re-send `ZEOF` |
| `ZSKIP` | close the file, go to the next file |
| `ZCRC` | compute CRC-32 over `ZP0..ZP3` bytes of the file and send it back in a `ZCRC` header (complemented, i.e. the `~crc` value) |
| `ZNAK` | the receiver says our last header was garbled — resend it |
| `ZCAN` / `ZABORT` / `ZFIN` / `TIMEOUT` | abort |
| `ZRINIT` while streaming | the peer lost its mind (echo); ignore/continue |

### 8.3 Choosing the subpacket terminator

```
if EOF reached inside this frame      -> ZCRCE
else if junk counter > 3              -> ZCRCW   (link is noisy: get an ack)
else if offset == last sync point     -> ZCRCW
else if receiver buffer window empty  -> ZCRCW   (respect Rxbuflen / CANOVIO)
else if sliding-window spacing hit    -> ZCRCQ   (sample the reverse channel)
else                                  -> ZCRCG   (pure streaming)
```

After `ZCRCW` the sender **waits** for a header. After `ZCRCQ` it keeps
sending but periodically reads a header from its interrupt buffer. While
streaming with `ZCRCG`, after each subpacket it samples the reverse channel
(without blocking) for `ZPAD` or `CAN`: on `ZPAD` it sends an empty `ZCRCE`
subpacket and then reads the error header; on `CAN` it assumes the user is
aborting.

### 8.4 End of file / end of session

```
send ZEOF(offset)         ; then loop:
   ZACK   -> send ZEOF again
   ZRPOS  -> resume data at that offset
   ZRINIT -> file accepted, go to next file
   ZSKIP  -> file skipped, go to next file
   other  -> error
```

When all files are done, the sender sends `ZFIN`, waits for the receiver's
`ZFIN`, then sends the two literal characters `"OO"` (Over and Out) and exits.
On `ZCAN` or timeout it exits without the `"OO"`.

### 8.5 16-bit vs 32-bit CRC negotiation

* The receiver advertises `CANFC32` (0x20) in `ZRINIT.ZF0`. Nothing else is
  negotiated.
* The sender may then set its `Txfcs32` flag and use `ZBIN32` headers and
  32-bit data subpackets. If `CANFC32` is absent the sender **must** use 16-bit
  FCS everywhere.
* The choice is sticky for the whole session: once a binary header used
  `ZBIN32`, the following data subpackets use 32-bit FCS, and vice versa.
* Hex headers are **always** 16-bit CRC regardless.

---

## 9. Initial handshake quirks (what you will actually see on the wire)

1. **Sender prologue.** `sz` writes the literal `"rz\r"`, then the hex
   `ZRQINIT`. Because the payload is `00 00 00 00 00`, its CRC-16 is `0000`, so
   the on-wire bytes are:

   ```
   72 7a 0d                                  "rz\r"
   2a 2a 18 42                               * * ZDLE ZHEX
   30 30                                     type 00  (ZRQINIT)
   30 30 30 30 30 30 30 30                   ZP0..ZP3 = 0
   30 30 30 30                               CRC-16 = 0000
   0d 0a 11                                  CR LF XON
   ```
   This is why the widely quoted "ZMODEM start" detector is the 7-byte string
   `**\x18B00`. A receiver that is already running in ZMODEM mode does not need
   this string — it will simply get a `ZRQINIT` header.

2. **Receiver reply.** `rz` answers with a hex `ZRINIT`. With the flags `rzsz`
   uses by default (`CANFC32|CANFDX|CANOVIO` = `0x23`) and buffer size 0:

   ```
   2a 2a 18 42 30 31 30 30 30 30 30 30 32 33 62 65 35 30 0d 0a 11
   * *  ZDLE ZHEX  "01" "00" "00" "00" "23" "be50"  CR LF XON
                    type  ZF3  ZF2  ZF1  ZF0  CRC-16
   ```
   i.e. the printable form `**\x18B0100000023be50\r\n` + XON.

   If a receiver advertises **no** capabilities (`ZF0 = 0` — seen in the wild,
   and matching the `**\x18B0100000000…` shape), the header is
   `**\x18B0100000000aa51\r\n` + XON. That means: **16-bit CRC only**, no
   full-duplex streaming, no overlapped I/O — so the sender must use `ZCRCW`
   framing. Do not assume `CANFC32`.

   If the receiver has its "escape control characters" option on it also sets
   `ESCCTL` (0x40): `**\x18B0100000063f694\r\n`.

   Other examples: `ZFIN` = `**\x18B0800000000022d\r\n` (**no XON**),
   `ZACK` = `**\x18B0300000000eed2\r\n` (**no XON**),
   `ZRPOS` at offset 1234 = `**\x18B09d204000031d7\r\n`.

3. **The `"rz waiting to receive."` banner** is *not* produced by the original
   `rzsz`; it belongs to another (GPL) implementation which could not be
   consulted. The original prints `"rz: ready to receive <name>"` only in
   XMODEM mode and, in ZMODEM mode, prints nothing except the `ZRINIT` hex
   header itself. Treat the visible banner as implementation-specific and
   **always key the protocol off the `**\x18B01`/`**\x18B00` header bytes**,
   not off any human-readable text.

4. **Timeouts and retries.**
   * Receiver: `Rxtimeout = 100` tenths = **10 s** per attempt, up to ~15
     `ZRINIT` attempts (spec: "resends its header at response time (default 10
     second) intervals for a suitable period of time (40 seconds total) before
     falling back to YMODEM protocol"). It also sends an `XON` at the end of
     the timeout/Attn sequence to release a sender stopped by a bogus XOFF.
   * Sender: waits up to **60 s** for any header before aborting.
   * Garbage limits: the receiver ignores up to `Zrwindow + baudrate` bytes
     (`Zrwindow` default 1400) of non-header garbage before declaring failure.

5. **CRC width agreement** is solely `CANFC32` in `ZRINIT.ZF0`. A receiver that
   wants 32-bit FCS must set it; a sender that supports it should honour it
   (larger CPU cost, ~4 orders of magnitude better undetected-error rate).

---

## 10. Timing / robustness rules that matter in a real implementation

| Rule | Detail |
|------|--------|
| Padding | binary header: one `ZPAD`; hex header: two `ZPAD`. Receiver must tolerate a run of `ZPAD`s and a single `ZPAD`. `ZPAD` before a header also serves as the "asynchronous header" flag the sender samples for |
| Retry budget | receiver tolerates 20 consecutive header/subpacket errors before aborting; sender retries `ZFILE`/`ZSINIT` up to ~20 times; `ZRQINIT`→`ZRINIT` up to ~10–15 times |
| Bad CRC | the header reader returns ERROR → the receiver emits the Attn sequence (and, for data, sends nothing of the bad subpacket) and waits for a header; the sender responds to a bad header by sending `ZNAK` and repeating |
| NAK | `ZNAK` means "your last header was garbled"; the receiver sends `ZNAK` when a `ZFILE`/`ZSINIT`/`ZCOMMAND` subpacket was not terminated by `ZCRCW` or failed |
| `ZPAD` in the reverse stream | the sender samples the reverse channel after each `ZCRCG`/`ZCRCQ` subpacket. A `ZPAD` means "the receiver has an error header for you": the sender sends an **empty `ZCRCE` subpacket** (not a `ZCRCW` — a `ZCRCW` would provoke an `ZACK` and can cause multiple in-flight frames on a long-delay link) and then reads the header |
| `CAN` in the reverse stream | the user is hitting Ctrl-X: same handling as `ZPAD` (empty `ZCRCE`, then read the header) |
| Attn sequence | receiver → sender, used to interrupt a streaming sender. Default is **empty**; the sender may specify one in `ZSINIT` (NUL-terminated, ≤ 32 bytes). The `rzsz` default for SysV builds is `{ 03, 0336, 0 }` = Ctrl-C, then a 1-second pause. Two meta-characters are interpreted by the receiver when replaying the string: `\335` (0xDD) = send a break, `\336` (0xDE) = pause one second |
| Attn follow-up | after the Attn sequence the receiver sends a **hex `ZRPOS`** to force resumption at the right offset. The sender should purge its output buffers so the receiver discards as little data as possible, and the next data frame should be a `ZCRCW` |
| Cancel | 5 CANs is the threshold; 10 CANs + 10 backspaces is what is actually sent. A CAN immediately followed by `ZCRCW` is an error, not an abort |
| XON/XOFF | escaped inside data; `XON` appended to hex headers except `ZACK`/`ZFIN`; `XON` sent after each `ZCRCW` subpacket; receiver discards stray `XON`/`XOFF` while hunting for a header |
| `ZNULLS` | `sz` optionally prefixes a `ZDATA` header with N nulls (env var `ZNULLS`, default 0) to flush buffered modems. A receiver must ignore leading NULs before a header |
| Empty subpackets | a zero-length subpacket is legal (idle keep-alive, or the "stop the bubble machine" `ZCRCE`). Do not treat length 0 as an error |
| Buffer size | `ZRINIT.ZP0|ZP1<<8` is the receiver's buffer size; 0 means "non-stop I/O is fine". A non-zero value is an upper bound on how much the sender may send before it must wait for an ack (`ZCRCW`) |
| Subpacket length | never exceed 1024 bytes of data. Recommended: 256 (<2400 bps), 512 (=2400 bps), 1024 (>4800 bps or clean link) |
| File-info limit | the whole ZFILE subpacket including its trailing NUL must be ≤ 1024 bytes |
| Session end | sender sends `ZFIN`, receiver answers `ZFIN`, sender emits `"OO"`. Receiver waits briefly for `"OO"` and then exits regardless |

---

## 11. Open questions / ambiguity (where to be careful)

1. **`ZRINIT` retry count and total wait.** The spec says "40 seconds total"
   with 10 s intervals, while the `rzsz` code loops ~15 times. Not
   interoperably critical, but a sender must not give up faster than the
   receiver's first few retries.

2. **Which flags a receiver "should" advertise.** The spec lists `CANCRY`,
   `CANLZW`, `CANBRK` with no guidance on defaults; the spec's own chapter 11
   prints `CANCRY` twice (`8` and `010`). `ZMODEM.H` gives the canonical octal
   values in §1. `ZRINIT` flags are only a *hint*; a robust sender must cope
   with any combination, including 0.

3. **The `"rz waiting to receive."` string.** Not verifiable from a permissive
   source (it is a GPL-implementation banner). Do not depend on it.

4. **`ZCRC` semantics.** The spec says the receiver sends a byte count and the
   sender replies with "the complement of the CRC". `rzsz` computes
   `~crc32(bytes)` with init `0xFFFFFFFF` and puts it in `ZP0..ZP3` of a `ZCRC`
   header. It is not spelled out whether a byte count of 0 means "whole file";
   the spec's footnote only says the CRC is initialised to `0xFFFFFFFF`.

5. **`ZSINIT` "complete control character escaping" (`ESCCTL`/`ESC8`).** The
   spec says that if `ZSINIT` specifies `ESCCTL`/`ESC8` a HEX header is used and
   the receiver activates the modes before reading the following subpacket.
   The exact meaning of `ESC8` (escape *all* 8th-bit-set bytes) versus the
   normal rule (which only escapes 0x80–0x9F) is described only loosely; the
   `ZRUB0`/`ZRUB1` escapes exist to cover the two bytes that the `^0x40` rule
   cannot express.

6. **`ZF3`/`ZXSPARS` naming.** The spec text calls it `ZTSPARS` and `ZMSPARS` in
   different places; `ZMODEM.H` calls it `ZXSPARS`, value 64 decimal (0x40).
   Sparse-file support is described as "to be determined" and should be
   treated as out of scope.

7. **`ZCOMMAND` / `ZCOMPL` server mode and `ZTCRYPT`/`ZTLZW`/`ZTRLE`** are all
   declared "details to be determined" in the spec. Implementations in the
   field never exercise them; a receiver should refuse (or ignore) them.

8. **Text-file conversion (`ZCNL`) and resume (`ZCRESUM`/`ZMCRC`)** are
   specified at the level of intent, not byte layout. If you implement only
   `ZCBIN` + `ZMCLOB`/`ZMAPND` you will interoperate with `sz`/`rz` in
   practice, but a receiver that ignores `ZCNL` may silently store CR/LF
   variants of text files.

9. **Residual / byte order of the 16-bit CRC.** The spec never states the byte
   order explicitly; it is only visible in the `rzsz` code (`zsendline(crc>>8)`
   then `zsendline(crc)`), so it is stated here from source, not spec. Same for
   the little-endian transmission order of the CRC-32.
