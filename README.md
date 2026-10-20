# `exarcia_hal_espidf` — ESP-IDF integration

This is the ESP-IDF HAL for the platform-agnostic Exarcia update client. Install
this component when you want an ESP32 device to enroll with Exarcia Core, poll
for ESP-IDF firmware updates, receive per-device parameter deployments, and
report status, heartbeats, inventory and logs.

The protocol and state machine live in the companion component
[`exarcia_ota`](https://github.com/exarcia/exarcia-ota-mcu). Read that
component's README for the full device-API contract and the HAL interface.

This document is for the developer or agent adding the library to a real
ESP-IDF project.

---

## 1. Requirements

- ESP-IDF v5.3 or newer. The HAL uses
  `esp_https_ota_config_t.http_client_init_cb` and is developed against v5.3,
  v5.5 and v6.0.
- v0.3.0 or newer of both components. Version 0.3.0 adds the corrected staged
  version capture and the non-destructive NVS policy; LAYER 1 and LAYER 2 must
  be built from matching versions.
- An `exarcia` NVS partition in the partition table. The HAL deliberately does
  not store Exarcia state in the application's default `nvs` partition.
- Network connectivity already established by the application. The HAL does not
  own Wi-Fi, Ethernet, Thread or a modem.
- A valid clock before the first HTTPS request. Call
  `exarcia_hal_espidf_sync_clock()` or use your own SNTP/RTC setup.
- A device identity and PSK provisioned by your factory tooling, or seeded in
  firmware for bench testing.

## 2. Add the component to a project

Add `exarcia_hal_espidf` as a dependency in the project's
`main/idf_component.yml`, using the v0.3.0 release. The component manager pulls
`exarcia_ota` transitively, so you do not declare it separately.

For local development, declare both components with `override_path` in the same
file instead of the git dependency. Adjust the paths to your checkout. Do not
add both the git dependency and the local override for the same component.

The consuming component must require `exarcia_hal_espidf` from its
`CMakeLists.txt`. After changing dependencies, run `idf.py reconfigure`, then
`idf.py build`, then `idf.py -p PORT flash monitor`.

## 3. Partition table

Add a dedicated NVS partition. The default label is `exarcia`, selected by
`CONFIG_EXARCIA_HAL_NVS_PARTITION`. The partition row must use type `data` and
subtype `nvs`.

The reference 4 MB layout uses a 24 KB partition at offset `0x379000`. Adjust
the offset and size to your flash layout. The partition must hold factory
provisioning, the learned device ID, OTA bookkeeping and the parameter document.

Select the partition CSV in `idf.py menuconfig` under **Partition Table**, or set
`CONFIG_PARTITION_TABLE_CUSTOM` and `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` in
`sdkconfig`.

If the partition is missing, `exarcia_hal_espidf_init()` fails with
`ESP_ERR_NOT_FOUND` and logs the partition name. It does not silently fall back
to the application's `nvs` partition.

On an orderly shutdown call `exarcia_hal_espidf_deinit()`. It releases the NVS
and OTA handles; it does not change the boot slot.

The HAL never erases the Exarcia NVS partition automatically. If the partition
reports `ESP_ERR_NVS_NO_FREE_PAGES` or a newer NVS format, initialization fails
with a clear log instead of silently destroying the device UUID, PSK and pending
OTA record. Recovery is an explicit factory re-provisioning step.

## 4. Kconfig

The HAL exposes these options:

| Kconfig | Default | Purpose |
|---|---|---|
| `CONFIG_EXARCIA_HAL_NVS_PARTITION` | `exarcia` | NVS partition holding all Exarcia state |
| `CONFIG_EXARCIA_HAL_TRANSPORT` | `wifi` | Value of the `X-Transport` fleet-attribution header |
| `CONFIG_EXARCIA_HAL_HTTP_TIMEOUT_MS` | 15000 | Core API request timeout |
| `CONFIG_EXARCIA_HAL_OTA_TIMEOUT_MS` | 300000 | Firmware download timeout |
| `CONFIG_EXARCIA_HAL_OTA_BUFFER_SIZE` | 2048 | HTTP receive buffer |
| `CONFIG_EXARCIA_HAL_ENABLE_PARTIAL_DOWNLOAD` | n | Range-based partial downloads, not used for Core URLs |
| `CONFIG_EXARCIA_HAL_HTTP_REQUEST_SIZE` | 32768 | Bytes per Range request when partial download is enabled |
| `CONFIG_EXARCIA_HAL_ENABLE_RESUMPTION` | y | Resume a transfer from a presigned URL only |
| `CONFIG_EXARCIA_HAL_USE_CERT_BUNDLE` | y | Validate HTTPS with the ESP certificate bundle |
| `CONFIG_EXARCIA_HAL_TLS_SKIP_HOSTNAME` | n | Lab only: keep chain validation, skip hostname |

`CONFIG_EXARCIA_HAL_ENABLE_PARTIAL_DOWNLOAD` additionally requires
`CONFIG_ESP_HTTPS_OTA_ENABLE_PARTIAL_DOWNLOAD` in `sdkconfig`. Without it the HAL
logs a warning and uses one request. Keep partial download disabled for current
Core, which streams the whole payload and does not honour `Range`.

The transitive `exarcia_ota` component also exposes log, poll and retry tunables
(`CONFIG_EXARCIA_LOG_*`, `CONFIG_EXARCIA_POLL_INTERVAL_SECONDS`, and so on). See
its README; the application copies those values into
`exarcia_client_options_t`.

Select the SoC with the normal ESP-IDF target mechanism, for example
`idf.py set-target esp32s3`. The same value must be used for
`capabilities.platform` and must match the Core target and artifact platform.

For TLS, the default trusts the ESP certificate bundle. To use a private CA,
set `ca_pem` and `ca_present` in `exarcia_hal_espidf_config_t` before
`exarcia_hal_espidf_init()`. `tls_skip_hostname` is lab-only.

## 5. Provisioning

The client reads provisioning from the `exarcia_cfg` NVS namespace. The keys are
part of the factory contract and must match your manufacturing tooling:

| Key | Required | Meaning |
|---|---|---|
| `core_url` | yes | Core base URL, for example `https://core.example.com` |
| `identity` | yes | Device serial or MAC |
| `identity_type` | recommended | `serial` or `mac`; inferred from the identity when empty |
| `device_psk` | yes | Device pre-shared key |
| `org_slug` | optional | Organization slug |
| `batch_id` | optional | Provisioning batch |
| `prov_method` | optional | `individual`, `batch`, and so on |
| `device_id` | learned | Written after registration or authentication |

For production, your factory provisioning tool writes these keys. For a bench
board, fill the matching fields of `exarcia_device_config_t` before
`exarcia_ctx_init()`.

`exarcia_ctx_load_config()` gives stored factory values precedence over the
seed and persists non-empty seed values back to NVS, so a bench board converges
after the first boot. A device is usable when `exarcia_config_is_complete()`
returns true and Core is reachable.

## 6. Initialize and run

A typical application sequence:

1. Bring up Wi-Fi or Ethernet and obtain L3 connectivity.
2. Load the HAL defaults with `exarcia_hal_espidf_default_config()`, then call
   `exarcia_hal_espidf_init()` to populate the `exarcia_hal_t`.
3. Synchronize the clock with `exarcia_hal_espidf_sync_clock()`.
4. Initialize `exarcia_device_config_t` with `exarcia_config_init()` and either
   seed the provisioning values or rely on factory NVS.
5. Initialize `exarcia_capabilities_t` with `exarcia_capabilities_init()`, then
   set `platform`, `update_backend`, `update_adapter` and `transport`. The
   transport helper is `exarcia_hal_espidf_transport()`.
6. Initialize `exarcia_client_options_t` with `exarcia_client_default_options()`.
7. Allocate the caller-owned log ring. Use at least `EXARCIA_LOG_BATCH_MAX`
   entries; 32 is a reasonable size. Pass `NULL, 0` to disable log upload.
8. Call `exarcia_ctx_init()` with the HAL, device configuration, capabilities,
   options and log storage.
9. Call `exarcia_ctx_load_config()` and verify
   `exarcia_config_is_complete()`.
10. Optionally register `exarcia_ctx_set_parameters_applied()` and
    `exarcia_ctx_set_inventory()`.
11. Create one task that repeatedly calls `exarcia_ota_tick()` and then sleeps.

`exarcia_ctx_init()` rejects an incomplete HAL with
`EXARCIA_ERR_INVALID_ARG`, so `exarcia_hal_espidf_init()` must succeed first.

Rules for the task:

- Call `exarcia_ota_tick()` from exactly one task. The client is not
  thread-safe.
- The tick advances at most one state step, but that step may perform an HTTP
  request or the `esp_https_ota` transfer. Do not call it from a timer callback
  or an interrupt.
- A 500–1000 ms tick period is typical. Poll, heartbeat and log intervals are
  scheduled internally.
- Give the task a generous stack. The library builds status, heartbeat and JSON
  bodies on the stack and calls into `esp_https_ota`. Use 8 KB or more and tune
  it on your target.
- The firmware transfer blocks inside the OTA step. If the task watchdog is
  enabled, exclude the Exarcia task from it or allow enough time for the largest
  image on your slowest link.
- Do not call `esp_ota_mark_app_valid_cancel_rollback()` yourself. The client
  confirms the new image on the first successful boot after an update.
- Do not call `esp_restart()` for an update. The HAL owns the reboot after
  `stage_finish()`.

The first tick after a reset performs boot reconciliation: it reports a pending
deployment as `committing` and `succeeded` when the staged image is running, or
`rolled_back` when the bootloader reverted. Start the task early so a broken
image cannot sit unconfirmed.

## 7. Read parameter values

Parameter deployments are stored under `exarcia_params/params`. The payload is a
flat JSON object; a value may itself be a nested object or array. The client
merges each new deployment into the existing document: matching top-level
parameter keys are overwritten, keys not present keep their previous value.

Call `exarcia_params_load()` with the HAL and a buffer of
`EXARCIA_PARAMS_MAX` bytes. It returns `EXARCIA_ERR_NOT_FOUND` until the first
parameter deployment, which means “no parameters yet”, not a failure.

Use `exarcia_params_get_string()`, `exarcia_params_get_int64()`,
`exarcia_params_get_double()` or `exarcia_params_get_bool()` for typed values.
Use `exarcia_params_get_raw()` when the value is a nested object, array, or a
JSON string you want to parse yourself.

`exarcia_ctx_set_parameters_applied()` registers an optional callback. It runs in
the Exarcia task and receives the accepted payload, not the merged document;
call `exarcia_params_load()` for the full current set. Keep the callback short,
or hand work to another task.

The merged document must fit in `EXARCIA_PARAMS_MAX` (4000 bytes). A larger
result fails the deployment rather than truncating. Applying a deployment uses
bounded heap allocations for the existing and merged documents; leave enough
heap for that and the HTTP response body.

Parameter values can contain secrets such as MQTT passwords. They are stored in
NVS in plaintext unless the Exarcia partition uses NVS encryption.

The client never reboots for a parameter deployment and never changes the
installed firmware version or rollback index. Device-file deployments are not
supported; the client advertises only firmware and parameter modes.

## 8. Firmware update behavior

When Core offers a firmware artifact:

1. The client reports `downloading`.
2. The HAL stages the primary payload into the inactive slot through
   `esp_https_ota`.
3. The client verifies the byte count and SHA-256.
4. It reports `installing`, then `rebooting`.
5. `stage_finish()` activates the slot and the HAL restarts.
6. On the next boot the client reports `committing` and `succeeded`, then calls
   `mark_app_valid()`.

The ESP-IDF OTA recipe emits one payload (`ota-app.bin`). The MCU client installs
that primary payload; multi-payload install plans are outside this component.

Set `PROJECT_VER` (or a root `version.txt`) to your release version so the
running application version matches the artifact version. The client uses the
app descriptor for boot confirmation, but Core uses the reported version to
suppress duplicate offers. The value must fit `esp_app_desc_t.version`, i.e. at
most 31 visible characters; numeric `MAJOR.MINOR.PATCH` is recommended.

`stage_run()` captures the staged application version from the downloaded image
header, not from the inactive partition, and `get_staged_app_version()` returns
it. This matters on a reused A/B slot: reading the partition before the download
would return the previous image's version.

Firmware artifacts are currently expected to be unsigned. If signing is enabled
on Core, this client does not verify the envelope; do not enable signing for
devices using it.

## 9. Optional inventory and logs

Register an inventory callback with `exarcia_ctx_set_inventory()`. It writes a
JSON object into a bounded buffer. Return a negative value to omit inventory.
The callback runs during heartbeats, so keep it fast.

Queue application messages for Core's log UI with `exarcia_log_record()`.
`EXARCIA_LOG_REDACT` is on by default and scrubs known-sensitive log shapes.
Never log a download URL, a PSK or a parameter document.

## 10. Production checklist

- The partition table contains the `exarcia` NVS partition.
- `CONFIG_EXARCIA_HAL_NVS_PARTITION` matches the partition label.
- Factory tooling writes `core_url`, `identity`, `identity_type` and `device_psk`.
- The clock is synchronized before the first HTTPS call.
- `capabilities.platform` matches the Core target platform.
- `update_backend` and `update_adapter` match manifest compatibility fields when
  your artifacts declare them.
- `deployment_modes` includes `"parameters"` only if you want parameter
  deployments. `exarcia_capabilities_init()` enables both modes by default.
- The Exarcia task starts early and is the only caller of `exarcia_ota_tick()`.
- The application does not confirm boot slots or switch partitions itself.
- NVS encryption is enabled if parameter payloads contain secrets.
- `CONFIG_EXARCIA_HAL_TLS_SKIP_HOSTNAME` is disabled in production.
- The project builds with matching release tags of both components.

## 11. Troubleshooting

| Symptom | Check |
|---|---|
| `exarcia_hal_espidf_init()` returns `ESP_ERR_NOT_FOUND` | The `exarcia` NVS partition is missing or the Kconfig name is wrong |
| Device registers but never gets deployments | `capabilities.platform`, target membership, `update_backend`, and `deployment_modes` |
| All Core calls return 401 | Identity or PSK mismatch; a rejected credential triggers one re-registration, then halts |
| OTA fails at `esp_https_ota_begin` | Core URL, TLS trust, clock, and that the URL is the authenticated device-API URL |
| OTA starts but the image is rejected | The artifact is not an ESP-IDF app image, or `is_format_supported()` rejects the format |
| Parameter deployment fails | Payload exceeds 4000 bytes, NVS is full, or the merged document is too large |
| Parameters are not visible to the app | Load `exarcia_params/params` with `exarcia_params_load()` |
| Device rolls back after an update | The application must run the Exarcia task early enough to confirm the image |
| Logs are missing | The log ring is full, `log_upload_level` filters the entry, or the batch is empty |

## 12. Optional MQTT messaging

MQTT is not part of the OTA HAL. If the application also needs MQTT, add the
separate `exarcia_mqtt_espidf` component. It reads the MQTT credentials from a
parameter deployment, defaults to QoS 0, and keeps its ESP-MQTT task independent
of `exarcia_ota_tick()`.

---

## 13. Design notes

- `stage_finish()` is the only call permitted to switch the boot slot.
  `stage_abort()` must leave the running image selected and bootable.
- The HAL forwards `stage->headers` only for authenticated Core device-API URLs.
  It never adds or infers credentials. A presigned storage URL gets no headers.
- Resumption and partial download are disabled for Core URLs because current
  Core streams the whole payload and does not honour `Range`. A presigned URL
  may use either feature when its server supports `Range`.
- The HAL contains no protocol logic: no endpoint paths, no JSON parsing, no
  deployment state.
- Host tests for the HAL live in the consuming project (the reference demo's
  `tests/` directory). `idf.py build` remains the real-toolchain check.

For the full endpoint, authentication, status-order and parameter-delivery
contract, see the [`exarcia_ota` README](../exarcia-ota-mcu/README.md).
