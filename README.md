# `exarcia_hal_espidf` — LAYER 2, the ESP-IDF HAL

Implements the interface declared in `exarcia_ota/include/exarcia_hal.h`. Owns
every **mechanism**: HTTPS transport, NVS storage, the clock, SHA-256 via PSA,
and the OTA slot. Contains **no protocol logic** — no JSON, no endpoint paths, no
deployment state.

```
LAYER 3  main/                          app_main, connectivity, provisioning
LAYER 2  exarcia_hal_espidf   <-- this component
LAYER 1  exarcia_ota                   payloads, parsing, policy, state machine
```

## The invariant this component exists to preserve

**`stage_finish()` is the only call permitted to switch the boot slot.
`stage_abort()` must leave the running image selected and bootable.**

`esp_https_ota_finish()` is `esp_ota_end()` followed by
`esp_ota_set_boot_partition(staging)` — verified in `esp_https_ota.c:913-945`
at v6.0.1. That is what makes the split safe: LAYER 1 verifies size and SHA-256
*after* the download and *before* calling `stage_finish()`, so a corrupt image is
aborted rather than activated.

Three tests enforce this mechanically (`tests/test_exarcia_hal_espidf.py`):

- the HAL never calls `esp_ota_set_boot_partition()` directly;
- `esp_https_ota_finish()` is called from exactly one place;
- `stage_finish()` refuses a stage that never reached `EXARCIA_STAGE_FINISHED`,
  because `esp_ota_end()` would validate and activate a truncated app.

They were negative-tested: re-introducing a direct
`esp_ota_set_boot_partition()` call fails them.

## Why `esp_https_ota` only

It is the only path that keeps resumption across a reboot, Range/partial
downloads with TLS session resumption, staging partitions, and chip-ID /
revision / secure-version validation. The alternative — raw `esp_ota_ops`, which
the PlatformIO demo uses — loses all four, so no second strategy is
implemented.

**The cost:** `esp_https_ota_config_t` has no per-buffer callback (only
`decrypt_cb`, gated behind `CONFIG_ESP_HTTPS_OTA_DECRYPT_CB`), so the staged
image cannot be hashed while it is written. `stage_digest()` therefore reads the
target partition back with `esp_partition_read()` in 4 KB chunks and hashes that
with PSA.

`esp_partition_mmap()` would be faster, but `ESP_PARTITION_MMAP_DATA` is
documented as *"4 MB total — only for esp32"*, so mapping the other slot risks
colliding with the running image's mapped region on C3/S3/H2. Portability won.

## Two deliberate omissions

**No network bearer.** No `esp_wifi.h`, no `esp_eth.h`, no `esp_openthread.h`.
Connectivity belongs to the application, which must have L3 up before calling
into this component — the same separation the reference device agent keeps, and
the reason the library works over Ethernet, Thread or a modem unchanged. A test
enforces it.

**No identity headers on the artifact URL.** `stage_begin()` points
`esp_https_ota` at the presigned object-storage URL and attaches *nothing* but
TLS trust. Forwarding `X-PSK` there would leak the device credential into a
third party's access logs. LAYER 1 owns the identity headers and sends them only
to Exarcia Core; a test asserts the strings appear nowhere in this component.

## Resume offset hand-off

`stage_begin()` receives only a URL, so LAYER 1 writes the offset into
`stage->resume_offset` beforehand. It is used to configure
`ota_resumption` / `ota_image_bytes_written`. The stored URL is compared before
resuming, so a checkpoint can never be applied to a different artifact.

## Offline verification

`tests/stubs/esp_idf/` holds minimal declarations of every ESP-IDF API used
here, transcribed from esp-idf tag `v6.0.1`, so `make test-hal` type-checks this
component with a bare host compiler. It proves arity, types and warning-cleanliness
under `-std=c99 -pedantic -Wall -Wextra -Werror`.

It is **not** a substitute for `idf.py build`: there is no runtime behaviour
behind the stubs, and nothing here has been executed on hardware.

This harness paid for itself immediately — it caught
`sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG(...)`, which is invalid C because the macro
expands to a braced initializer list and is only valid in a declaration.

## Still to verify with the real toolchain

1. `idf.py build` — component requirements, `esp_app_desc_t` field names, PSA
   linkage against `mbedtls`.
2. `CONFIG_ESP_HTTPS_OTA_ENABLE_PARTIAL_DOWNLOAD` is only settable in
   `sdkconfig`, not Kconfig. Enabling `EXARCIA_HAL_ENABLE_PARTIAL_DOWNLOAD`
   without it logs a warning and falls back to a single request.
3. That `esp_partition_read()` on an un-erased staging slot returns the written
   bytes and not stale flash from a previous image. Sequential writes from offset
   0 should cover this, but it is worth confirming on a device.
4. Timing of `stage_digest()` on a real 1.7 MB image, to size the tick budget.