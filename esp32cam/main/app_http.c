#include "app_http.h"

#include <errno.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>

#include "app_camera.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "app_http";

#define STREAM_BOUNDARY_STR "123456789000000000000987654321"
#define STREAM_CONTENT_TYPE "multipart/x-mixed-replace;boundary=" STREAM_BOUNDARY_STR
#define STREAM_PART_HEADER "\r\n--" STREAM_BOUNDARY_STR "\r\n"
// Boundary and part headers are written in one go: separate small writes trigger
// Nagle/delayed-ACK stalls and cut the raw frame rate by an order of magnitude.
#define STREAM_PART_FMT STREAM_PART_HEADER "Content-Type: %s\r\nContent-Length: %u\r\n\r\n"

#define STREAM_TASK_STACK 4096
#define STREAM_TASK_PRIORITY 5
// Raw frames are uncompressed grey (QQVGA = 19200 bytes); the link, not the
// sensor, sets the pace, so only a token pause is needed.
#define RAW_FRAME_INTERVAL_MS 5

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static uint32_t s_stream_frames;
static uint32_t s_jpeg_frames;
static uint32_t s_raw_frames;
static int64_t s_stream_started_us;

static esp_err_t root_get(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start);
}

static esp_err_t status_get(httpd_req_t *req)
{
    char body[384];
    int64_t now = esp_timer_get_time();
    int64_t stream_seconds = s_stream_started_us > 0 ? (now - s_stream_started_us) / 1000000 : 0;

    int len = snprintf(body, sizeof(body),
                       "{\"camera\":\"%s\",\"mode\":\"%s\",\"sensor\":\"%s\","
                       "\"streams\":%d,\"frames\":{\"stream\":%u,\"jpeg\":%u,\"raw\":%u},"
                       "\"stream_seconds\":%lld,\"heap_internal_free\":%u,\"psram_free\":%u}\n",
                       app_cam_available() ? "ready" : "unavailable", app_cam_mode_name(),
                       app_cam_sensor_name(), app_cam_stream_users(),
                       (unsigned)s_stream_frames, (unsigned)s_jpeg_frames,
                       (unsigned)s_raw_frames, (long long)stream_seconds,
                       (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                       (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, body, len);
}

static esp_err_t conflict(httpd_req_t *req, const char *message)
{
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, message);
}

static esp_err_t mode_get(httpd_req_t *req)
{
    char query[64];
    char value[16];

    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "set", value, sizeof(value)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "use /mode?set=jpeg or ?set=gray");
        return ESP_FAIL;
    }

    app_cam_mode_t target;
    if (strcmp(value, "jpeg") == 0) {
        target = APP_CAM_MODE_JPEG;
    } else if (strcmp(value, "gray") == 0) {
        target = APP_CAM_MODE_RAW;
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "set must be jpeg or gray");
        return ESP_FAIL;
    }

    if (app_cam_stream_users() > 0) {
        return conflict(req, "a stream is running; stop it before switching mode");
    }

    esp_err_t err = app_cam_set_mode(target);
    if (err != ESP_OK && err != ESP_ERR_NOT_FINISHED) {
        ESP_LOGE(TAG, "mode switch failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "mode switch failed");
        return ESP_FAIL;
    }

    char body[64];
    int len = snprintf(body, sizeof(body), "{\"mode\":\"%s\"}\n", app_cam_mode_name());
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, len);
}

static esp_err_t jpg_get(httpd_req_t *req)
{
    if (!app_cam_available()) {
        return conflict(req, "no sensor detected");
    }
    if (app_cam_mode() != APP_CAM_MODE_JPEG) {
        return conflict(req, "sensor is in raw mode; switch with /mode?set=jpeg");
    }

    camera_fb_t *fb = app_cam_fb_get();
    if (fb == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "capture failed");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=capture.jpg");
    esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);

    app_cam_fb_return(fb);
    if (res == ESP_OK) {
        s_jpeg_frames++;
    }
    return res;
}

// One greyscale frame. The sensor produces YUV422; the luma plane is copied out
// here (a single stride copy) so clients receive plain 8-bit pixels.
static esp_err_t raw_get(httpd_req_t *req)
{
    if (!app_cam_available()) {
        return conflict(req, "no sensor detected");
    }
    if (app_cam_mode() != APP_CAM_MODE_RAW) {
        return conflict(req, "sensor is in jpeg mode; switch with /mode?set=gray");
    }

    camera_fb_t *fb = app_cam_fb_get();
    if (fb == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "capture failed");
        return ESP_FAIL;
    }

    const size_t pixels = (size_t)app_cam_frame_width() * app_cam_frame_height();
    uint8_t *luma = heap_caps_malloc_prefer(pixels, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                            MALLOC_CAP_8BIT);
    if (luma == NULL) {
        app_cam_fb_return(fb);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_FAIL;
    }
    size_t len = app_cam_luma_from_fb(fb, luma, pixels);
    app_cam_fb_return(fb);
    if (len == 0) {
        heap_caps_free(luma);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "unexpected frame format");
        return ESP_FAIL;
    }

    char width[8];
    char height[8];
    snprintf(width, sizeof(width), "%d", app_cam_frame_width());
    snprintf(height, sizeof(height), "%d", app_cam_frame_height());

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "X-Frame-Width", width);
    httpd_resp_set_hdr(req, "X-Frame-Height", height);
    httpd_resp_set_hdr(req, "X-Frame-Format", app_cam_frame_format());
    esp_err_t res = httpd_resp_send(req, (const char *)luma, len);
    heap_caps_free(luma);

    if (res == ESP_OK) {
        s_raw_frames++;
    }
    return res;
}

static esp_err_t send_stream_part(httpd_req_t *req, const char *content_type, const uint8_t *data,
                                  size_t len)
{
    char head[160];
    int head_len = snprintf(head, sizeof(head), STREAM_PART_FMT, content_type, (unsigned)len);

    esp_err_t res = httpd_resp_send_chunk(req, head, head_len);
    if (res == ESP_OK) {
        res = httpd_resp_send_chunk(req, (const char *)data, len);
    }
    return res;
}

// A send only fails once TCP notices the peer is gone, which can take many
// seconds on a half-closed socket. Polling for EOF lets the stream task exit as
// soon as the viewer goes away, so mode switches are not blocked by a zombie.
static bool client_gone(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    if (fd < 0) {
        return true;
    }

    char scratch;
    int r = recv(fd, &scratch, 1, MSG_DONTWAIT);
    if (r == 0) {
        return true;
    }
    if (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        return true;
    }
    return false;
}

// The stream lives on its own task: a blocking loop inside the server task would
// stop every other endpoint until the client disconnected.
static void stream_task(void *arg)
{
    httpd_req_t *req = (httpd_req_t *)arg;
    const bool raw = app_cam_mode() == APP_CAM_MODE_RAW;

    // Every frame starts with a small header write; with Nagle enabled that write
    // waits for an ACK and each frame stalls ~40 ms.
    const int fd = httpd_req_to_sockfd(req);
    if (fd >= 0) {
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }

    // Header values must outlive the response header block, so they sit in this
    // task's frame for the whole loop.
    char width_str[8];
    char height_str[8];
    snprintf(width_str, sizeof(width_str), "%d", app_cam_frame_width());
    snprintf(height_str, sizeof(height_str), "%d", app_cam_frame_height());

    uint8_t *luma = NULL;
    size_t luma_size = 0;
    if (raw) {
        luma_size = (size_t)app_cam_frame_width() * app_cam_frame_height();
        luma = heap_caps_malloc_prefer(luma_size, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                       MALLOC_CAP_8BIT);
        if (luma == NULL) {
            ESP_LOGE(TAG, "no memory for the luma buffer");
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
            httpd_req_async_handler_complete(req);
            app_cam_end_stream();
            vTaskDelete(NULL);
            return;
        }
    }

    esp_err_t res = httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
    if (res == ESP_OK) {
        res = httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    }
    if (res == ESP_OK) {
        res = httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    }
    if (res == ESP_OK && raw) {
        res = httpd_resp_set_hdr(req, "X-Frame-Width", width_str);
        if (res == ESP_OK) {
            res = httpd_resp_set_hdr(req, "X-Frame-Height", height_str);
        }
        if (res == ESP_OK) {
            res = httpd_resp_set_hdr(req, "X-Frame-Format", app_cam_frame_format());
        }
        if (res != ESP_OK) {
            ESP_LOGE(TAG, "stream headers failed: %s", esp_err_to_name(res));
        }
    }

    ESP_LOGI(TAG, "stream client connected (%s)", raw ? "raw" : "jpeg");
    s_stream_started_us = esp_timer_get_time();

    uint32_t stat_frames = 0;
    int64_t stat_start = esp_timer_get_time();
    int64_t stat_capture_us = 0;
    int64_t stat_send_us = 0;

    while (res == ESP_OK) {
        if (client_gone(req)) {
            ESP_LOGI(TAG, "client closed the stream");
            break;
        }

        const int64_t t_capture = esp_timer_get_time();
        camera_fb_t *fb = app_cam_fb_get();
        const int64_t t_captured = esp_timer_get_time();
        if (fb == NULL) {
            ESP_LOGW(TAG, "stream capture failed, closing");
            break;
        }

        const uint8_t *payload = fb->buf;
        size_t payload_len = fb->len;
        if (raw) {
            payload_len = app_cam_luma_from_fb(fb, luma, luma_size);
            payload = luma;
            if (payload_len == 0) {
                ESP_LOGW(TAG, "unexpected frame format, closing");
                app_cam_fb_return(fb);
                break;
            }
        }

        res = send_stream_part(req, raw ? "application/octet-stream" : "image/jpeg", payload,
                               payload_len);
        const int64_t t_sent = esp_timer_get_time();
        app_cam_fb_return(fb);

        if (res == ESP_OK) {
            s_stream_frames++;
            stat_frames++;
            stat_capture_us += t_captured - t_capture;
            stat_send_us += t_sent - t_captured;
            const int64_t now = esp_timer_get_time();
            if (now - stat_start >= 5000000) {
                ESP_LOGI(TAG, "stream: %u frames in %.1f s (%.1f fps), capture %.0f ms, "
                         "send %.0f ms", stat_frames, (now - stat_start) / 1e6f,
                         stat_frames * 1e6f / (now - stat_start),
                         stat_capture_us / 1000.0f / stat_frames,
                         stat_send_us / 1000.0f / stat_frames);
                stat_frames = 0;
                stat_capture_us = 0;
                stat_send_us = 0;
                stat_start = now;
            }
            if (raw) {
                vTaskDelay(pdMS_TO_TICKS(RAW_FRAME_INTERVAL_MS));
            }
        }
    }

    httpd_resp_send_chunk(req, NULL, 0);
    s_stream_started_us = 0;
    heap_caps_free(luma);
    httpd_req_async_handler_complete(req);
    app_cam_end_stream();
    ESP_LOGI(TAG, "stream client disconnected");
    vTaskDelete(NULL);
}

static esp_err_t stream_get(httpd_req_t *req)
{
    if (!app_cam_available()) {
        return conflict(req, "no sensor detected");
    }
    if (!app_cam_begin_stream()) {
        return conflict(req, "camera is switching mode, retry");
    }

    httpd_req_t *copy = NULL;
    esp_err_t err = httpd_req_async_handler_begin(req, &copy);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "async handler begin failed: %s", esp_err_to_name(err));
        app_cam_end_stream();
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "server busy");
        return ESP_FAIL;
    }

    if (xTaskCreate(stream_task, "mjpeg", STREAM_TASK_STACK, copy, STREAM_TASK_PRIORITY, NULL) !=
        pdPASS) {
        ESP_LOGE(TAG, "cannot start stream task");
        app_cam_end_stream();
        httpd_req_async_handler_complete(copy);
        return ESP_FAIL;
    }

    return ESP_OK;
}

esp_err_t app_http_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 10;
    config.stack_size = 6144;
    config.lru_purge_enable = true;
    config.recv_wait_timeout = 10;
    config.send_wait_timeout = 5;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t uris[] = {
        { .uri = "/", .method = HTTP_GET, .handler = root_get },
        { .uri = "/stream", .method = HTTP_GET, .handler = stream_get },
        { .uri = "/jpg", .method = HTTP_GET, .handler = jpg_get },
        { .uri = "/raw", .method = HTTP_GET, .handler = raw_get },
        { .uri = "/mode", .method = HTTP_GET, .handler = mode_get },
        { .uri = "/status", .method = HTTP_GET, .handler = status_get },
    };

    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        err = httpd_register_uri_handler(server, &uris[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "registering %s failed: %s", uris[i].uri, esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(TAG, "http server listening on port %d", config.server_port);
    return ESP_OK;
}
