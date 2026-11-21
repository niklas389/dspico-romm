# dspico-romm

A [RomM](https://github.com/rommapp/romm) client for the DSi, launched from the
DSPico / Pico Launcher. The planned features are browsing the library,
downloading ROMs (DS, DSi, GBA for GBARunner), managing ROMs on the SD card,
and syncing `.sav` files with RomM.

**Current state: proof of concept (`romm-poc.nds`).** It tests the risky parts
on real hardware before any real UI is built.

## What the PoC tests

| Step | What it measures |
| --- | --- |
| System | DSi mode active, CPU at 134 MHz, RTC time (the certificate check needs a correct clock) |
| Wi-Fi | WPA2 connection through DSWiFi in DSi mode, using the APs saved in System Settings |
| Heartbeat | TLS handshake time, TLS version and cipher, and connection reuse on a second request |
| Pairing | RomM 5.4 device pairing: a QR code on the top screen, approval on your phone, Bearer token saved to `token.txt` |
| Platforms / ROMs | Paginated `/api/roms` (10 per page): JSON size and parse time per page |
| Download | Throughput over the network only (X) and with SD writes (A), in KB/s |

## SD card setup

```
<SD>/romm/config.ini   # copy of sdroot/romm/config.ini.example, with your URL
<SD>/romm/ca.pem       # root CA certificate(s) in PEM format
```

The app looks for `romm/` first on the drive it was launched from, then on
`sd:/`, then on `fat:/`. It writes `poc.log`, `token.txt` and `test/` to the
same folder.

### ca.pem

Mbed TLS needs the root CA that signed your server certificate. To find which
one that is:

```bash
openssl s_client -connect romm.example.com:443 -servername romm.example.com </dev/null 2>/dev/null | openssl x509 -noout -issuer
```

Copy the matching root from `/etc/ssl/certs/` into `ca.pem`. For Let's Encrypt
RSA certificates this is `ISRG Root X1`. You can also use the full Mozilla
bundle (https://curl.se/ca/cacert.pem, about 220 KB). The log shows how long
the DSi takes to parse it.

### Wi-Fi

Save your WPA2 network in **DSi System Settings → Internet → Advanced Setup**
(connections 4–6). Connections 1–3 only support WEP or open networks.

## Speed test

Select a ROM of at least 8 MB and press **Y**. The suite measures:

- the round-trip time to the server (via TCP connect),
- the CPU speed of ChaCha20-Poly1305 and AES-128-GCM (the TLS cost),
- the SD write speed,
- download speed with 1, 2 and 4 parallel connections (HTTP Range requests,
  8 MB each, data discarded),
- optionally plain HTTP, if `http_url=` is set in `config.ini`.

- download speed with the body written to the SD card (like a real download),
- for every test, how many packets the ARM7 had to drop because the
  ARM7 -> ARM9 ring buffer was full ("lost").

There are three builds, differing only in DSWiFi settings:

| ROM | TCP window | ARM7 -> ARM9 RX buffer | Log file |
| --- | --- | --- | --- |
| `romm-poc.nds` | 5840 B (stock) | 12 KiB (stock) | `poc-stock-4x1460-rx12.log` |
| `romm-poc-wnd32.nds` | 46720 B | 12 KiB | `poc-32x1460-rx12.log` |
| `romm-poc-rx128.nds` | 46720 B | 128 KiB | `poc-32x1460-rx128.log` |

The patched builds also use a matching ARM7 core, because the RX buffer is
shared between both CPUs (see `tools/build_dswifi.sh` and `patches/`).

## Building

The project needs BlocksDS with Mbed TLS, libcurl and zlib:

```bash
wf-pacman -Sy blocksds-toolchain toolchain-gcc-arm-none-eabi-zlib blocksds-mbedtls blocksds-libcurl
```

```bash
make            # romm-poc.nds
```

```bash
make variants   # also the patched DSWiFi builds (see tools/build_dswifi.sh)
```

GitHub Actions builds `romm-poc.nds` on every push; see the workflow's
artifacts.

## Reporting results

After a run, send back `<SD>/romm/poc.log`. It contains the timings, TLS
details and errors. The token is never written to the log.
