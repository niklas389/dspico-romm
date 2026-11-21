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

## Building

The project needs BlocksDS with Mbed TLS, libcurl and zlib:

```bash
wf-pacman -Sy blocksds-toolchain toolchain-gcc-arm-none-eabi-zlib blocksds-mbedtls blocksds-libcurl
```

```bash
make
```

GitHub Actions builds `romm-poc.nds` on every push; see the workflow's
artifacts.

## Reporting results

After a run, send back `<SD>/romm/poc.log`. It contains the timings, TLS
details and errors. The token is never written to the log.
