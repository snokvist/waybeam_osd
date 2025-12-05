#include <arpa/inet.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// Simple UDP generator that animates a full 8-value payload at ~10 Hz.
// value[0]: 0..1 triangle wave (bar)
// value[1]: 1..0 triangle wave (example_bar_2)
// value[2]: 98..195 ramp (spare)
// value[3]: 0..100 ramp (spare bar)
// value[4]: -50..50 triangle (spare bar)
// value[5]: 0..360 wrap (e.g. degrees)
// value[6]: steady 0.5
// value[7]: steady 0.0
// texts[i]: 16-char sample descriptors for each channel
// Usage: ./bar_generator [ip] [port] [ms]
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
        char buf[512];
        uint32_t bar_color = 0xFF0000;
        if (v[0] >= 0.75) {
            bar_color = 0x00FF00; // green
        } else if (v[0] >= 0.5) {
            bar_color = 0xFFFF00; // yellow
        } else if (v[0] >= 0.25) {
            bar_color = 0xFFA500; // orange
        }

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

        if (bar_color != last_bar_color) {
            int extra = snprintf(buf + len, sizeof(buf) - (size_t)len,
                                  ",\"asset_updates\":[{\"id\":0,\"bar_color\":%u}]",
                                  bar_color);
            if (extra < 0 || len + extra >= (int)sizeof(buf)) {
                fprintf(stderr, "Failed to append asset update\n");
                break;
            }
            len += extra;
            last_bar_color = bar_color;
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
