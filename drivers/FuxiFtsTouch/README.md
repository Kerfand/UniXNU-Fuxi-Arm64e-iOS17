# FuxiFtsTouch

ST FingerTip FTM5 touch controller protocol and contact tracking over an abstract SPI transport, written for the
Xiaomi 13 (fuxi, SM8550) panel. The controller keeps its firmware in its own flash, so the host only resets it, waits
for its ready event, starts sensing and drains its event FIFO.

| File | Contents |
| --- | --- |
| `FtsProtocol.c`, `FtsProtocol.h` | Framing, register reads and writes, FIFO reads, reset with the ready wait and the 4-wire SPI fallback, sensing start |
| `FtsTouchCore.c`, `FtsTouchCore.h` | Contact slots by controller tracking id, event classes, error recovery flags, board coordinate transform |
| `Test/fts-protocol.c` | Host test against a model of the controller's SPI side |
| `Test/touch-core.c` | Host test of the event decoding and contact tracking |

The code is pure C: no allocation, no libc, no board addresses. Callers provide the transport through `struct fts_bus`
(full-duplex transfer, millisecond delay, reset line). The protocol facts follow a study of the Xiaomi `fts_spi` driver
that the fuxi kernel loads; no code is copied from it.

The fuxi board probe, its constants and the device tree facts are in
[OddlyLabs/unixnu-fuxi](https://github.com/OddlyLabs/unixnu-fuxi). The catalog entry is in
[OddlyLabs/UniXNU](https://github.com/OddlyLabs/UniXNU) (`components.toml`, ID `FuxiFtsTouch`).

## Tests

```sh
for t in fts-protocol touch-core; do
  cc -std=c11 -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror -I. Test/$t.c FtsProtocol.c FtsTouchCore.c -o /tmp/$t && /tmp/$t
done
```

## License

BSD-2-Clause-Patent. See [LICENSE](LICENSE).
