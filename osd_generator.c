#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// UDP generator that animates values/texts and exercises runtime asset_updates.
// - Asset 0 bar color shifts through red/orange/yellow/green based on value.
// - Asset 6 is enabled on the fly as a bar bound to value[6].
// - Asset 7 is enabled on the fly as a text widget showing texts[7].
// Usage: ./osd_generator [ip] [port] [ms]
// Defaults: 127.0.0.1 7777 100
int main(int argc, char **argv)
{
    const char *ip = (argc > 1) ? argv[1] : "127.0.0.1";
    int port = (argc > 2) ? atoi(argv[2]) : 7777;
    int interval_ms = (argc > 3) ? atoi(argv[3]) : 100;
    if (interval_ms < 5) interval_ms = 5;

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid IP address: %s\n", ip);
        close(sock);
        return 1;
    }

    double v[8] = {0};
    v[1] = 1.0;
    v[2] = 98.0;
    double v3 = 0.0;
    double v4 = -50.0;
    double v5 = 0.0;
    int dir0 = 1;
    int dir2 = 1;
    int dir4 = 1;
    const char *texts[8] = {
        "TEXTCH_00_SAMPLE",
        "TEXTCH_01_SAMPLE",
        "TEXTCH_02_SAMPLE",
        "TEXTCH_03_SAMPLE",
        "TEXTCH_04_SAMPLE",
        "TEXTCH_05_SAMPLE",
        "TEXTCH_06_SAMPLE",
        "TEXTCH_07_SAMPLE",
    };

    uint32_t last_bar_color = 0;

    while (1) {
        char buf[768];
        char updates[512];
        int uoff = 0;
        int first_update = 1;

        uint32_t bar_color = 0xFF0000;
        if (v[0] >= 0.75) {
            bar_color = 0x00FF00; // green
        } else if (v[0] >= 0.5) {
            bar_color = 0xFFFF00; // yellow
        } else if (v[0] >= 0.25) {
            bar_color = 0xFFA500; // orange
        }

#define APPEND_UPDATE(fmt, ...) \
        do { \
            int add = snprintf(updates + uoff, sizeof(updates) - (size_t)uoff, \
                               first_update ? fmt : "," fmt, __VA_ARGS__); \
            if (add < 0 || add >= (int)(sizeof(updates) - (size_t)uoff)) { \
                fprintf(stderr, "Asset update buffer overflow\n"); \
                close(sock); \
                return 1; \
            } \
            uoff += add; \
            first_update = 0; \
        } while (0)

        // Enable asset 6 as a bar and bind it to value[6]
        APPEND_UPDATE("{\"id\":6,\"enabled\":true,\"type\":\"bar\",\"value_index\":6,\"label\":\"UDP BAR 6\","
                      "\"x\":10,\"y\":200,\"width\":300,\"height\":24,\"min\":0,\"max\":1,"
                      "\"bar_color\":%u,\"text_color\":16777215,\"background\":1,\"background_opacity\":60}",
                      0x00AA00u);

        // Enable asset 7 as a text widget fed by texts[7]
        APPEND_UPDATE("{\"id\":7,\"enabled\":true,\"type\":\"text\",\"text_indices\":[7],"
                      "\"text_inline\":true,\"label\":\"UDP TEXT 7\",\"x\":360,\"y\":200,\"width\":320,\"height\":60,"
                      "\"background\":3,\"background_opacity\":50,\"text_color\":16777215}");

        if (bar_color != last_bar_color) {
            APPEND_UPDATE("{\"id\":0,\"bar_color\":%u}", bar_color);
            last_bar_color = bar_color;
        }

#undef APPEND_UPDATE

        int len = snprintf(buf, sizeof(buf),
                           "{\"values\":[%.3f,%.3f,%.1f,%.1f,%.1f,%.1f,%.3f,%.3f],"
                           "\"texts\":[\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"]",
                           v[0], v[1], v[2], v3, v4, v5, 0.5, 0.0,
                           texts[0], texts[1], texts[2], texts[3],
                           texts[4], texts[5], texts[6], texts[7]);
        if (len < 0 || len >= (int)sizeof(buf)) {
            fprintf(stderr, "Failed to format payload\n");
            break;
        }

        if (uoff > 0) {
            int extra = snprintf(buf + len, sizeof(buf) - (size_t)len, ",\"asset_updates\":[%s]", updates);
            if (extra < 0 || len + extra >= (int)sizeof(buf)) {
                fprintf(stderr, "Failed to append asset updates\n");
                break;
            }
            len += extra;
        }

        if (len >= (int)sizeof(buf) - 1) {
            fprintf(stderr, "Buffer too small for payload\n");
            break;
        }

        buf[len++] = '}';
        buf[len] = '\0';

        ssize_t sent = sendto(sock, buf, (size_t)len, 0,
                              (struct sockaddr *)&addr, sizeof(addr));
        if (sent < 0) {
            perror("sendto");
            break;
        }

        v[0] += dir0 * 0.05;
        if (v[0] >= 1.0) {
            v[0] = 1.0;
            dir0 = -1;
        } else if (v[0] <= 0.0) {
            v[0] = 0.0;
            dir0 = 1;
        }

        v[1] = 1.0 - v[0];

        v[2] += dir2 * 2.5;
        if (v[2] >= 195.0) {
            v[2] = 195.0;
            dir2 = -1;
        } else if (v[2] <= 98.0) {
            v[2] = 98.0;
            dir2 = 1;
        }

        v3 += 2.0;
        if (v3 > 100.0) v3 = 0.0;

        v4 += dir4 * 2.5;
        if (v4 >= 50.0) {
            v4 = 50.0;
            dir4 = -1;
        } else if (v4 <= -50.0) {
            v4 = -50.0;
            dir4 = 1;
        }

        v5 += 10.0;
        if (v5 >= 360.0) v5 -= 360.0;

        usleep((useconds_t)interval_ms * 1000);
    }

    close(sock);
    return 0;
}
