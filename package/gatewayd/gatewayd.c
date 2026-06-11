/**
 * gatewayd.c — Lightweight Wyze Sense Hub replacement for CC1310 radio
 *
 * Protocol: AA 55 53 LEN CMD [PAYLOAD...] CHK_HI CHK_LO
 *   LEN = payload_len + 3  (CMD(1) + payload(N) + CHK(2))
 *   CHK = 16-bit big-endian sum of ALL bytes before CHK
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>
#include <signal.h>
#include <errno.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/select.h>
#include <time.h>
#include <ctype.h>
#include <stdarg.h>

#define SOF1            0xAA
#define SOF2            0x55
#define PROTO_CC1310    0x53
#define PROTO_BLE       0x63

#define SUB_VER_REQ         0x16
#define SUB_BAT_REQ         0x57
#define SUB_SENSOR_SUM      0x2E
#define SUB_SENSOR_LIST     0x30
#define SUB_TS_CMD          0x33
#define KEYPAD_STATUS_RESP  0x53
#define PAIR_SCAN           0x1C
#define PAIR_VERIFY         0x23
#define UNPAIR_SENSOR       0x25

#define CMD_VER_RESP        0x17
#define CMD_BAT_RESP        0x58
#define CMD_SENSOR_SUM_ACK  0x2F
#define CMD_SENSOR_MAC      0x31
#define RX_SENSOR_REPORT    0x19
#define RX_SENSOR_ALARM     0x1A
#define CMD_KEYPAD_REQ      0x55
#define CMD_PAIR_BROADCAST  0x20

#define MAX_PKT 512
#define MAX_SENSORS 32

static int uart_fd = -1;
static volatile int running = 1;
static char hub_mac[18] = {0};
static char sensor_macs[MAX_SENSORS][17] = {{0}};
static int sensor_mac_count = 0;
static char mqtt_host[128] = "localhost";
static int mqtt_port = 1883;
static char mqtt_user[64] = {0};
static char mqtt_pass[64] = {0};
static int mqtt_ssl = 0;
static int gpio_cc1310_rst = 62;
static int gpio_cc1310_boot = 61;
static int gpio_bg21_rst = 58;
static int gpio_battery_power = 6;

typedef enum {
    STATE_UNINIT = 0,
    STATE_VER_REQ_SENT,
    STATE_VER_RCVD,
    STATE_BAT_RCVD,
    STATE_SUM_RCVD,
    STATE_MONITORING
} init_state_t;

static init_state_t init_state = STATE_UNINIT;

static const char *state_name(init_state_t s) {
    switch (s) {
        case STATE_UNINIT:      return "UNINIT";
        case STATE_VER_REQ_SENT: return "VER_REQ_SENT";
        case STATE_VER_RCVD:     return "VER_RCVD";
        case STATE_BAT_RCVD:     return "BAT_RCVD";
        case STATE_SUM_RCVD:     return "SUM_RCVD";
        case STATE_MONITORING:   return "MONITORING";
        default:                 return "UNKNOWN";
    }
}

typedef struct {
    uint8_t proto;
    uint8_t cmd;
    uint8_t payload[MAX_PKT];
    int payload_len;
} cc1310_pkt_t;

static void log_hex(const char *label, const uint8_t *buf, int len) {
    fprintf(stderr, "[dbg] %s (%d bytes):", label, len);
    for (int i = 0; i < len && i < 128; i++)
        fprintf(stderr, " %02X", buf[i]);
    if (len > 128) fprintf(stderr, " ...");
    fprintf(stderr, "\n");
}

static void append_chk(uint8_t *buf, int len) {
    uint16_t sum = 0;
    for (int i = 0; i < len; i++) sum += buf[i];
    buf[len]     = (sum >> 8) & 0xFF;
    buf[len + 1] = sum & 0xFF;
    fprintf(stderr, "[dbg] append_chk: sum(0..%d)=0x%04X -> [%d]=0x%02X [%d]=0x%02X\n",
            len - 1, sum, len, buf[len], len + 1, buf[len + 1]);
}

static int build_pkt(uint8_t *buf, uint8_t cmd, const uint8_t *payload, int plen) {
    int pos = 0;
    buf[pos++] = SOF1; buf[pos++] = SOF2; buf[pos++] = PROTO_CC1310;
    buf[pos++] = plen + 3;      /* LEN = CMD(1) + payload(N) + CHK(2) */
    buf[pos++] = cmd;
    for (int i = 0; i < plen; i++) {
        buf[pos++] = payload[i];
    }
    append_chk(buf, pos);
    int total = pos + 2;
    log_hex("build_pkt output", buf, total);
    return total;
}

static void send_ack(uint8_t cmd) {
    uint8_t buf[8];
    int pos = 0;
    buf[pos++] = SOF1;
    buf[pos++] = SOF2;
    buf[pos++] = PROTO_CC1310;
    buf[pos++] = cmd;
    buf[pos++] = 0xFF; /* ACK indicator */
    append_chk(buf, pos);
    int len = pos + 2;
    fprintf(stderr, "[dbg] send_ack: cmd=0x%02X len=%d\n", cmd, len);
    int n = write(uart_fd, buf, len);
    if (n != len)
        fprintf(stderr, "[uart] send_ack WARNING: wrote %d of %d bytes (errno=%d)\n", n, len, errno);
}

static void send_timestamp(void) {
    uint64_t ms = (uint64_t)time(NULL) * 1000;
    uint8_t ts[8];
    for (int i = 0; i < 8; i++)
        ts[i] = (ms >> (56 - i * 8)) & 0xFF;
    uint8_t buf[32];
    int len = build_pkt(buf, SUB_TS_CMD, ts, 8);
    fprintf(stderr, "[dbg] send_timestamp: t=%lu ms buf_len=%d\n", (unsigned long)ms, len);
    log_hex("send_timestamp packet", buf, len);
    write(uart_fd, buf, len);
}

static int parse_packet(uint8_t *buf, int *len, cc1310_pkt_t *pkt) {
    if (*len < 7) {
        return 0;
    }

    int sync_idx = -1;
    for (int i = 0; i <= *len - 3; i++) {
        if (buf[i] == 0x55 && buf[i+1] == 0xAA && buf[i+2] == PROTO_CC1310) {
            sync_idx = i;
            break;
        }
    }

    if (sync_idx > 0) {
        fprintf(stderr, "[dbg] parse: found sync at idx=%d, discarding %d garbage bytes\n",
                sync_idx, sync_idx);
        memmove(buf, buf + sync_idx, *len - sync_idx);
        *len -= sync_idx;
    } else if (sync_idx == -1) {
        if (*len > 2) {
            memmove(buf, buf + *len - 2, 2);
            *len = 2;
        }
        return 0;
    }

    if (*len < 7) {
        return 0;
    }

    uint8_t byte3 = buf[3]; // LEN
    uint8_t byte4 = buf[4]; // CMD or ACK (0xFF)

    if (byte4 == 0xFF) {
        uint16_t chk_received = (buf[5] << 8) | buf[6];
        uint16_t sum = 0;
        for (int j = 0; j < 5; j++) sum += buf[j];

        fprintf(stderr, "[dbg] parse: ACK candidate cmd=0x%02X sum=0x%04X chk_rcvd=0x%04X\n",
                byte3, sum, chk_received);

        if (sum == chk_received) {
            pkt->proto = buf[2];
            pkt->cmd = byte3;
            pkt->payload_len = 0;
            int consumed = 7;
            fprintf(stderr, "[dbg] parse: ACK OK cmd=0x%02X consumed=%d\n", byte3, consumed);
            memmove(buf, buf + consumed, *len - consumed);
            *len -= consumed;
            return 1;
        }
        fprintf(stderr, "[dbg] parse: ACK checksum FAIL sum=0x%04X != chk=0x%04X, discarding 1 byte\n",
                sum, chk_received);
        memmove(buf, buf + 1, *len - 1);
        (*len)--;
        return 0;
    } else {
        if (byte3 < 3) {
            fprintf(stderr, "[dbg] parse: invalid LEN %d, discarding 1 byte\n", byte3);
            memmove(buf, buf + 1, *len - 1);
            (*len)--;
            return 0;
        }

        int data_len = byte3 - 3;
        int total_len = 4 + byte3;

        if (*len < total_len) {
            fprintf(stderr, "[dbg] parse: need %d bytes, have %d, waiting\n", total_len, *len);
            return 0;
        }

        uint16_t chk_received = (buf[total_len-2] << 8) | buf[total_len-1];
        uint16_t sum = 0;
        for (int j = 0; j < total_len - 2; j++) sum += buf[j];

        fprintf(stderr, "[dbg] parse: DATA candidate cmd=0x%02X data_len=%d sum=0x%04X chk_rcvd=0x%04X\n",
                byte4, data_len, sum, chk_received);

        if (sum == chk_received) {
            pkt->proto = buf[2];
            pkt->cmd = byte4;
            pkt->payload_len = data_len;
            if (data_len > 0) {
                memcpy(pkt->payload, &buf[5], data_len);
            }
            int consumed = total_len;
            fprintf(stderr, "[dbg] parse: DATA OK cmd=0x%02X payload_len=%d consumed=%d\n",
                    pkt->cmd, pkt->payload_len, consumed);
            memmove(buf, buf + consumed, *len - consumed);
            *len -= consumed;
            return 1;
        }

        fprintf(stderr, "[dbg] parse: DATA checksum FAIL sum=0x%04X != chk=0x%04X, discarding 1 byte\n",
                sum, chk_received);
        memmove(buf, buf + 1, *len - 1);
        (*len)--;
        return 0;
    }
}

static void mqtt_publish(const char *topic, const char *message, int retain) {
    if (!mqtt_host[0]) { fprintf(stderr, "[mqtt] no host configured, skipping publish\n"); return; }
    char cmd[1024];
    int n = snprintf(cmd, sizeof(cmd),
        "mosquitto_pub -h '%s' -p %d 2>/dev/null", mqtt_host, mqtt_port);
    if (mqtt_user[0]) n += snprintf(cmd+n, sizeof(cmd)-n, " -u '%s'", mqtt_user);
    if (mqtt_pass[0]) n += snprintf(cmd+n, sizeof(cmd)-n, " -P '%s'", mqtt_pass);
    if (mqtt_ssl)     n += snprintf(cmd+n, sizeof(cmd)-n, " --capath /etc/ssl/certs");
    if (retain)       n += snprintf(cmd+n, sizeof(cmd)-n, " -r");
    n += snprintf(cmd+n, sizeof(cmd)-n, " -t '%s' -m '%s'", topic, message);
    fprintf(stderr, "[mqtt] publish: topic='%s' retain=%d cmdlen=%d\n", topic, retain, n);
    if (n >= (int)sizeof(cmd))
        fprintf(stderr, "[mqtt] WARNING: command truncated!\n");
    int ret = system(cmd);
    if (ret != 0)
        fprintf(stderr, "[mqtt] publish returned %d\n", ret);
}

static void ha_discovery_sensor(const char *sensor_mac,
                                const char *device_class) {
    char unique_id[64], topic[256], payload[2048];
    snprintf(unique_id, sizeof(unique_id), "gateway_%s_%s", hub_mac, sensor_mac);
    snprintf(topic, sizeof(topic),
        "homeassistant/binary_sensor/%s/config", unique_id);

    char dev_json[256];
    snprintf(dev_json, sizeof(dev_json),
        "{\"identifiers\":[\"gateway_%s\"],"
        "\"name\":\"Wyze Hub Gateway\","
        "\"model\":\"Wyze Sense Hub GW3U\","
        "\"manufacturer\":\"Wyze/Thingino\"}",
        hub_mac);

    snprintf(payload, sizeof(payload),
        "{\"name\":\"Sensor %s\","
        "\"unique_id\":\"%s\","
        "\"state_topic\":\"gateways/%s/sensors/%s/state\","
        "\"availability_topic\":\"gateways/%s/status\","
        "\"payload_available\":\"online\","
        "\"payload_not_available\":\"offline\","
        "%s%s%s"
        "\"device\":%s}",
        sensor_mac, unique_id,
        hub_mac, sensor_mac,
        hub_mac,
        device_class ? "\"device_class\":\"" : "",
        device_class ? device_class : "",
        device_class ? "\"," : "",
        dev_json);

    fprintf(stderr, "[ha] discovery: sensor=%s class=%s\n", sensor_mac,
            device_class ? device_class : "none");
    mqtt_publish(topic, payload, 1);
}

static void publish_sensor_event(const char *sensor_mac,
                                 const char *class_str,
                                 const char *state_str,
                                 int battery_pct, int rssi) {
    char topic[256], payload[512];
    snprintf(topic, sizeof(topic),
        "gateways/%s/sensors/%s/state", hub_mac, sensor_mac);
    snprintf(payload, sizeof(payload),
        "{\"state\":\"%s\",\"class\":\"%s\",\"battery\":%d,\"rssi\":%d}",
        state_str, class_str, battery_pct, rssi);
    fprintf(stderr, "[sensor] publish: %s -> %s (%s batt=%d%% rssi=%d)\n",
            sensor_mac, state_str, class_str, battery_pct, rssi);
    mqtt_publish(topic, payload, 0);

    snprintf(topic, sizeof(topic),
        "gateways/%s/sensors/%s/battery", hub_mac, sensor_mac);
    char batt_str[16]; snprintf(batt_str, sizeof(batt_str), "%d", battery_pct);
    mqtt_publish(topic, batt_str, 1);

    snprintf(topic, sizeof(topic),
        "gateways/%s/sensors/%s/rssi", hub_mac, sensor_mac);
    char rssi_str[16]; snprintf(rssi_str, sizeof(rssi_str), "%d", rssi);
    mqtt_publish(topic, rssi_str, 1);
}

static void publish_availability(const char *status) {
    char topic[128];
    snprintf(topic, sizeof(topic), "gateways/%s/status", hub_mac);
    fprintf(stderr, "[mqtt] publish_availability: %s\n", status);
    mqtt_publish(topic, status, 1);
}

static int mac_from_sys(void) {
    FILE *f;
    const char *ifaces[] = {"eth0", "wlan0", "usb0", NULL};
    for (int i = 0; ifaces[i]; i++) {
        char path[64];
        snprintf(path, sizeof(path), "/sys/class/net/%s/address", ifaces[i]);
        f = fopen(path, "r");
        if (f) {
            if (fgets(hub_mac, sizeof(hub_mac), f)) {
                for (char *p = hub_mac; *p; p++) *p = toupper((unsigned char)*p);
                hub_mac[strcspn(hub_mac, "\n")] = 0;
            }
            fclose(f);
            if (hub_mac[0]) {
                fprintf(stderr, "[config] MAC from %s: %s\n", ifaces[i], hub_mac);
                return 1;
            }
        }
    }
    strcpy(hub_mac, "UNKNOWN");
    fprintf(stderr, "[config] MAC not found, using UNKNOWN\n");
    return 0;
}

static char *json_strdup_until(const char *start, const char *end) {
    int len = end - start;
    char *r = malloc(len + 1);
    if (r) { memcpy(r, start, len); r[len] = 0; }
    return r;
}

static char *json_find_value(const char *json, const char *key) {
    const char *p = json, *found;
    while ((found = strstr(p, key))) {
        const char *after_key = found + strlen(key);
        while (*after_key == ' ' || *after_key == '\t' || *after_key == ':' || *after_key == '"') after_key++;
        if (*after_key == '"') {
            const char *val_start = after_key + 1;
            const char *val_end = strchr(val_start, '"');
            if (val_end) return json_strdup_until(val_start, val_end);
        } else if (*after_key == 't' || *after_key == 'f') {
            if (strncmp(after_key, "true", 4) == 0) return strdup("true");
            if (strncmp(after_key, "false", 5) == 0) return strdup("false");
        } else if (*after_key == '-' || (*after_key >= '0' && *after_key <= '9')) {
            const char *num_end = after_key;
            while (*num_end && (isdigit((unsigned char)*num_end) || *num_end == '.' || *num_end == '-')) num_end++;
            return json_strdup_until(after_key, num_end);
        }
        p = found + 1;
    }
    return NULL;
}

static char *read_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "[config] cannot open %s\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    if (len <= 0) { fclose(f); fprintf(stderr, "[config] %s is empty\n", path); return NULL; }
    char *buf = malloc(len + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t r = fread(buf, 1, len, f);
    fclose(f);
    buf[r] = 0;
    fprintf(stderr, "[config] read %s (%zu bytes)\n", path, r);
    return buf;
}

static char *json_search_in(const char *json, const char *key) {
    if (!json) return NULL;
    const char *p = json;
    while ((p = strstr(p, key))) {
        const char *after = p + strlen(key);
        while (*after == ' ' || *after == '\t' || *after == ':' || *after == '"') after++;
        if (*after == '"') {
            const char *vs = after + 1, *ve = strchr(vs, '"');
            if (ve) return json_strdup_until(vs, ve);
        } else if (*after == 't' || *after == 'f') {
            if (strncmp(after, "true", 4)==0) return strdup("true");
            if (strncmp(after, "false", 5)==0) return strdup("false");
        } else if (*after == '-' || (*after >= '0' && *after <= '9')) {
            const char *ne = after;
            while (*ne && (isdigit((unsigned char)*ne) || *ne=='.' || *ne=='-')) ne++;
            return json_strdup_until(after, ne);
        }
        p++;
    }
    return NULL;
}

static char *json_find_nested(const char *json, const char *parent, const char *key) {
    if (!json) return NULL;
    const char *p = json;
    while ((p = strstr(p, parent))) {
        const char *after = p + strlen(parent);
        while (*after == ' ' || *after == '\t' || *after == ':') after++;
        if (*after == '{') {
            const char *block_start = after + 1;
            int depth = 1;
            const char *scan = block_start;
            while (*scan && depth > 0) {
                if (*scan == '{') depth++;
                else if (*scan == '}') depth--;
                scan++;
            }
            if (depth == 0) {
                int blen = scan - block_start - 1;
                char *block = strndup(block_start, blen);
                if (block) {
                    char *val = json_search_in(block, key);
                    free(block);
                    if (val) return val;
                }
            }
        }
        p = after;
    }
    return NULL;
}

static void read_config(void) {
    fprintf(stderr, "[config] reading configuration...\n");
    char *json = read_file("/etc/thingino.json");
    if (!json) json = read_file("/rom/thingino.json");
    if (!json) { fprintf(stderr, "[config] no config file found, using defaults\n"); return; }

    fprintf(stderr, "[config] json contents:\n%s\n", json);

    char *v;

    v = json_find_nested(json, "gatewayd", "mqtt_host");
    if (!v) v = json_find_value(json, "mqtt_host");
    if (v) { strncpy(mqtt_host, v, sizeof(mqtt_host)-1); fprintf(stderr, "[config] mqtt_host=%s\n", mqtt_host); free(v); }

    v = json_find_nested(json, "gatewayd", "mqtt_port");
    if (!v) v = json_find_value(json, "mqtt_port");
    if (v) { mqtt_port = atoi(v); fprintf(stderr, "[config] mqtt_port=%d\n", mqtt_port); free(v); }

    v = json_find_nested(json, "gatewayd", "mqtt_user");
    if (!v) v = json_find_value(json, "mqtt_user");
    if (v) { strncpy(mqtt_user, v, sizeof(mqtt_user)-1); fprintf(stderr, "[config] mqtt_user=%s\n", mqtt_user); free(v); }

    v = json_find_nested(json, "gatewayd", "mqtt_pass");
    if (!v) v = json_find_value(json, "mqtt_pass");
    if (v) { strncpy(mqtt_pass, v, sizeof(mqtt_pass)-1); fprintf(stderr, "[config] mqtt_pass=****\n"); free(v); }

    v = json_find_nested(json, "gatewayd", "mqtt_ssl");
    if (!v) v = json_find_value(json, "mqtt_ssl");
    if (v) { mqtt_ssl = (strcmp(v,"true")==0); fprintf(stderr, "[config] mqtt_ssl=%d\n", mqtt_ssl); free(v); }

    v = json_find_nested(json, "cc1310", "reset");
    if (v) { gpio_cc1310_rst = atoi(v); fprintf(stderr, "[config] cc1310.reset=GPIO%d\n", gpio_cc1310_rst); free(v); }

    v = json_find_nested(json, "cc1310", "boot");
    if (v) { gpio_cc1310_boot = atoi(v); fprintf(stderr, "[config] cc1310.boot=GPIO%d\n", gpio_cc1310_boot); free(v); }

    v = json_find_nested(json, "cc1310", "aux_reset");
    if (v) { gpio_bg21_rst = atoi(v); fprintf(stderr, "[config] cc1310.aux_reset=GPIO%d\n", gpio_bg21_rst); free(v); }

    v = json_find_nested(json, "gpio", "battery_power");
    if (v) { gpio_battery_power = atoi(v); fprintf(stderr, "[config] gpio.battery_power=GPIO%d\n", gpio_battery_power); free(v); }

    free(json);
    fprintf(stderr, "[config] done\n");
}

static void gpio_export(int gpio) {
    char path[64];
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d", gpio);
    if (access(path, F_OK) == 0) {
        fprintf(stderr, "[gpio] GPIO%d already exported\n", gpio);
        return;
    }

    int fd = open("/sys/class/gpio/export", O_WRONLY);
    if (fd >= 0) {
        dprintf(fd, "%d", gpio);
        close(fd);
        usleep(100000);
        fprintf(stderr, "[gpio] exported GPIO%d\n", gpio);
    } else {
        fprintf(stderr, "[gpio] WARNING: cannot export GPIO%d (errno=%d)\n", gpio, errno);
    }
}

static void gpio_set(int gpio, int val) {
    char path[64];
    gpio_export(gpio);

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", gpio);
    int fd = open(path, O_WRONLY);
    if (fd >= 0) { write(fd, "out", 3); close(fd); }

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", gpio);
    fd = open(path, O_WRONLY);
    if (fd >= 0) {
        dprintf(fd, "%d", val);
        close(fd);
        fprintf(stderr, "[gpio] GPIO%d = %d\n", gpio, val);
    } else {
        fprintf(stderr, "[gpio] WARNING: cannot set GPIO%d (errno=%d)\n", gpio, errno);
    }
}

static void cc1310_reset(void) {
    fprintf(stderr, "[gpio] CC1310 reset sequence starting\n");
    fprintf(stderr, "[gpio]   reset=GPIO%d boot=GPIO%d battery_power=GPIO%d\n",
            gpio_cc1310_rst, gpio_cc1310_boot, gpio_battery_power);

    if (gpio_battery_power >= 0) {
        fprintf(stderr, "[gpio] enabling battery_power GPIO%d\n", gpio_battery_power);
        gpio_set(gpio_battery_power, 1);
        usleep(50000);
    }

    if (gpio_cc1310_rst < 0) {
        fprintf(stderr, "[gpio] CC1310 reset GPIO not configured, skipping\n");
        return;
    }

    fprintf(stderr, "[gpio] setting BOOT=HIGH (bootloader mode)\n");
    gpio_set(gpio_cc1310_boot, 1);
    usleep(10000);

    fprintf(stderr, "[gpio] pulsing RESET LOW\n");
    gpio_set(gpio_cc1310_rst, 0);
    usleep(100000);
    fprintf(stderr, "[gpio] releasing RESET HIGH\n");
    gpio_set(gpio_cc1310_rst, 1);
    usleep(300000);

    fprintf(stderr, "[gpio] CC1310 reset complete\n");
}

static void bg21_reset(void) {
    if (gpio_bg21_rst < 0) return;
    fprintf(stderr, "[gpio] resetting BG21 BLE module (GPIO%d)...\n", gpio_bg21_rst);
    gpio_set(gpio_bg21_rst, 0);
    usleep(100000);
    gpio_set(gpio_bg21_rst, 1);
    fprintf(stderr, "[gpio] BG21 reset complete\n");
}

static int uart_open(const char *dev, int baud) {
    fprintf(stderr, "[uart] opening %s at %d baud\n", dev, baud);

    int fd = open(dev, O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd < 0) {
        fprintf(stderr, "[uart] failed to open %s, loading jz-uart module\n", dev);
        system("modprobe jz-uart 2>/dev/null");
        usleep(200000);
        fd = open(dev, O_RDWR | O_NOCTTY | O_NDELAY);
        if (fd < 0) {
            fprintf(stderr, "[uart] ERROR: still cannot open %s (errno=%d)\n", dev, errno);
            return -1;
        }
    }

    struct termios tio;
    memset(&tio, 0, sizeof(tio));
    cfmakeraw(&tio);
    tio.c_cflag = CREAD | CLOCAL | CS8;
    tio.c_cc[VMIN] = 1;
    tio.c_cc[VTIME] = 1;
    speed_t s = (baud == 115200) ? B115200 : B9600;
    cfsetispeed(&tio, s);
    cfsetospeed(&tio, s);
    tcsetattr(fd, TCSANOW, &tio);
    tcflush(fd, TCIOFLUSH);

    fprintf(stderr, "[uart] opened fd=%d at speed=%d\n", fd, baud);
    uart_fd = fd;
    return fd;
}

static char *find_uart(void) {
    for (int i = 0; i < 16; i++) {
        char path[32];
        snprintf(path, sizeof(path), "/dev/ttyS%d", i);
        int fd = open(path, O_RDWR | O_NOCTTY | O_NDELAY);
        if (fd >= 0) {
            close(fd);
            char *r = strdup(path);
            fprintf(stderr, "[uart] found available UART: %s\n", r);
            return r;
        }
    }
    fprintf(stderr, "[uart] no UART devices found\n");
    return NULL;
}

static void sig_handler(int sig) {
    fprintf(stderr, "[gatewayd] signal %d received, stopping\n", sig);
    running = 0;
}

static void daemonize(void) {
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); exit(1); }
    if (pid > 0) {
        FILE *pf = fopen("/var/run/gatewayd.pid", "w");
        if (pf) { fprintf(pf, "%d", pid); fclose(pf); }
        fprintf(stderr, "[gatewayd] daemonized, pid=%d\n", pid);
        exit(0);
    }
    setsid();
    chdir("/");
    close(0); close(1); close(2);
    open("/dev/null", O_RDWR);
    dup(0); dup(0);
}

static const char *class_name(uint8_t sclass) {
    switch (sclass) {
        case 0x01: return "CONTACT";
        case 0x02: return "MOTION";
        case 0x03: return "LEAK";
        default:   return "?";
    }
}

int main(int argc, char **argv) {
    const char *uart_dev = NULL;
    int do_reset = 1;
    int do_bg21 = 0;
    int dmode = 0;

    fprintf(stderr, "[gatewayd] === starting ===\n");
    fprintf(stderr, "[gatewayd] PID=%d\n", getpid());

    read_config();

    for (int i = 1; i < argc; i++) {
        fprintf(stderr, "[gatewayd] argv[%d] = '%s'\n", i, argv[i]);
        if (strcmp(argv[i], "--no-reset") == 0)  { do_reset = 0; fprintf(stderr, "[gatewayd] reset disabled\n"); }
        else if (strcmp(argv[i], "--reset-bg21") == 0) { do_bg21 = 1; fprintf(stderr, "[gatewayd] BG21 reset enabled\n"); }
        else if (strcmp(argv[i], "--daemon") == 0) { dmode = 1; fprintf(stderr, "[gatewayd] daemon mode\n"); }
        else if (argv[i][0] != '-') { uart_dev = argv[i]; fprintf(stderr, "[gatewayd] UART device from args: %s\n", uart_dev); }
    }

    mac_from_sys();
    if (dmode) daemonize();

    fprintf(stderr, "[gatewayd] hub mac: %s\n", hub_mac);
    fprintf(stderr, "[gatewayd] MQTT broker: %s:%d\n", mqtt_host, mqtt_port);

    if (do_reset) {
        fprintf(stderr, "[gatewayd] resetting CC1310...\n");
        cc1310_reset();
        if (do_bg21) {
            fprintf(stderr, "[gatewayd] resetting BG21...\n");
            bg21_reset();
        }
    } else {
        fprintf(stderr, "[gatewayd] skipping CC1310 reset (--no-reset)\n");
    }

    if (!uart_dev) {
        char *found = find_uart();
        if (found) {
            uart_dev = found;
            fprintf(stderr, "[uart] using auto-detected %s\n", uart_dev);
        } else {
            uart_dev = "/dev/ttyS0";
            fprintf(stderr, "[uart] no UART found, falling back to %s\n", uart_dev);
        }
    }

    fprintf(stderr, "[uart] opening UART...\n");
    int fd = uart_open(uart_dev, 115200);
    if (fd < 0) {
        fprintf(stderr, "[uart] ERROR: cannot open %s (errno=%d)\n", uart_dev, errno);
        return 1;
    }
    fprintf(stderr, "[uart] opened %s on fd=%d\n", uart_dev, fd);

    int wdt_fd = open("/dev/watchdog", O_WRONLY);
    if (wdt_fd >= 0) {
        fprintf(stderr, "[watchdog] opened /dev/watchdog on fd=%d\n", wdt_fd);
    } else {
        fprintf(stderr, "[watchdog] /dev/watchdog not available (errno=%d)\n", errno);
    }

    publish_availability("online");
    fprintf(stderr, "[mqtt] published 'online'\n");

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    uint8_t rxbuf[MAX_PKT];
    int rxlen = 0;
    uint8_t tx[64];
    int tlen;

    fprintf(stderr, "[init] sending version request (CMD=0x03 SUB_VER_REQ=0x16)...\n");
    tlen = build_pkt(tx, SUB_VER_REQ, (uint8_t[]){0x01}, 1);
    log_hex("TX version request", tx, tlen);
    int wrote = write(uart_fd, tx, tlen);
    fprintf(stderr, "[uart] wrote %d bytes to UART\n", wrote);
    init_state = STATE_VER_REQ_SENT;
    time_t last_handshake_tx = time(NULL);
    fprintf(stderr, "[init] state -> VER_REQ_SENT\n");

    int sensor_count = 0, macs_rcvd = 0, monitoring = 0, init_sent_sum = 0;
    time_t last_ts = 0;
    uint64_t loop_count = 0;

    while (running) {
        loop_count++;
        if (wdt_fd >= 0) {
            int w = write(wdt_fd, "\0", 1);
            if (w < 0) fprintf(stderr, "[watchdog] kick failed (errno=%d)\n", errno);
        }

        fd_set rfds;
        struct timeval tv;
        FD_ZERO(&rfds);
        FD_SET(uart_fd, &rfds);
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        int ret = select(uart_fd + 1, &rfds, NULL, NULL, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            fprintf(stderr, "[select] error (errno=%d)\n", errno);
            break;
        }

        time_t now = time(NULL);
        if (init_state == STATE_VER_REQ_SENT && (now - last_handshake_tx > 2)) {
            fprintf(stderr, "[init] retrying version request (%.0fs since last)\n",
                    difftime(now, last_handshake_tx));
            tlen = build_pkt(tx, SUB_VER_REQ, (uint8_t[]){0x01}, 1);
            log_hex("TX version request (retry)", tx, tlen);
            write(uart_fd, tx, tlen);
            last_handshake_tx = now;
        }

        if (ret > 0) {
            int n = read(uart_fd, rxbuf + rxlen, MAX_PKT - rxlen);
            if (n > 0) {
                fprintf(stderr, "[uart] read %d bytes (buf_len was %d, now %d)\n",
                        n, rxlen, rxlen + n);
                log_hex("RX raw bytes", rxbuf + rxlen, n);
                rxlen += n;
                cc1310_pkt_t pkt;
                int parsed_count = 0;
                while (parse_packet(rxbuf, &rxlen, &pkt)) {
                    parsed_count++;
                    fprintf(stderr, "[rx] === PARSED PKT #%d: proto=0x%02X cmd=0x%02X payload_len=%d ===\n",
                            parsed_count, pkt.proto, pkt.cmd, pkt.payload_len);
                    if (pkt.payload_len > 0) {
                        log_hex("RX payload", pkt.payload, pkt.payload_len);
                    }

                    switch (pkt.cmd) {

                    case CMD_VER_RESP:
                        fprintf(stderr, "[init] CC1310 version response: %.*s\n",
                                pkt.payload_len, pkt.payload);
                        send_ack(pkt.cmd);
                        if (init_state < STATE_VER_RCVD) {
                            fprintf(stderr, "[init] version received, requesting battery info\n");
                            init_state = STATE_VER_RCVD;
                            tlen = build_pkt(tx, SUB_BAT_REQ, (uint8_t[]){0x01}, 1);
                            log_hex("TX battery request", tx, tlen);
                            write(uart_fd, tx, tlen);
                        }
                        break;

                    case CMD_BAT_RESP:
                    case 0x5E:
                        fprintf(stderr, "[init] battery/power response cmd=0x%02X\n", pkt.cmd);
                        send_ack(pkt.cmd);
                        if (init_state == STATE_VER_RCVD && !init_sent_sum) {
                            init_sent_sum = 1;
                            init_state = STATE_BAT_RCVD;
                            fprintf(stderr, "[init] battery OK, requesting sensor summary\n");
                            tlen = build_pkt(tx, SUB_SENSOR_SUM, (uint8_t[]){0x01}, 1);
                            log_hex("TX sensor summary request", tx, tlen);
                            write(uart_fd, tx, tlen);
                        }
                        break;

                    case CMD_SENSOR_SUM_ACK:
                        sensor_count = pkt.payload[0];
                        macs_rcvd = 0;
                        fprintf(stderr, "[init] sensor count ACK: %d sensors\n",
                                sensor_count);
                        send_ack(pkt.cmd);
                        if (sensor_count > 0 && init_state < STATE_SUM_RCVD) {
                            init_state = STATE_SUM_RCVD;
                            fprintf(stderr, "[init] requesting sensor MAC list (count=%d)\n",
                                    sensor_count);
                            tlen = build_pkt(tx, SUB_SENSOR_LIST, (uint8_t[]){(uint8_t)sensor_count, 0x01}, 2);
                            log_hex("TX sensor list request", tx, tlen);
                            write(uart_fd, tx, tlen);
                        } else if (sensor_count == 0) {
                            fprintf(stderr, "[init] no paired sensors, sending timestamp\n");
                            send_timestamp();
                            monitoring = 1;
                            init_state = STATE_MONITORING;
                            fprintf(stderr, "[init] state -> MONITORING (no sensors)\n");
                        }
                        break;

                    case CMD_SENSOR_MAC: {
                        char mac_str[17] = {0};
                        int clen = pkt.payload_len < 16 ? pkt.payload_len : 16;
                        memcpy(mac_str, pkt.payload, clen);
                        fprintf(stderr, "[sensor] #%d MAC: %s\n",
                                macs_rcvd + 1, mac_str);
                        send_ack(pkt.cmd);
                        if (macs_rcvd < MAX_SENSORS) {
                            strncpy(sensor_macs[macs_rcvd], mac_str, 16);
                        }
                        macs_rcvd++;
                        if (macs_rcvd >= sensor_count && init_state < STATE_MONITORING) {
                            fprintf(stderr, "[init] all %d sensor MACs received, sending timestamp\n",
                                    macs_rcvd);
                            send_timestamp();
                            sensor_mac_count = macs_rcvd;
                            for (int si = 0; si < sensor_mac_count; si++) {
                                char full[32];
                                snprintf(full, sizeof(full), "CC%s", sensor_macs[si]);
                                publish_sensor_event(full, "UNKNOWN", "CLOSED", 50, 0);
                            }
                            monitoring = 1;
                            init_state = STATE_MONITORING;
                            last_ts = time(NULL);
                            fprintf(stderr, "[init] state -> MONITORING (published %d initial states)\n",
                                    sensor_mac_count);
                        }
                        break;
                    }

                    case 0x32:
                        fprintf(stderr, "[init] CMD 0x32 received, sending ack + timestamp\n");
                        send_ack(0x32);
                        send_timestamp();
                        last_ts = time(NULL);
                        break;

                    case RX_SENSOR_REPORT:
                    case RX_SENSOR_ALARM: {
                        fprintf(stderr, "[sensor] event cmd=0x%02X payload_len=%d\n",
                                pkt.cmd, pkt.payload_len);
                        send_ack(pkt.cmd);
                        log_hex("sensor event payload", pkt.payload,
                                pkt.payload_len < 32 ? pkt.payload_len : 32);
                        if (pkt.payload_len < 17) break;

                        char mac[9] = {0};
                        memcpy(mac, pkt.payload + 9, 8);

                        uint8_t *alarm = pkt.payload + 17;

                        char mac_full[17];
                        snprintf(mac_full, sizeof(mac_full), "CC%s", mac);

                        uint8_t sclass  = alarm[0];
                        uint8_t battery = alarm[2];
                        uint8_t state   = alarm[5];
                        int8_t  rssi    = (int8_t)alarm[8];

                        const char *class_str = class_name(sclass);
                        const char *device_class = NULL;
                        if (sclass == 0x01)      device_class = "door";
                        else if (sclass == 0x02) device_class = "motion";
                        else if (sclass == 0x03) device_class = "moisture";

                        const char *state_str = (sclass == 0x02)
                            ? (state ? "ACTIVE" : "INACTIVE")
                            : (state ? "OPEN"   : "CLOSED");

                        fprintf(stderr, "\n>>> %s [%s]: %s | batt=%d%% | rssi=%d\n\n",
                                mac_full, class_str, state_str, battery, rssi);

                        ha_discovery_sensor(mac_full, device_class);
                        publish_sensor_event(mac_full, class_str,
                                             state_str, battery, rssi);
                        break;
                    }

                    default:
                        fprintf(stderr, "[rx] UNKNOWN cmd=0x%02X len=%d\n",
                                pkt.cmd, pkt.payload_len);
                        send_ack(pkt.cmd);
                        break;
                    }
                }
                if (parsed_count == 0 && n > 0)
                    fprintf(stderr, "[rx] read %d bytes but no valid packets parsed\n", n);
            } else if (n < 0 && errno != EAGAIN) {
                perror("read");
                break;
            } else if (n == 0) {
                fprintf(stderr, "[uart] read returned 0 (EOF?)\n");
                break;
            }
        }

        if (init_state == STATE_MONITORING && time(NULL) - last_ts > 60) {
            fprintf(stderr, "[monitor] sending periodic timestamp (%.0fs idle)\n",
                    difftime(time(NULL), last_ts));
            send_timestamp();
            last_ts = time(NULL);
        }

        time_t t_now = time(NULL);
        if (ret == 0 && init_state != STATE_UNINIT) {
            fprintf(stderr, "[dbg] loop[%lu] state=%s elapsed=%.0fs\n",
                    (unsigned long)loop_count, state_name(init_state),
                    difftime(t_now, last_handshake_tx));
        }
    }

    fprintf(stderr, "[gatewayd] shutting down...\n");
    publish_availability("offline");
    if (wdt_fd >= 0) close(wdt_fd);
    close(uart_fd);
    fprintf(stderr, "[gatewayd] stopped\n");
    return 0;
}