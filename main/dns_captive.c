#include "dns_captive.h"

#include <string.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"
#include "esp_log.h"

static const char *TAG = "dns_cap";

// Captive portal DNS: replies to every A-record query with the AP's IP
// (192.168.4.1). The DNS message format we return:
//   [header copy with QR=1, RA=1, AA=1, ANCOUNT=1]
//   [question echoed]
//   [answer: name pointer 0xC00C, TYPE=A, CLASS=IN, TTL=60, RDLEN=4, RDATA=IP]

#define AP_IP_A  192
#define AP_IP_B  168
#define AP_IP_C  4
#define AP_IP_D  1

static void dns_task(void *arg) {
    (void)arg;
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket(): %d", errno);
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in saddr = {
        .sin_family = AF_INET,
        .sin_port   = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr*)&saddr, sizeof(saddr)) < 0) {
        ESP_LOGE(TAG, "bind(): %d", errno);
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "captive DNS on port 53");

    uint8_t buf[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t fl = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr*)&from, &fl);
        if (n < 12) continue;

        // Flags: set QR, AA, RA, RCODE=0.
        buf[2] = 0x84;
        buf[3] = 0x80;
        // QDCOUNT kept; ANCOUNT=1; NSCOUNT=0; ARCOUNT=0
        buf[4] = 0x00; buf[5] = 0x01; // QDCOUNT=1
        buf[6] = 0x00; buf[7] = 0x01; // ANCOUNT=1
        buf[8] = 0x00; buf[9] = 0x00;
        buf[10] = 0x00; buf[11] = 0x00;

        // Answer starts right after the question. Find end of question (label
        // sequence terminated by 0x00, then 4 bytes QTYPE+QCLASS).
        int p = 12;
        while (p < n && buf[p] != 0) {
            p += buf[p] + 1;
            if (p >= n) break;
        }
        if (p >= n) continue;
        p += 1 + 4; // terminator + QTYPE + QCLASS
        if (p + 16 > (int)sizeof(buf)) continue;

        // Name pointer to offset 12 (the start of the question NAME).
        buf[p++] = 0xC0; buf[p++] = 0x0C;
        // TYPE=A(0x0001)
        buf[p++] = 0x00; buf[p++] = 0x01;
        // CLASS=IN(0x0001)
        buf[p++] = 0x00; buf[p++] = 0x01;
        // TTL = 60s
        buf[p++] = 0x00; buf[p++] = 0x00; buf[p++] = 0x00; buf[p++] = 60;
        // RDLENGTH = 4
        buf[p++] = 0x00; buf[p++] = 0x04;
        // RDATA: AP IP
        buf[p++] = AP_IP_A; buf[p++] = AP_IP_B;
        buf[p++] = AP_IP_C; buf[p++] = AP_IP_D;

        sendto(sock, buf, p, 0, (struct sockaddr*)&from, fl);
    }
}

void dns_captive_start(void) {
    xTaskCreate(dns_task, "dns_cap", 4096, NULL, 4, NULL);
}
