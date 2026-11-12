#include "cam_client.h"

#include <errno.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "jpeg_decoder.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

static const char *TAG = "cam_udp";

#define UDP_PORT           CONFIG_APP_UDP_PORT
#define UDP_MAX_PAYLOAD    1400
#define UDP_MAGIC          0x4341  // "CA"
#define UDP_MAX_FRAME      (24 * 1024)
#define UDP_MAX_FRAGMENTS  64
#define UDP_RX_BYTES       2048
#define UDP_TASK_STACK     8192
#define UDP_TASK_PRIORITY  6

// Wire format shared with the camera (esp32cam/main/app_udp.c).
typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint16_t frame_id;
    uint16_t width;
    uint16_t height;
    uint8_t format;      // 0 = grey8, 1 = jpeg
    uint8_t frag_index;
    uint8_t frag_count;
    uint8_t reserved;
    uint32_t payload_len;
} cam_udp_header_t;

#define FEEDBACK_MAGIC 0x4342  // "CB", viewer -> camera

typedef struct __attribute__((packed)) {
    uint16_t magic;
    uint16_t fps_x10;
    uint16_t drops;
    uint16_t reserved;
} cam_feedback_t;

static struct sockaddr_in s_cam_addr;
static bool s_cam_valid;

static SemaphoreHandle_t s_frame_ready;
static SemaphoreHandle_t s_frame_taken;
static SemaphoreHandle_t s_pub_lock;
static cam_frame_t s_pub;
static bool s_pub_valid;

static uint8_t *s_asm;   // reassembly buffer
static uint8_t *s_rx;    // datagram buffer

// Only used when the camera sends JPEG instead of grey (older firmware).
static uint8_t *s_rgb;
static size_t s_rgb_cap;
static uint8_t *s_gray_buf;
static size_t s_gray_cap;

static esp_err_t jpeg_to_gray(const uint8_t *jpeg, size_t len, cam_frame_t *frame)
{
    esp_jpeg_image_cfg_t cfg = {
        .indata = (uint8_t *)jpeg,
        .indata_size = (uint32_t)len,
        .out_format = JPEG_IMAGE_FORMAT_RGB565,
        .out_scale = JPEG_IMAGE_SCALE_0,
    };

    esp_jpeg_image_output_t info = { 0 };
    esp_err_t err = esp_jpeg_get_image_info(&cfg, &info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "not a decodable JPEG (%zu bytes): %s", len, esp_err_to_name(err));
        return ESP_ERR_INVALID_RESPONSE;
    }

    // A QVGA source is halved to keep the RGB565 buffer small; QQVGA is used
    // as-is.
    if (info.width > 200) {
        cfg.out_scale = JPEG_IMAGE_SCALE_1_2;
        if (esp_jpeg_get_image_info(&cfg, &info) != ESP_OK) {
            return ESP_ERR_INVALID_RESPONSE;
        }
    }

    if (info.output_len > s_rgb_cap) {
        uint8_t *grown = realloc(s_rgb, info.output_len);
        if (grown == NULL) {
            return ESP_ERR_NO_MEM;
        }
        s_rgb = grown;
        s_rgb_cap = info.output_len;
    }

    cfg.outbuf = s_rgb;
    cfg.outbuf_size = (uint32_t)s_rgb_cap;
    err = esp_jpeg_decode(&cfg, &info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "jpeg decode failed: %s", esp_err_to_name(err));
        return ESP_ERR_INVALID_RESPONSE;
    }

    const size_t pixels = (size_t)info.width * info.height;
    if (pixels > s_gray_cap) {
        uint8_t *grown = realloc(s_gray_buf, pixels);
        if (grown == NULL) {
            return ESP_ERR_NO_MEM;
        }
        s_gray_buf = grown;
        s_gray_cap = pixels;
    }

    // esp_jpeg leaves RGB565 little-endian: with swap_color_bytes off it writes
    // LOBYTE first and HIBYTE second.
    for (size_t i = 0; i < pixels; i++) {
        const uint16_t rgb = (uint16_t)s_rgb[2 * i] | ((uint16_t)s_rgb[2 * i + 1] << 8);
        const int r = (rgb >> 11) & 0x1F;
        const int g = (rgb >> 5) & 0x3F;
        const int b = rgb & 0x1F;
        const int r8 = (r << 3) | (r >> 2);
        const int g8 = (g << 2) | (g >> 4);
        const int b8 = (b << 3) | (b >> 2);
        s_gray_buf[i] = (uint8_t)((77 * r8 + 150 * g8 + 29 * b8) >> 8);
    }

    frame->data = s_gray_buf;
    frame->len = pixels;
    frame->width = info.width;
    frame->height = info.height;
    frame->from_jpeg = true;
    return ESP_OK;
}

static void publish(const cam_frame_t *frame)
{
    xSemaphoreTake(s_pub_lock, portMAX_DELAY);
    s_pub = *frame;
    s_pub_valid = true;
    xSemaphoreGive(s_pub_lock);

    xSemaphoreGive(s_frame_ready);
    // Do not touch the frame buffers until the display has copied this one.
    xSemaphoreTake(s_frame_taken, pdMS_TO_TICKS(250));
}

static void udp_task(void *arg)
{
    (void)arg;

    const int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket failed: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }

    int rcvbuf = 32 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(UDP_PORT),
        .sin_addr.s_addr = INADDR_ANY,
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind udp/%d failed: errno %d", UDP_PORT, errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "listening on udp/%d", UDP_PORT);

    uint16_t frame_id = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t format = 0;
    uint8_t frag_count = 0;
    uint8_t received = 0;
    uint64_t mask = 0;
    size_t end = 0;
    bool active = false;
    uint32_t frames = 0;
    uint32_t datagrams = 0;
    uint32_t drops = 0;
    uint32_t window_frames = 0;
    int64_t window_start = esp_timer_get_time();
    uint32_t report_frames = 0;
    uint32_t report_drops = 0;
    int64_t report_start = esp_timer_get_time();

    while (true) {
        struct sockaddr_in src;
        socklen_t src_len = sizeof(src);
        const int n = recvfrom(sock, s_rx, UDP_RX_BYTES, 0, (struct sockaddr *)&src, &src_len);
        if (n <= 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        datagrams++;
        s_cam_addr = src;
        s_cam_valid = true;

        // Tell the camera how fast frames are actually reaching the panel, so
        // it can adjust its send interval instead of piling frames up here.
        const int64_t report_now = esp_timer_get_time();
        if (report_now - report_start >= 1000000) {
            const uint32_t elapsed = (uint32_t)(report_now - report_start);
            cam_feedback_t fb = {
                .magic = FEEDBACK_MAGIC,
                .fps_x10 = (uint16_t)((uint64_t)report_frames * 10 * 1000000 / elapsed),
                .drops = (uint16_t)report_drops,
                .reserved = 0,
            };
            struct sockaddr_in dest = s_cam_addr;
            dest.sin_port = htons(UDP_PORT + 1);
            sendto(sock, &fb, sizeof(fb), 0, (struct sockaddr *)&dest, sizeof(dest));
            report_frames = 0;
            report_drops = 0;
            report_start = report_now;
        }
        if (n < (int)sizeof(cam_udp_header_t)) {
            continue;
        }

        cam_udp_header_t header;
        memcpy(&header, s_rx, sizeof(header));
        if (header.magic != UDP_MAGIC || header.frag_count == 0 ||
            header.frag_count > UDP_MAX_FRAGMENTS || header.frag_index >= header.frag_count ||
            header.payload_len > (uint32_t)(n - sizeof(header))) {
            drops++;
            report_drops++;
            continue;
        }

        if (!active || header.frame_id != frame_id || header.frag_index == 0) {
            if (active && received < frag_count) {
                drops++;
                report_drops++;
            }
            active = true;
            frame_id = header.frame_id;
            width = header.width;
            height = header.height;
            format = header.format;
            frag_count = header.frag_count;
            mask = 0;
            received = 0;
            end = 0;
        }

        const size_t off = (size_t)header.frag_index * UDP_MAX_PAYLOAD;
        if (off + header.payload_len > UDP_MAX_FRAME) {
            active = false;
            drops++;
            continue;
        }

        const uint64_t bit = (uint64_t)1 << header.frag_index;
        if ((mask & bit) == 0) {
            mask |= bit;
            received++;
        }
        memcpy(s_asm + off, s_rx + sizeof(header), header.payload_len);
        if (off + header.payload_len > end) {
            end = off + header.payload_len;
        }

        if (received < frag_count) {
            continue;
        }

        active = false;
        cam_frame_t frame = { 0 };
        if (format == 1 || (end >= 2 && s_asm[0] == 0xFF && s_asm[1] == 0xD8)) {
            if (jpeg_to_gray(s_asm, end, &frame) != ESP_OK) {
                drops++;
                continue;
            }
        } else {
            const size_t pixels = (size_t)width * height;
            if (pixels == 0 || pixels > end) {
                drops++;
                continue;
            }
            frame.data = s_asm;
            frame.len = pixels;
            frame.width = width;
            frame.height = height;
            frame.from_jpeg = false;
        }

        publish(&frame);

        frames++;
        window_frames++;
        report_frames++;
        const int64_t now = esp_timer_get_time();
        if (now - window_start >= 5000000) {
            ESP_LOGI(TAG, "%u frames, %.1f fps, %u datagrams, %u drops", frames,
                     window_frames * 1e6f / (now - window_start), datagrams, drops);
            window_frames = 0;
            window_start = now;
        }
    }
}

esp_err_t cam_client_start(void)
{
    s_frame_ready = xSemaphoreCreateBinary();
    s_frame_taken = xSemaphoreCreateBinary();
    s_pub_lock = xSemaphoreCreateMutex();
    if (s_frame_ready == NULL || s_frame_taken == NULL || s_pub_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    s_asm = malloc(UDP_MAX_FRAME);
    s_rx = malloc(UDP_RX_BYTES);
    if (s_asm == NULL || s_rx == NULL) {
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreate(udp_task, "cam_udp", UDP_TASK_STACK, NULL, UDP_TASK_PRIORITY, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t cam_client_wait_frame(cam_frame_t *frame, uint8_t *dst, size_t dst_cap,
                                uint32_t timeout_ms)
{
    if (s_frame_ready == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_frame_ready, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    esp_err_t err = ESP_OK;
    xSemaphoreTake(s_pub_lock, portMAX_DELAY);
    if (s_pub_valid && s_pub.data != NULL && s_pub.len <= dst_cap) {
        *frame = s_pub;
        memcpy(dst, s_pub.data, s_pub.len);
        frame->data = dst;
    } else {
        err = ESP_ERR_INVALID_SIZE;
    }
    s_pub_valid = false;
    xSemaphoreGive(s_pub_lock);
    xSemaphoreGive(s_frame_taken);
    return err;
}
