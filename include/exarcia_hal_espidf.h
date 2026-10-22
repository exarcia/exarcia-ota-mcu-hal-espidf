/* LAYER 2 — ESP-IDF implementation of the Exarcia HAL.
 *
 * Owns every mechanism the platform-agnostic client in components/exarcia_ota
 * deliberately does not: HTTPS transport, NVS storage, the clock, SHA-256 via
 * PSA, and the OTA slot. Contains no protocol logic.
 *
 * ## The one invariant this component must uphold
 *
 * stage_finish() is the ONLY call permitted to switch the boot slot.
 * stage_abort() MUST leave the running image selected and bootable.
 *
 * esp_https_ota_finish() is esp_ota_end() followed by
 * esp_ota_set_boot_partition(staging), which is why the split is safe:
 * LAYER 1 verifies size and SHA-256 after the download and before calling
 * stage_finish(), so a corrupt image is aborted rather than activated.
 *
 * ## Install strategy
 *
 * esp_https_ota only. It is the only path that keeps resumption across a
 * reboot, Range/partial downloads with TLS session resumption, staging
 * partitions and image chip-ID/revision/secure-version validation.
 *
 * Its cost is that esp_https_ota exposes no per-buffer callback, so the staged
 * image cannot be hashed while it is written. stage_digest() therefore reads
 * the target partition back with esp_partition_read() and hashes that.
 * esp_partition_mmap() would be faster, but ESP_PARTITION_MMAP_DATA is
 * documented as mapping into the data window on esp32 only, so it risks
 * colliding with the running image on the other targets.
 */
#pragma once

#include "exarcia_hal.h" /* from LAYER 1 */

#include "esp_err.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Default NVS partition holding ALL Exarcia state. The partition table must
 * contain it (see partitions_exarcia_4mb.csv); manufacturing flashes
 * per-device .bin files here. */
#define EXARCIA_HAL_NVS_PARTITION "exarcia"

typedef struct exarcia_hal_espidf_config {
    /* NULL or "" selects CONFIG_EXARCIA_HAL_NVS_PARTITION. */
    const char *nvs_partition;
    /* Explicit CA allowlist in PEM form. When set it always wins and
     * use_cert_bundle is ignored. Borrowed for the lifetime of the HAL. */
    const char *ca_pem;
    bool ca_present;
    /* Optional fallback for applications that do not supply a CA: validate
     * against the ESP certificate bundle. Ignored when ca_present/ca_pem are
     * set, because an explicit CA is authoritative. The kiln controller keeps
     * this false. */
    bool use_cert_bundle;
    /* Lab only: keeps chain validation, skips hostname verification. */
    bool tls_skip_hostname;
    int http_timeout_ms;
    int ota_timeout_ms;
    /* HTTP receive buffer. Smaller means less RAM and more esp_https_ota
     * progress callbacks, which LAYER 1 throttles. */
    int ota_buffer_size;
    /* Range requests plus TLS session reuse. Requires the image server to
     * honour Range; esp_https_ota falls back to a single request otherwise. */
    bool enable_partial_download;
    int http_request_size;
    /* Persist and resume a partially downloaded image across a reboot. */
    bool enable_resumption;
} exarcia_hal_espidf_config_t;

/* Fills `config` from Kconfig defaults. Safe before esp_netif is up. */
void exarcia_hal_espidf_default_config(exarcia_hal_espidf_config_t *config);

/* Brings up the "exarcia" NVS partition and populates `hal`. When the running
 * partition table has no such entry this fails with ESP_ERR_NOT_FOUND and a log
 * line naming partitions_exarcia_4mb.csv, rather than silently falling back to
 * the default "nvs" partition. */
esp_err_t exarcia_hal_espidf_init(exarcia_hal_t *hal, const exarcia_hal_espidf_config_t *config);

/* Releases the NVS partition handle. Does not touch the OTA slot. */
void exarcia_hal_espidf_deinit(exarcia_hal_t *hal);

/* SNTP over the application's existing network. Call once L3 is up and before
 * the first HTTPS request, since TLS rejects an unvalidated clock. On timeout
 * the request gate below keeps every TLS call deferred until SNTP succeeds, so
 * the device never presents a 2001 RTC timestamp to a certificate verifier. */
esp_err_t exarcia_hal_espidf_sync_clock(exarcia_hal_t *hal, int timeout_ms);

/* True once the system clock is past the build's certificate-validity floor.
 * Every HTTPS/OTA request is refused with EXARCIA_ERR_UNAVAILABLE while this is
 * false, and MQTT is not started, so a dead RTC cannot produce a misleading
 * MBEDTLS_ERR_X509_CERT_VERIFY_FAILED before SNTP corrects the clock. */
bool exarcia_hal_espidf_clock_is_valid(void);

/* Value for the X-Transport header, so the backend can attribute connectivity
 * per device. Derived from which netif is up. */
const char *exarcia_hal_espidf_transport(void);

#ifdef __cplusplus
}
#endif