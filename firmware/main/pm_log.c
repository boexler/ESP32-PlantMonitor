/**
 * @file pm_log.c
 * @brief Optional UDP syslog forwarding (RFC 5424-style payload).
 */

#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <arpa/inet.h>
#include <time.h>
#include "sdkconfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/message_buffer.h"
#include "lwip/sockets.h"
#include "esp_log.h"

#include "pm_log.h"

static const char PLANT_TAG[] = "plantmonitor";

MessageBufferHandle_t xMessageBufferTrans;
bool writeToStdout;
char wifiIpAddress[16] = {'-', '\0'};

static void udp_client(void *pvParameters)
{
    PARAMETER_t param;
    memcpy(&param, pvParameters, sizeof(PARAMETER_t));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(param.port),
        .sin_addr.s_addr = inet_addr(param.ipv4),
    };

    int fd = lwip_socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    LWIP_ASSERT("udp socket", fd >= 0);

    char buffer[xItemSize];
    xTaskNotifyGive(param.taskHandle);

    while (1) {
        size_t received = xMessageBufferReceive(xMessageBufferTrans,
                                                buffer,
                                                sizeof(buffer),
                                                portMAX_DELAY);
        if (received > 0) {
            (void)lwip_sendto(fd,
                              buffer,
                              received,
                              0,
                              (struct sockaddr *)&addr,
                              sizeof(addr));
        }
    }

    // Unreachable unless receive fails silently
}

esp_err_t udp_logging_init(char *ipaddr, unsigned long port, int16_t enableStdout)
{
    if (ipaddr == NULL || strcmp(ipaddr, "") == 0) {
        return ESP_OK;
    }

    xMessageBufferTrans = xMessageBufferCreate(xBufferSizeBytes);
    configASSERT(xMessageBufferTrans != NULL);

    PARAMETER_t param = {
        .port = (uint16_t)port,
        .taskHandle = xTaskGetCurrentTaskHandle(),
    };
    strncpy(param.ipv4, ipaddr, sizeof(param.ipv4) - 1);
    param.ipv4[sizeof(param.ipv4) - 1] = '\0';

    xTaskCreate(udp_client, "pm_udp_syslog", 6144, (void *)&param, 2, NULL);

    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    writeToStdout = enableStdout ? true : false;
    esp_log_set_vprintf(logging_vprintf);
    return ESP_OK;
}

int logging_vprintf(const char *fmt, va_list l)
{
    char *buffer = calloc(xItemSize, sizeof(char));
    assert(buffer != NULL);

    int buffer_len = vsnprintf(buffer, xItemSize, fmt, l);
    if (buffer_len > 0 && buffer_len < (int)xItemSize) {
        char *msg = calloc(xItemSize, sizeof(char));
        assert(msg != NULL);

        time_t now = time(NULL);
        struct tm now_utc;
        gmtime_r(&now, &now_utc);
        char now_str[32];
        strftime(now_str, sizeof(now_str), "%Y-%m-%dT%H:%M:%SZ", &now_utc);

        int msg_len = snprintf(msg,
                               xItemSize,
                               "<13>1 %s %s %s - 0001 - %s",
                               now_str,
                               wifiIpAddress,
                               PLANT_TAG,
                               buffer);
        if (msg_len > 0 && msg_len < (int)xItemSize) {
            (void)xMessageBufferSend(xMessageBufferTrans, msg, msg_len, 0);
        }
        free(msg);
    }

    free(buffer);

    if (writeToStdout) {
        return vprintf(fmt, l);
    }
    return 0;
}

void init_logging(void)
{
#ifdef CONFIG_ESP_SYSLOG_SERVER
    if (strlen(CONFIG_ESP_SYSLOG_SERVER) > 0) {
        (void)udp_logging_init(CONFIG_ESP_SYSLOG_SERVER, 514, 1);
    }
#endif
}
