#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// Contract-safe limits
#define MAX_TEXT_LEN 16

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s --dest <ip> [--port <n>] [--interval <ms>] <iface>\n"
            "\n"
            "Options:\n"
            "  --dest <ip>        REQUIRED destination IPv4 address\n"
            "  --port <n>         UDP port (default 7777)\n"
            "  --interval <ms>    Send interval in ms (default 100, min 5)\n"
            "  -h, --help         Show this help\n"
            "\n"
            "Example:\n"
            "  %s --dest 192.168.2.20 --interval 16 wlx40a5ef2f2308\n",
            prog, prog);
}

static uint32_t color_for_signal_dbm(double s)
{
    // Direct dBm cutoffs
    if (s >= -50.0) return 0x00FF00; // green
    if (s >= -60.0) return 0xFFFF00; // yellow
    if (s >= -70.0) return 0xFFA500; // orange
    return 0xFF0000;                 // red
}

static void clamp_ssid_16(const char *in, char out[MAX_TEXT_LEN + 1])
{
    if (!in) {
        out[0] = '\0';
        return;
    }
    strncpy(out, in, MAX_TEXT_LEN);
    out[MAX_TEXT_LEN] = '\0';
}

static int read_iw_link(const char *iface, char *ssid_out, size_t ssid_sz, double *signal_dbm_out)
{
    if (!iface || !ssid_out || ssid_sz == 0 || !signal_dbm_out)
        return 0;

    ssid_out[0] = '\0';
    *signal_dbm_out = 0.0;

    char cmd[256];
    snprintf(cmd, sizeof(cmd), "iw dev %s link 2>&1", iface);

    FILE *fp = popen(cmd, "r");
    if (!fp) return 0;

    char line[256];
    int have_ssid = 0;
    int have_signal = 0;

    while (fgets(line, sizeof(line), fp)) {
        char *p = strstr(line, "SSID:");
        if (p) {
            p += 5;
            while (*p == ' ' || *p == '\t') p++;
            size_t n = strcspn(p, "\r\n");
            if (n >= ssid_sz) n = ssid_sz - 1;
            memcpy(ssid_out, p, n);
            ssid_out[n] = '\0';
            if (ssid_out[0] != '\0')
                have_ssid = 1;
            continue;
        }

        p = strstr(line, "signal:");
        if (p) {
            double s = 0.0;
            if (sscanf(p, "signal: %lf", &s) == 1) {
                *signal_dbm_out = s;
                have_signal = 1;
            }
            continue;
        }

        if (strstr(line, "Not connected") || strstr(line, "not connected")) {
            have_ssid = 0;
            have_signal = 0;
        }
    }

    pclose(fp);
    return (have_ssid && have_signal);
}

static long monotonic_sec(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec;
}

int main(int argc, char **argv)
{
    const char *dest_ip = NULL;
    int port = 7777;
    int interval_ms = 100;

    static struct option long_opts[] = {
        {"dest", required_argument, 0, 1},
        {"port", required_argument, 0, 2},
        {"interval", required_argument, 0, 3},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };

    int opt, idx = 0;
    while ((opt = getopt_long(argc, argv, "h", long_opts, &idx)) != -1) {
        switch (opt) {
        case 1:
            dest_ip = optarg;
            break;
        case 2:
            port = atoi(optarg);
            break;
        case 3:
            interval_ms = atoi(optarg);
            break;
        case 'h':
        default:
            usage(argv[0]);
            return (opt == 'h') ? 0 : 1;
        }
    }

    if (!dest_ip) {
        fprintf(stderr, "Error: --dest is required.\n");
        usage(argv[0]);
        return 1;
    }

    if (port <= 0 || port > 65535) {
        fprintf(stderr, "Error: invalid --port value.\n");
        return 1;
    }

    if (interval_ms < 5) interval_ms = 5;

    if (optind >= argc) {
        fprintf(stderr, "Error: missing <iface>.\n");
        usage(argv[0]);
        return 1;
    }

    const char *iface = argv[optind];

    int sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);

    if (inet_pton(AF_INET, dest_ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid IP address: %s\n", dest_ip);
        close(sock);
        return 1;
    }

    uint32_t last_color[4] = {0, 0, 0, 0};
    long last_debug_sec = -1;

    while (1) {
        char ssid_raw[128];
        double signal_dbm = 0.0;

        int ok = read_iw_link(iface, ssid_raw, sizeof(ssid_raw), &signal_dbm);

        char ssid16[MAX_TEXT_LEN + 1];
        if (ok) {
            clamp_ssid_16(ssid_raw, ssid16);
            if (ssid16[0] == '\0') {
                strncpy(ssid16, "CONNECTED", sizeof(ssid16));
                ssid16[MAX_TEXT_LEN] = '\0';
            }
        } else {
            strncpy(ssid16, "DISCONNECTED", sizeof(ssid16));
            ssid16[MAX_TEXT_LEN] = '\0';
            signal_dbm = 0.0;
        }

        double v[8] = {0};
        if (ok) {
            v[0] = signal_dbm;
            v[1] = signal_dbm;
            v[2] = signal_dbm;
            v[3] = signal_dbm;
        }

        uint32_t c = ok ? color_for_signal_dbm(signal_dbm) : 0xFF0000;

        // Original-style strict buffer size
        char buf[512];

        int len = snprintf(buf, sizeof(buf),
                           "{\"values\":[%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.1f],"
                           "\"texts\":[\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"]",
                           v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7],
                           ssid16, ssid16, ssid16, ssid16, ssid16, ssid16, ssid16, ssid16);

        if (len < 0 || len >= (int)sizeof(buf)) {
            fprintf(stderr, "Failed to format payload\n");
            break;
        }

        int any_color_change = 0;
        for (int i = 0; i < 4; i++) {
            if (last_color[i] != c) { any_color_change = 1; break; }
        }

        if (any_color_change) {
            int extra = snprintf(buf + len, sizeof(buf) - (size_t)len,
                                 ",\"asset_updates\":["
                                 "{\"id\":0,\"bar_color\":%u},"
                                 "{\"id\":1,\"bar_color\":%u},"
                                 "{\"id\":2,\"bar_color\":%u},"
                                 "{\"id\":3,\"bar_color\":%u}"
                                 "]",
                                 c, c, c, c);

            if (extra < 0 || len + extra >= (int)sizeof(buf)) {
                fprintf(stderr, "Failed to append asset update\n");
                break;
            }
            len += extra;

            for (int i = 0; i < 4; i++)
                last_color[i] = c;
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

        long now_sec = monotonic_sec();
        if (now_sec != last_debug_sec) {
            last_debug_sec = now_sec;
            fprintf(stderr,
                    "[waybeam_sender] dst=%s:%d iface=%s ok=%d ssid=\"%s\" "
                    "signal=%.1f dBm values0-3=[%.1f %.1f %.1f %.1f] color=0x%06X len=%d interval=%dms\n",
                    dest_ip, port, iface, ok, ssid16,
                    signal_dbm, v[0], v[1], v[2], v[3],
                    (unsigned)c, len, interval_ms);
        }

        usleep((useconds_t)interval_ms * 1000);
    }

    close(sock);
    return 0;
}
