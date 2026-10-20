#include "exarcia_hal_espidf.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_log_level.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "psa/crypto.h"

static const char *TAG = "exarcia_hal";

/* Chunk size for stage_digest(). Reading the staged image back is a flash-mapped
 * copy, so a modest buffer keeps the stack small without costing throughput. */
#ifndef EXARCIA_DIGEST_CHUNK
#define EXARCIA_DIGEST_CHUNK 4096
#endif

/* Refuse absurd response bodies rather than allocating them. Core answers are
 * small; anything larger is a misconfiguration or an attack. */
#define EXARCIA_MAX_RESPONSE_BYTES (64U * 1024U)

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    esp_err_t error;
} response_buffer_t;

typedef struct exarcia_hal_espidf {
    exarcia_hal_t hal; /* must stay first: hal->user points back here */
    exarcia_hal_espidf_config_t config;
    char nvs_partition[32];

    const esp_partition_t *staging;
    esp_https_ota_handle_t https_ota;
    char staged_version[EXARCIA_APP_VERSION_MAX];
    /* Borrowed from stage_begin() until esp_https_ota_begin() returns: the
     * ESP-IDF client-init callback has no user argument. */
    const exarcia_headers_t *stage_headers;
} exarcia_hal_espidf_t;

static exarcia_hal_espidf_t *stage_callback_owner;

/* Helpers */

static exarcia_hal_espidf_t *self(void *user)
{
    return (exarcia_hal_espidf_t *)user;
}

static exarcia_err_t from_esp_err(esp_err_t err)
{
    switch (err) {
    case ESP_OK:
        return EXARCIA_OK;
    case ESP_ERR_INVALID_ARG:
        return EXARCIA_ERR_INVALID_ARG;
    case ESP_ERR_INVALID_SIZE:
        return EXARCIA_ERR_BUFFER_TOO_SMALL;
    case ESP_ERR_NOT_FOUND:
        return EXARCIA_ERR_NOT_FOUND;
    case ESP_ERR_NOT_SUPPORTED:
        return EXARCIA_ERR_NOT_SUPPORTED;
    case ESP_ERR_NO_MEM:
        return EXARCIA_ERR_NO_SPACE;
    case ESP_ERR_TIMEOUT:
        return EXARCIA_ERR_UNAVAILABLE;
    case ESP_ERR_NVS_NOT_FOUND:
        return EXARCIA_ERR_NOT_FOUND;
    case ESP_ERR_NVS_INVALID_LENGTH:
    case ESP_ERR_NVS_VALUE_TOO_LONG:
        return EXARCIA_ERR_BUFFER_TOO_SMALL;
    case ESP_ERR_NVS_NOT_ENOUGH_SPACE:
        return EXARCIA_ERR_NO_SPACE;
    default:
        return EXARCIA_ERR_UNAVAILABLE;
    }
}

static void hal_log(void *user, exarcia_log_level_t level, const char *message)
{
    (void)user;
    switch (level) {
    case EXARCIA_LOG_ERROR:
        ESP_LOGE(TAG, "%s", message);
        break;
    case EXARCIA_LOG_WARN:
        ESP_LOGW(TAG, "%s", message);
        break;
    case EXARCIA_LOG_DEBUG:
        ESP_LOGD(TAG, "%s", message);
        break;
    case EXARCIA_LOG_INFO:
    default:
        ESP_LOGI(TAG, "%s", message);
        break;
    }
}

static void configure_tls(esp_http_client_config_t *http, const exarcia_hal_espidf_config_t *config)
{
    if (config->ca_present && config->ca_pem != NULL && config->ca_pem[0] != '\0') {
        http->cert_pem = config->ca_pem;
    } else if (config->use_cert_bundle) {
        http->crt_bundle_attach = esp_crt_bundle_attach;
    }
    if (config->tls_skip_hostname) {
        http->skip_cert_common_name_check = true;
    }
}

/* Time */

static uint64_t hal_now_ms(void *user)
{
    (void)user;
    return (uint64_t)(esp_timer_get_time() / 1000LL);
}

esp_err_t exarcia_hal_espidf_sync_clock(exarcia_hal_t *hal, int timeout_ms)
{
    if (hal == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");

    if (esp_netif_sntp_init(&sntp) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP init failed; HTTPS may fail until the clock is valid");
        return ESP_FAIL;
    }
    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS((uint32_t)timeout_ms)) != ESP_OK) {
        ESP_LOGW(TAG, "SNTP sync timed out; continuing with the current clock");
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

const char *exarcia_hal_espidf_transport(void)
{
    /* Reported to the backend as the X-Transport header, for fleet attribution
     * only. Set from Kconfig because the bearer is a property of the product
     * build, not something to sniff at runtime. */
    return CONFIG_EXARCIA_HAL_TRANSPORT;
}

/* Storage */

/* The dedicated partition, not the default "nvs" the app owns: provisioning,
 * rotated credentials and OTA bookkeeping must survive an app update that
 * reformats or erases the application's own namespace. */
static esp_err_t store_open(exarcia_hal_espidf_t *impl, const char *ns, nvs_open_mode_t mode,
                            nvs_handle_t *out)
{
    esp_err_t err = nvs_open_from_partition(impl->nvs_partition, ns, mode, out);

    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Never erase automatically: this partition holds the device UUID, PSK
         * and the pending OTA record. Losing it turns a recoverable storage
         * problem (or an NVS format change followed by a rollback) into a
         * device that must be re-provisioned in the factory. */
        ESP_LOGE(TAG,
                 "Exarcia partition '%s' cannot be opened (%s); refusing to erase "
                 "provisioning. Re-flash the factory NVS image or erase it explicitly.",
                 impl->nvs_partition, esp_err_to_name(err));
    }
    return err;
}

static exarcia_err_t hal_store_get(void *user, const char *ns, const char *key,
                                   char *out, size_t out_size)
{
    exarcia_hal_espidf_t *impl = self(user);
    nvs_handle_t handle;
    size_t required = out_size;

    if (impl == NULL || ns == NULL || key == NULL || out == NULL || out_size == 0U) {
        return EXARCIA_ERR_INVALID_ARG;
    }
    {
        esp_err_t open_err = store_open(impl, ns, NVS_READONLY, &handle);
        if (open_err != ESP_OK) {
            return from_esp_err(open_err);
        }
    }
    {
        esp_err_t err = nvs_get_str(handle, key, out, &required);
        nvs_close(handle);
        if (err == ESP_OK) {
            return EXARCIA_OK;
        }
        return (err == ESP_ERR_NVS_NOT_FOUND) ? EXARCIA_ERR_NOT_FOUND : from_esp_err(err);
    }
}

static exarcia_err_t hal_store_set(void *user, const char *ns, const char *key, const char *value)
{
    exarcia_hal_espidf_t *impl = self(user);
    nvs_handle_t handle;
    esp_err_t err;

    if (impl == NULL || ns == NULL || key == NULL || value == NULL) {
        return EXARCIA_ERR_INVALID_ARG;
    }
    err = store_open(impl, ns, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return from_esp_err(err);
    }
    err = nvs_set_str(handle, key, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return from_esp_err(err);
}

static exarcia_err_t hal_erase_namespace(void *user, const char *ns)
{
    exarcia_hal_espidf_t *impl = self(user);
    nvs_handle_t handle;
    esp_err_t err;

    if (impl == NULL || ns == NULL) {
        return EXARCIA_ERR_INVALID_ARG;
    }
    err = store_open(impl, ns, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return from_esp_err(err);
    }
    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return from_esp_err(err);
}

/* Transport */

static esp_err_t http_event_handler(esp_http_client_event_t *event)
{
    response_buffer_t *response = (response_buffer_t *)event->user_data;
    size_t needed;

    if (event->event_id != HTTP_EVENT_ON_DATA || event->data_len <= 0 || response->error != ESP_OK) {
        return ESP_OK;
    }
    needed = response->length + (size_t)event->data_len + 1U;
    if (needed > EXARCIA_MAX_RESPONSE_BYTES) {
        response->error = ESP_ERR_INVALID_SIZE;
        return response->error;
    }
    if (needed > response->capacity) {
        size_t capacity = (response->capacity == 0U) ? 512U : response->capacity;
        char *expanded;
        while (capacity < needed) {
            capacity *= 2U;
        }
        expanded = (char *)realloc(response->data, capacity);
        if (expanded == NULL) {
            response->error = ESP_ERR_NO_MEM;
            return response->error;
        }
        response->data = expanded;
        response->capacity = capacity;
    }
    memcpy(response->data + response->length, event->data, (size_t)event->data_len);
    response->length += (size_t)event->data_len;
    response->data[response->length] = '\0';
    return ESP_OK;
}

static esp_err_t to_http_method(const char *method, esp_http_client_method_t *out)
{
    if (method == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(method, "POST") == 0) {
        *out = HTTP_METHOD_POST;
    } else if (strcmp(method, "GET") == 0) {
        *out = HTTP_METHOD_GET;
    } else if (strcmp(method, "PATCH") == 0) {
        *out = HTTP_METHOD_PATCH;
    } else {
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static exarcia_err_t hal_http_request(void *user, const char *method, const char *url,
                                      const exarcia_headers_t *headers, const char *body,
                                      size_t body_len, exarcia_http_response_t *out)
{
    exarcia_hal_espidf_t *impl = self(user);
    response_buffer_t response = {.error = ESP_OK};
    esp_http_client_config_t http_config;
    esp_http_client_method_t http_method;
    esp_http_client_handle_t client;
    esp_err_t err;
    size_t i;

    if (impl == NULL || url == NULL || out == NULL) {
        return EXARCIA_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    err = to_http_method(method, &http_method);
    if (err != ESP_OK) {
        return EXARCIA_ERR_INVALID_ARG;
    }

    memset(&http_config, 0, sizeof(http_config));
    http_config.url = url;
    http_config.timeout_ms = impl->config.http_timeout_ms;
    http_config.buffer_size = impl->config.ota_buffer_size;
    /* Every Core call carries X-Device-ID and X-PSK, so a redirect would hand the
     * credential to whatever host the response names. ESP-IDF follows 30x by
     * default and esp_http_client_set_url() only resets the connection on a host
     * change -- it does not strip headers set by the application. Refuse instead;
     * a 3xx then surfaces as a status code LAYER 1 can report. */
    http_config.disable_auto_redirect = true;
    http_config.event_handler = http_event_handler;
    http_config.user_data = &response;
    configure_tls(&http_config, &impl->config);

    client = esp_http_client_init(&http_config);
    if (client == NULL) {
        return EXARCIA_ERR_NO_SPACE;
    }

    err = esp_http_client_set_method(client, http_method);
    for (i = 0; err == ESP_OK && headers != NULL && i < headers->count; i++) {
        err = esp_http_client_set_header(client, headers->items[i].name, headers->items[i].value);
    }
    if (err == ESP_OK && body != NULL && body_len > 0U) {
        err = esp_http_client_set_header(client, "Content-Type", "application/json");
        if (err == ESP_OK) {
            err = esp_http_client_set_post_field(client, body, (int)body_len);
        }
    }

    if (err == ESP_OK) {
        err = esp_http_client_perform(client);
    }

    /* Read the status code even when perform() failed.
     *
     * ESP-IDF intercepts 401 in esp_http_check_response() and calls
     * esp_http_client_add_auth(), which fails when no user/password is
     * configured; perform() then returns ESP_FAIL and the 401 never reaches
     * LAYER 1. The device therefore could not tell a revoked credential from a
     * network fault, so its re-registration path could never run. Reporting the
     * real status lets LAYER 1 map 401 to EXARCIA_ERR_UNAUTHORIZED, and the other
     * 4xx classes it depends on, exactly as it does on any other platform. */
    {
        int status = esp_http_client_get_status_code(client);
        esp_http_client_cleanup(client);

        if (status > 0) {
            /* An HTTP response is more informative than the transport error, and
             * LAYER 1 already knows how to classify it. */
            out->status_code = status;
            if (response.error != ESP_OK) {
                free(response.data);
                return from_esp_err(response.error);
            }
            goto have_response;
        }

        if (err != ESP_OK) {
            free(response.data);
            return from_esp_err(err);
        }
        if (response.error != ESP_OK) {
            free(response.data);
            return from_esp_err(response.error);
        }
        out->status_code = status;
    }

have_response:
    /* Always hand back a NUL-terminated buffer: several Core endpoints answer
     * with an empty body and the parsers expect "{}" semantics. */
    if (response.data == NULL) {
        response.data = (char *)malloc(2U);
        if (response.data == NULL) {
            return EXARCIA_ERR_NO_SPACE;
        }
        response.data[0] = '{';
        response.data[1] = '\0';
        response.length = 0U;
    }
    out->body = response.data;
    out->body_len = response.length;
    return EXARCIA_OK;
}

/* Identity and capability */

static exarcia_err_t hal_get_mac(void *user, uint8_t out[6])
{
    (void)user;
    if (out == NULL) {
        return EXARCIA_ERR_INVALID_ARG;
    }
    return from_esp_err(esp_read_mac(out, ESP_MAC_WIFI_STA));
}

static exarcia_err_t hal_get_app_version(void *user, char *out, size_t out_size)
{
    const esp_app_desc_t *descriptor;

    (void)user;
    if (out == NULL || out_size == 0U) {
        return EXARCIA_ERR_INVALID_ARG;
    }
    out[0] = '\0';
    /* The running app descriptor is the authoritative version string: it is
     * what PROJECT_VER stamped into this image. */
    descriptor = esp_app_get_description();
    if (descriptor == NULL || descriptor->version[0] == '\0') {
        return EXARCIA_ERR_IO;
    }
    snprintf(out, out_size, "%s", descriptor->version);
    return EXARCIA_OK;
}

static bool hal_is_format_supported(void *user, const char *format)
{
    (void)user;
    if (format == NULL || format[0] == '\0') {
        return true; /* unspecified: let the bootloader validate it */
    }
    /* Only the signed ESP-IDF application image is installable here.
     * "raw-firmware" is deliberately NOT accepted: esp_ota_end() validates the
     * app descriptor and magic byte, which a raw blob lacks. */
    return strcmp(format, "esp-idf-app") == 0;
}

static exarcia_err_t hal_get_boot_state(void *user, exarcia_boot_state_t *out)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;

    (void)user;
    if (out == NULL || running == NULL) {
        return EXARCIA_ERR_INVALID_ARG;
    }
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return EXARCIA_ERR_IO;
    }
    switch (state) {
    case ESP_OTA_IMG_PENDING_VERIFY:
        *out = EXARCIA_BOOT_PENDING_VERIFY;
        break;
    case ESP_OTA_IMG_VALID:
        *out = EXARCIA_BOOT_VALID;
        break;
    case ESP_OTA_IMG_INVALID:
        *out = EXARCIA_BOOT_INVALID;
        break;
    case ESP_OTA_IMG_NEW:
        *out = EXARCIA_BOOT_NEW;
        break;
    default:
        *out = EXARCIA_BOOT_UNDEFINED;
        break;
    }
    return EXARCIA_OK;
}

static exarcia_err_t hal_mark_app_valid(void *user)
{
    (void)user;
    return from_esp_err(esp_ota_mark_app_valid_cancel_rollback());
}

static exarcia_err_t hal_mark_app_invalid(void *user)
{
    (void)user;
    /* An operator-scheduled rollback means "revert now", so the running image is
     * disowned and the device restarts onto the previous slot. Marking without
     * rebooting would leave the bad image in place until some unrelated reset. */
    esp_err_t err = esp_ota_mark_app_invalid_rollback();
    if (err != ESP_OK) {
        return from_esp_err(err);
    }
    esp_restart();
    return EXARCIA_OK; /* not reached */
}

static exarcia_err_t hal_request_restart(void *user)
{
    (void)user;
    esp_restart();
    return EXARCIA_OK; /* not reached */
}

/* Staging */

/* esp_https_ota_config_t.http_client_init_cb has no user argument, so the HAL
 * instance is handed over through this file-static pointer for the duration of
 * esp_https_ota_begin(). Only one OTA can be staged at a time. */
static esp_err_t stage_http_client_init_cb(esp_http_client_handle_t client)
{
    exarcia_hal_espidf_t *impl = stage_callback_owner;
    size_t index;

    if (impl == NULL || impl->stage_headers == NULL || impl->stage_headers->items == NULL ||
        client == NULL) {
        return ESP_OK;
    }
    for (index = 0U; index < impl->stage_headers->count; ++index) {
        const exarcia_header_t *header = &impl->stage_headers->items[index];
        esp_err_t err;

        if (header->name == NULL || header->value == NULL) {
            continue;
        }
        err = esp_http_client_set_header(client, header->name, header->value);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

/* Read the descriptor from the bytes downloaded for this update, not from the
 * inactive partition: at stage_begin() the partition still holds whatever was
 * there before (often the previous release on a reused slot). Using the
 * partition reader here can therefore persist the old version and make the next
 * boot misdiagnose a successful activation as a rollback. */
static void capture_staged_version(exarcia_hal_espidf_t *impl)
{
    esp_app_desc_t staged;

    if (impl == NULL || impl->https_ota == NULL || impl->staging == NULL ||
        impl->staged_version[0] != '\0') {
        return;
    }
    if (esp_https_ota_get_img_desc(impl->https_ota, &staged) != ESP_OK) {
        ESP_LOGW(TAG, "could not read the staged image descriptor yet");
        return;
    }
    if (strlen(staged.version) >= EXARCIA_APP_VERSION_MAX) {
        ESP_LOGW(TAG, "staged version is too long for esp_app_desc_t; it will not match");
    }
    snprintf(impl->staged_version, sizeof(impl->staged_version), "%s", staged.version);
}

static exarcia_err_t hal_stage_begin(void *user, exarcia_stage_t *stage, const char *url)
{
    exarcia_hal_espidf_t *impl = self(user);
    esp_http_client_config_t http_config;
    esp_https_ota_config_t ota_config;

    if (impl == NULL || stage == NULL || url == NULL || url[0] == '\0') {
        return EXARCIA_ERR_INVALID_ARG;
    }
    if (stage->state == EXARCIA_STAGE_ACTIVE || stage->state == EXARCIA_STAGE_FINISHED) {
        return EXARCIA_ERR_INVALID_ARG;
    }

    /* We choose the slot ourselves and keep the pointer, because
     * esp_https_ota exposes no getter for it and stage_digest() has to read
     * back exactly what was written. */
    impl->staging = esp_ota_get_next_update_partition(NULL);
    if (impl->staging == NULL) {
        ESP_LOGE(TAG, "no free OTA slot; check the partition table");
        return EXARCIA_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "staging into %s at 0x%" PRIx32, impl->staging->label, impl->staging->address);

    memset(&http_config, 0, sizeof(http_config));
    http_config.url = url;
    http_config.timeout_ms = impl->config.ota_timeout_ms;
    http_config.buffer_size = impl->config.ota_buffer_size;
    /* Device credentials are forwarded only when LAYER 1 supplied headers for
     * a Core device-API URL. A presigned storage URL gets an empty set, so the
     * PSK cannot leak into a third party's access logs. Refuse redirects only
     * while credentials are attached. */
    http_config.disable_auto_redirect =
        stage->headers != NULL && stage->headers->count > 0U;
    configure_tls(&http_config, &impl->config);

    memset(&ota_config, 0, sizeof(ota_config));
    ota_config.http_config = &http_config;
    ota_config.partition.staging = impl->staging;
    ota_config.http_client_init_cb = stage_http_client_init_cb;
    if (impl->config.enable_resumption && stage->resume_offset > 0U) {
        if (stage->headers != NULL && stage->headers->count > 0U) {
            /* Current Core streams the whole payload and does not honour Range.
             * Sending a Range header would make esp_https_ota append a full
             * image at a resume offset, so restart from zero for Core URLs. */
            ESP_LOGW(TAG, "resumption disabled for an authenticated Core payload");
        } else {
            /* esp_https_ota silently ignores a resume offset below its internal
             * minimum, which is harmless. */
            ota_config.ota_resumption = true;
            ota_config.ota_image_bytes_written = stage->resume_offset;
            ESP_LOGI(TAG, "resuming at %zu bytes", stage->resume_offset);
        }
    }
#if CONFIG_ESP_HTTPS_OTA_ENABLE_PARTIAL_DOWNLOAD
    if (impl->config.enable_partial_download) {
        if (stage->headers != NULL && stage->headers->count > 0U) {
            /* Core does not honour Range, so a partial request would receive the
             * full image and write it more than once. */
            ESP_LOGW(TAG, "partial download disabled for an authenticated Core payload");
        } else {
            ota_config.partial_http_download = true;
            ota_config.max_http_request_size = impl->config.http_request_size;
        }
    }
#else
    if (impl->config.enable_partial_download) {
        ESP_LOGW(TAG, "partial download requested but not built in; using one request");
    }
#endif

    impl->https_ota = NULL;
    impl->staged_version[0] = '\0';
    impl->stage_headers = stage->headers;
    stage_callback_owner = impl;
    {
        esp_err_t err = esp_https_ota_begin(&ota_config, &impl->https_ota);
        impl->stage_headers = NULL;
        stage_callback_owner = NULL;
        if (err != ESP_OK) {
            impl->staging = NULL;
            ESP_LOGE(TAG, "esp_https_ota_begin failed: %s", esp_err_to_name(err));
            return from_esp_err(err);
        }
    }

    stage->state = EXARCIA_STAGE_ACTIVE;
    return EXARCIA_OK;
}

static exarcia_err_t hal_stage_run(void *user, exarcia_stage_t *stage,
                                   exarcia_progress_cb_t on_progress, void *progress_user)
{
    exarcia_hal_espidf_t *impl = self(user);
    size_t image_size;

    if (impl == NULL || stage == NULL || stage->state != EXARCIA_STAGE_ACTIVE) {
        return EXARCIA_ERR_INVALID_ARG;
    }
    image_size = (size_t)esp_https_ota_get_image_size(impl->https_ota);
    if (image_size == 0U || image_size == (size_t)-1) {
        image_size = 0U; /* length unknown: report progress without a total */
    }

    while (1) {
        esp_err_t err = esp_https_ota_perform(impl->https_ota);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "esp_https_ota_perform failed: %s", esp_err_to_name(err));
                return from_esp_err(err);
            }
            break;
        }
        if (on_progress != NULL) {
            int read = esp_https_ota_get_image_len_read(impl->https_ota);
            if (read > 0) {
                on_progress((size_t)read, image_size, progress_user);
            }
        }
    }

    if (on_progress != NULL) {
        int read = esp_https_ota_get_image_len_read(impl->https_ota);
        if (read > 0) {
            on_progress((size_t)read, image_size, progress_user);
        }
    }

    if (!esp_https_ota_is_complete_data_received(impl->https_ota)) {
        ESP_LOGE(TAG, "server closed the connection before the image was complete");
        return EXARCIA_ERR_SIZE_MISMATCH;
    }

    capture_staged_version(impl);
    stage->state = EXARCIA_STAGE_FINISHED;
    return EXARCIA_OK;
}

static size_t hal_stage_bytes(void *user, exarcia_stage_t *stage)
{
    exarcia_hal_espidf_t *impl = self(user);
    int read;

    (void)stage;
    if (impl == NULL || impl->https_ota == NULL) {
        return 0U;
    }
    read = esp_https_ota_get_image_len_read(impl->https_ota);
    return (read > 0) ? (size_t)read : 0U;
}

/* esp_https_ota has no per-buffer callback, so the staged image is read back
 * and hashed here. Flash is memory-mapped, so this is a copy rather than a
 * SPI-flash transaction. */
static exarcia_err_t hal_stage_digest(void *user, exarcia_stage_t *stage, size_t from,
                                      size_t len, uint8_t out[32])
{
    exarcia_hal_espidf_t *impl = self(user);
    psa_hash_operation_t operation = PSA_HASH_OPERATION_INIT;
    uint8_t *chunk;
    size_t offset = from;
    size_t remaining = len;
    psa_status_t status;

    (void)stage;
    if (impl == NULL || impl->staging == NULL || out == NULL || (len > 0U && from + len > (size_t)impl->staging->size)) {
        return EXARCIA_ERR_INVALID_ARG;
    }
    if (len == 0U) {
        return EXARCIA_ERR_INVALID_ARG;
    }

    chunk = (uint8_t *)malloc(EXARCIA_DIGEST_CHUNK);
    if (chunk == NULL) {
        return EXARCIA_ERR_NO_SPACE;
    }
    if (psa_crypto_init() != PSA_SUCCESS) {
        free(chunk);
        return EXARCIA_ERR_IO;
    }
    status = psa_hash_setup(&operation, PSA_ALG_SHA_256);
    if (status != PSA_SUCCESS) {
        free(chunk);
        return EXARCIA_ERR_IO;
    }

    while (remaining > 0U) {
        size_t take = (remaining < EXARCIA_DIGEST_CHUNK) ? remaining : EXARCIA_DIGEST_CHUNK;
        esp_err_t err = esp_partition_read(impl->staging, offset, chunk, take);

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "reading back the staged image failed at 0x%zx: %s", offset,
                     esp_err_to_name(err));
            (void)psa_hash_abort(&operation);
            free(chunk);
            return from_esp_err(err);
        }
        status = psa_hash_update(&operation, chunk, take);
        if (status != PSA_SUCCESS) {
            (void)psa_hash_abort(&operation);
            free(chunk);
            return EXARCIA_ERR_IO;
        }
        offset += take;
        remaining -= take;
    }
    free(chunk);

    {
        size_t digest_length = 0U;
        status = psa_hash_finish(&operation, out, 32U, &digest_length);
        if (status != PSA_SUCCESS || digest_length != 32U) {
            return EXARCIA_ERR_IO;
        }
    }
    return EXARCIA_OK;
}

static exarcia_err_t hal_stage_finish(void *user, exarcia_stage_t *stage)
{
    exarcia_hal_espidf_t *impl = self(user);
    esp_err_t err;

    if (impl == NULL || stage == NULL || impl->https_ota == NULL) {
        return EXARCIA_ERR_INVALID_ARG;
    }
    if (stage->state != EXARCIA_STAGE_FINISHED) {
        /* finish() on a partially written image would validate and activate a
         * truncated app. Refuse it. */
        ESP_LOGE(TAG, "refusing to finish a stage that never completed");
        return EXARCIA_ERR_INVALID_ARG;
    }

    /* The ONLY place the boot slot changes: esp_ota_end() then
     * esp_ota_set_boot_partition(). */
    err = esp_https_ota_finish(impl->https_ota);
    impl->https_ota = NULL;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_finish failed: %s", esp_err_to_name(err));
        return from_esp_err(err);
    }
    stage->state = EXARCIA_STAGE_FINISHED;
    return EXARCIA_OK;
}

static void hal_stage_abort(void *user, exarcia_stage_t *stage)
{
    exarcia_hal_espidf_t *impl = self(user);

    if (impl == NULL || stage == NULL) {
        return;
    }
    if (impl->https_ota != NULL) {
        /* esp_https_ota_abort() closes the connection and frees the handle
         * WITHOUT touching the boot slot, which is exactly what LAYER 1 needs
         * after a verification failure. */
        (void)esp_https_ota_abort(impl->https_ota);
        impl->https_ota = NULL;
    }
    impl->staging = NULL;
    stage->state = EXARCIA_STAGE_ABORTED;
}

static exarcia_err_t hal_get_staged_app_version(void *user, exarcia_stage_t *stage,
                                                char *out, size_t out_size)
{
    exarcia_hal_espidf_t *impl = self(user);

    (void)stage;
    if (impl == NULL || out == NULL || out_size == 0U) {
        return EXARCIA_ERR_INVALID_ARG;
    }
    if (impl->staged_version[0] == '\0') {
        return EXARCIA_ERR_NOT_SUPPORTED;
    }
    snprintf(out, out_size, "%s", impl->staged_version);
    return EXARCIA_OK;
}

/* Lifecycle */

void exarcia_hal_espidf_default_config(exarcia_hal_espidf_config_t *config)
{
    if (config == NULL) {
        return;
    }
    memset(config, 0, sizeof(*config));
    config->nvs_partition = CONFIG_EXARCIA_HAL_NVS_PARTITION;
    config->ca_pem = NULL;
    config->ca_present = false;
#ifdef CONFIG_EXARCIA_HAL_USE_CERT_BUNDLE
    config->use_cert_bundle = true;
#else
    config->use_cert_bundle = false;
#endif
    /* Lab-only escape hatch; the Kconfig default keeps it off. A bool that
     * defaults to n is emitted as "# CONFIG_... is not set", so the symbol is
     * undefined rather than 0 and must be tested with #ifdef. */
#ifdef CONFIG_EXARCIA_HAL_TLS_SKIP_HOSTNAME
    config->tls_skip_hostname = true;
#else
    config->tls_skip_hostname = false;
#endif
    config->http_timeout_ms = CONFIG_EXARCIA_HAL_HTTP_TIMEOUT_MS;
    config->ota_timeout_ms = CONFIG_EXARCIA_HAL_OTA_TIMEOUT_MS;
    config->ota_buffer_size = CONFIG_EXARCIA_HAL_OTA_BUFFER_SIZE;
#ifdef CONFIG_EXARCIA_HAL_ENABLE_PARTIAL_DOWNLOAD
    config->enable_partial_download = true;
    config->http_request_size = CONFIG_EXARCIA_HAL_HTTP_REQUEST_SIZE;
#else
    config->enable_partial_download = false;
    config->http_request_size = 0;
#endif
#ifdef CONFIG_EXARCIA_HAL_ENABLE_RESUMPTION
    config->enable_resumption = true;
#else
    config->enable_resumption = false;
#endif
}

esp_err_t exarcia_hal_espidf_init(exarcia_hal_t *hal, const exarcia_hal_espidf_config_t *config)
{
    exarcia_hal_espidf_t *impl;
    esp_err_t err;

    if (hal == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    impl = (exarcia_hal_espidf_t *)calloc(1U, sizeof(*impl));
    if (impl == NULL) {
        return ESP_ERR_NO_MEM;
    }

    exarcia_hal_espidf_default_config(&impl->config);
    if (config != NULL) {
        impl->config = *config;
    }
    if (impl->config.nvs_partition == NULL || impl->config.nvs_partition[0] == '\0') {
        impl->config.nvs_partition = CONFIG_EXARCIA_HAL_NVS_PARTITION;
    }
    snprintf(impl->nvs_partition, sizeof(impl->nvs_partition), "%s", impl->config.nvs_partition);

    /* Fail loudly when the table has no Exarcia partition rather than quietly
     * using the app's own "nvs", which would mix provisioning with app data. */
    err = nvs_flash_init_partition(impl->nvs_partition);
    if (err == ESP_ERR_NOT_FOUND) {
        ESP_LOGE(TAG, "NVS partition '%s' is missing; flash a table containing it "
                      "(see partitions_exarcia_4mb.csv)", impl->nvs_partition);
        free(impl);
        return err;
    }
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGE(TAG,
                 "Exarcia partition '%s' needs recovery (%s); refusing to erase "
                 "provisioning automatically. Re-flash the factory NVS image or "
                 "erase the partition explicitly.",
                 impl->nvs_partition, esp_err_to_name(err));
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Exarcia NVS init failed: %s", esp_err_to_name(err));
        free(impl);
        return err;
    }

    memset(&impl->hal, 0, sizeof(impl->hal));
    impl->hal.now_ms = hal_now_ms;
    impl->hal.store_get = hal_store_get;
    impl->hal.store_set = hal_store_set;
    impl->hal.erase_namespace = hal_erase_namespace;
    impl->hal.http_request = hal_http_request;
    impl->hal.get_mac = hal_get_mac;
    impl->hal.get_app_version = hal_get_app_version;
    impl->hal.is_format_supported = hal_is_format_supported;
    impl->hal.get_boot_state = hal_get_boot_state;
    impl->hal.mark_app_valid = hal_mark_app_valid;
    impl->hal.mark_app_invalid = hal_mark_app_invalid;
    impl->hal.request_restart = hal_request_restart;
    impl->hal.stage_begin = hal_stage_begin;
    impl->hal.stage_run = hal_stage_run;
    impl->hal.stage_bytes = hal_stage_bytes;
    impl->hal.stage_digest = hal_stage_digest;
    impl->hal.stage_finish = hal_stage_finish;
    impl->hal.stage_abort = hal_stage_abort;
    impl->hal.get_staged_app_version = hal_get_staged_app_version;
    impl->hal.log = hal_log;
    impl->hal.user = impl;

    *hal = impl->hal;

    esp_log_level_set("esp-x509-crt-bundle", ESP_LOG_WARN);

    return ESP_OK;
}

void exarcia_hal_espidf_deinit(exarcia_hal_t *hal)
{
    exarcia_hal_espidf_t *impl;

    if (hal == NULL) {
        return;
    }
    impl = self(hal->user);
    if (impl == NULL) {
        return;
    }
    if (impl->https_ota != NULL) {
        (void)esp_https_ota_abort(impl->https_ota);
        impl->https_ota = NULL;
    }
    (void)nvs_flash_deinit_partition(impl->nvs_partition);
    free(impl);
    memset(hal, 0, sizeof(*hal));
}