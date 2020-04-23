#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sysparam.h>
#include <etstimer.h>
#include <esplibs/libmain.h>
#include <espressif/esp_common.h>
#include <lwip/sockets.h>

#include <semphr.h>

#include <http-parser/http_parser.h>
#include <dhcpserver.h>

#include "wifi_config.h"
#include "form_urlencoded.h"

#define WIFI_CONFIG_SERVER_PORT 80

#ifndef WIFI_CONFIG_CONNECT_TIMEOUT
#define WIFI_CONFIG_CONNECT_TIMEOUT 15000
#endif
#ifndef WIFI_CONFIG_CONNECTED_MONITOR_INTERVAL
#define WIFI_CONFIG_CONNECTED_MONITOR_INTERVAL 30000
#endif
#ifndef WIFI_CONFIG_DISCONNECTED_MONITOR_INTERVAL
#define WIFI_CONFIG_DISCONNECTED_MONITOR_INTERVAL 10000
#endif

#define INFO(message, ...) printf(">>> wifi_config: " message "\n", ##__VA_ARGS__);
#define ERROR(message, ...) printf("!!! wifi_config: " message "\n", ##__VA_ARGS__);

#ifdef WIFI_CONFIG_DEBUG
#define DEBUG(message, ...) printf("*** wifi_config: " message "\n", ##__VA_ARGS__);
#else
#define DEBUG(message, ...)
#endif


typedef enum {
    ENDPOINT_UNKNOWN = 0,
    ENDPOINT_INDEX,
    ENDPOINT_SETTINGS,
    ENDPOINT_SETTINGS_UPDATE,
} endpoint_t;


typedef struct {
    char *ssid_prefix;
    char *password;
    char *custom_html;
    void (*on_wifi_ready)();  // deprecated
    void (*on_event)(wifi_config_event_t);

    int first_time;
    ETSTimer network_monitor_timer;
    TaskHandle_t http_task_handle;
    TaskHandle_t dns_task_handle;
} wifi_config_context_t;


static wifi_config_context_t *context = NULL;

typedef struct _client {
    int fd;

    http_parser parser;
    endpoint_t endpoint;
    uint8_t *body;
    size_t body_length;
} client_t;


static int wifi_config_has_configuration();
static int wifi_config_station_connect();
static void wifi_config_softap_start();
static void wifi_config_softap_stop();

static client_t *client_new() {
    client_t *client = malloc(sizeof(client_t));
    memset(client, 0, sizeof(client_t));

    http_parser_init(&client->parser, HTTP_REQUEST);
    client->parser.data = client;

    return client;
}


static void client_free(client_t *client) {
    if (client->body)
        free(client->body);

    free(client);
}


static void client_send(client_t *client, const char *payload, size_t payload_size) {
    lwip_write(client->fd, payload, payload_size);
}


static void client_send_chunk(client_t *client, const char *payload) {
    int len = strlen(payload);
    char buffer[10];
    int buffer_len = snprintf(buffer, sizeof(buffer), "%x\r\n", len);
    client_send(client, buffer, buffer_len);
    client_send(client, payload, len);
    client_send(client, "\r\n", 2);
}


static void client_send_redirect(client_t *client, int code, const char *redirect_url) {
    DEBUG("Redirecting to %s", redirect_url);
    char buffer[128];
    size_t len = snprintf(buffer, sizeof(buffer), "HTTP/1.1 %d \r\nLocation: %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", code, redirect_url);
    client_send(client, buffer, len);
}


typedef struct _wifi_network_info {
    char ssid[33];
    bool secure;

    struct _wifi_network_info *next;
} wifi_network_info_t;


wifi_network_info_t *wifi_networks = NULL;
SemaphoreHandle_t wifi_networks_mutex;


static void wifi_scan_done_cb(void *arg, sdk_scan_status_t status)
{
    if (status != SCAN_OK)
    {
        ERROR("WiFi scan failed");
        return;
    }

    xSemaphoreTake(wifi_networks_mutex, portMAX_DELAY);

    wifi_network_info_t *wifi_network = wifi_networks;
    while (wifi_network) {
        wifi_network_info_t *next = wifi_network->next;
        free(wifi_network);
        wifi_network = next;
    }
    wifi_networks = NULL;

    struct sdk_bss_info *bss = (struct sdk_bss_info *)arg;
    // first one is invalid
    bss = bss->next.stqe_next;

    while (bss) {
        wifi_network_info_t *net = wifi_networks;
        while (net) {
            if (!strncmp(net->ssid, (char *)bss->ssid, sizeof(net->ssid)))
                break;
            net = net->next;
        }
        if (!net) {
            wifi_network_info_t *net = malloc(sizeof(wifi_network_info_t));
            memset(net, 0, sizeof(*net));
            strncpy(net->ssid, (char *)bss->ssid, sizeof(net->ssid));
            net->secure = bss->authmode != AUTH_OPEN;
            net->next = wifi_networks;

            wifi_networks = net;
        }

        bss = bss->next.stqe_next;
    }

    xSemaphoreGive(wifi_networks_mutex);
}

static void wifi_scan_task(void *arg)
{
    INFO("Starting WiFi scan");
    while (true)
    {
        if (sdk_wifi_get_opmode() != STATIONAP_MODE)
            break;

        sdk_wifi_station_scan(NULL, wifi_scan_done_cb);
        vTaskDelay(10000 / portTICK_PERIOD_MS);
    }

    xSemaphoreTake(wifi_networks_mutex, portMAX_DELAY);

    wifi_network_info_t *wifi_network = wifi_networks;
    while (wifi_network) {
        wifi_network_info_t *next = wifi_network->next;
        free(wifi_network);
        wifi_network = next;
    }
    wifi_networks = NULL;

    xSemaphoreGive(wifi_networks_mutex);

    vTaskDelete(NULL);
}

#include "index.html.h"

static void wifi_config_server_on_settings(client_t *client) {
    static const char http_prologue[] =
        "HTTP/1.1 200 \r\n"
        "Content-Type: text/html; charset=utf-8\r\n"
        "Cache-Control: no-store\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n"
        "\r\n";

    client_send(client, http_prologue, sizeof(http_prologue)-1);
    client_send_chunk(client, html_settings_header);

    if (context->custom_html != NULL && context->custom_html[0] > 0) {
	uint8_t buffer_size = strlen(html_settings_custom_html) + strlen(context->custom_html); 
	char* buffer = (char*) calloc(buffer_size, sizeof(char)); //fill up the buffer with zeros
	snprintf(buffer, buffer_size, html_settings_custom_html, context->custom_html); //fill in template with the custom_html content
	client_send_chunk(client, buffer); 
	free(buffer);
    }

    client_send_chunk(client, html_settings_body);

    if (xSemaphoreTake(wifi_networks_mutex, 5000 / portTICK_PERIOD_MS)) {
	char buffer[64];
        wifi_network_info_t *net = wifi_networks;
        while (net) {
            snprintf(
                buffer, sizeof(buffer),
                html_network_item,
                net->secure ? "secure" : "unsecure", net->ssid
            );
            client_send_chunk(client, buffer);

            net = net->next;
        }

        xSemaphoreGive(wifi_networks_mutex);
    }

    client_send_chunk(client, html_settings_footer);
    client_send_chunk(client, "");
}


static void wifi_config_server_on_settings_update(client_t *client) {
    DEBUG("Update settings, body = %s", client->body);

    form_param_t *form = form_params_parse((char *)client->body);
    if (!form) {
        DEBUG("Couldn't parse form data, redirecting to /settings");
        client_send_redirect(client, 302, "/settings");
        return;
    }

    form_param_t *ssid_param = form_params_find(form, "ssid");
    form_param_t *password_param = form_params_find(form, "password");
    if (!ssid_param) {
        DEBUG("Invalid form data, redirecting to /settings");
        form_params_free(form);
        client_send_redirect(client, 302, "/settings");
        return;
    }

    static const char payload[] = "HTTP/1.1 204 \r\nContent-Type: text/html\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
    client_send(client, payload, sizeof(payload)-1);

    DEBUG("Setting wifi_ssid param = %s", ssid_param->value);
    DEBUG("Setting wifi_password param = %s", password_param->value);

    sysparam_set_string("wifi_ssid", ssid_param->value);
    if (password_param) {
        sysparam_set_string("wifi_password", password_param->value);
    } else {
        sysparam_set_string("wifi_password", "");
    }
    form_params_free(form);

    vTaskDelay(500 / portTICK_PERIOD_MS);

    wifi_config_station_connect();
}


static int wifi_config_server_on_url(http_parser *parser, const char *data, size_t length) {
    client_t *client = (client_t*) parser->data;

    client->endpoint = ENDPOINT_UNKNOWN;
    if (parser->method == HTTP_GET) {
        if (!strncmp(data, "/settings", length)) {
            client->endpoint = ENDPOINT_SETTINGS;
        } else if (!strncmp(data, "/", length)) {
            client->endpoint = ENDPOINT_INDEX;
        }
    } else if (parser->method == HTTP_POST) {
        if (!strncmp(data, "/settings", length)) {
            client->endpoint = ENDPOINT_SETTINGS_UPDATE;
        }
    }

    if (client->endpoint == ENDPOINT_UNKNOWN) {
        char *url = strndup(data, length);
        DEBUG("Got HTTP request: %s %s", http_method_str(parser->method), url);
        free(url);
    }

    return 0;
}


static int wifi_config_server_on_body(http_parser *parser, const char *data, size_t length) {
    client_t *client = parser->data;
    client->body = realloc(client->body, client->body_length + length + 1);
    memcpy(client->body + client->body_length, data, length);
    client->body_length += length;
    client->body[client->body_length] = 0;

    return 0;
}


static int wifi_config_server_on_message_complete(http_parser *parser) {
    client_t *client = parser->data;

    switch(client->endpoint) {
        case ENDPOINT_INDEX: {
            DEBUG("GET / -> redirecting to /settings");
            client_send_redirect(client, 301, "/settings");
            break;
        }
        case ENDPOINT_SETTINGS: {
            DEBUG("GET /settings");
            wifi_config_server_on_settings(client);
            break;
        }
        case ENDPOINT_SETTINGS_UPDATE: {
            DEBUG("POST /settings");
            wifi_config_server_on_settings_update(client);
            break;
        }
        case ENDPOINT_UNKNOWN: {
            DEBUG("Unknown endpoint -> redirecting to http://192.168.4.1/settings");
            client_send_redirect(client, 302, "http://192.168.4.1/settings");
            break;
        }
    }

    if (client->body) {
        free(client->body);
        client->body = NULL;
        client->body_length = 0;
    }

    return 0;
}


static http_parser_settings wifi_config_http_parser_settings = {
    .on_url = wifi_config_server_on_url,
    .on_body = wifi_config_server_on_body,
    .on_message_complete = wifi_config_server_on_message_complete,
};


static void http_task(void *arg) {
    INFO("Starting HTTP server");

    struct sockaddr_in serv_addr;
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    memset(&serv_addr, '0', sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(WIFI_CONFIG_SERVER_PORT);
    int flags;
    if ((flags = lwip_fcntl(listenfd, F_GETFL, 0)) < 0) {
        ERROR("Failed to get HTTP socket flags");
        lwip_close(listenfd);
        vTaskDelete(NULL);
        return;
    };
    if (lwip_fcntl(listenfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        ERROR("Failed to set HTTP socket flags");
        lwip_close(listenfd);
        vTaskDelete(NULL);
        return;
    }
    bind(listenfd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));
    listen(listenfd, 2);

    char data[64];

    bool running = true;
    while (running) {
        uint32_t task_value = 0;
        if (xTaskNotifyWait(0, 1, &task_value, 0) == pdTRUE) {
            if (task_value) {
                running = false;
                break;
            }
        }

        int fd = accept(listenfd, (struct sockaddr *)NULL, (socklen_t *)NULL);
        if (fd < 0) {
            vTaskDelay(500 / portTICK_PERIOD_MS);
            continue;
        }

        const struct timeval timeout = { 2, 0 }; /* 2 second timeout */
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

        client_t *client = client_new();
        client->fd = fd;

        for (;;) {
            int data_len = lwip_read(client->fd, data, sizeof(data));
            if (data_len == 0) {
                break;
            }

            if (data_len > 0) {
                DEBUG("Got %d incomming data", data_len);

                http_parser_execute(
                    &client->parser, &wifi_config_http_parser_settings,
                    data, data_len
                );
            }

            if (xTaskNotifyWait(0, 1, &task_value, 0) == pdTRUE) {
                if (task_value) {
                    running = false;
                    break;
                }
            }
        }

        DEBUG("Client disconnected");

        lwip_close(client->fd);
        client_free(client);
    }

    INFO("Stopping HTTP server");

    lwip_close(listenfd);
    vTaskDelete(NULL);
}


static void http_start() {
    xTaskCreate(http_task, "wifi_config HTTP", 512, NULL, 2, &context->http_task_handle);
}


static void http_stop() {
    if (! context->http_task_handle)
        return;

    xTaskNotify(context->http_task_handle, 1, eSetValueWithOverwrite);
}


static void dns_task(void *arg)
{
    INFO("Starting DNS server");

    ip4_addr_t server_addr;
    IP4_ADDR(&server_addr, 192, 168, 4, 1);

    struct sockaddr_in serv_addr;
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

    memset(&serv_addr, '0', sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(53);
    bind(fd, (struct sockaddr*)&serv_addr, sizeof(serv_addr));

    const struct timeval timeout = { 2, 0 }; /* 2 second timeout */
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    const struct ifreq ifreq1 = { "en1" };
    setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, &ifreq1, sizeof(ifreq1));

    for (;;) {
        char buffer[96];
        struct sockaddr src_addr;
        socklen_t src_addr_len = sizeof(src_addr);
        size_t count = recvfrom(fd, buffer, sizeof(buffer), 0, (struct sockaddr*)&src_addr, &src_addr_len);

        /* Drop messages that are too large to send a response in the buffer */
        if (count > 0 && count <= sizeof(buffer) - 16 && src_addr.sa_family == AF_INET) {
            size_t qname_len = strlen(buffer + 12) + 1;
            uint32_t reply_len = 2 + 10 + qname_len + 16 + 4;

            char *head = buffer + 2;
            *head++ = 0x80; // Flags
            *head++ = 0x00;
            *head++ = 0x00; // Q count
            *head++ = 0x01;
            *head++ = 0x00; // A count
            *head++ = 0x01;
            *head++ = 0x00; // Auth count
            *head++ = 0x00;
            *head++ = 0x00; // Add count
            *head++ = 0x00;
            head += qname_len;
            *head++ = 0x00; // Q type
            *head++ = 0x01;
            *head++ = 0x00; // Q class
            *head++ = 0x01;
            *head++ = 0xC0; // LBL offs
            *head++ = 0x0C;
            *head++ = 0x00; // Type
            *head++ = 0x01;
            *head++ = 0x00; // Class
            *head++ = 0x01;
            *head++ = 0x00; // TTL
            *head++ = 0x00;
            *head++ = 0x00;
            *head++ = 0x78;
            *head++ = 0x00; // RD len
            *head++ = 0x04;
            *head++ = ip4_addr1(&server_addr);
            *head++ = ip4_addr2(&server_addr);
            *head++ = ip4_addr3(&server_addr);
            *head++ = ip4_addr4(&server_addr);

            DEBUG("Got DNS query, sending response");
            sendto(fd, buffer, reply_len, 0, &src_addr, src_addr_len);
        }

        uint32_t task_value = 0;
        if (xTaskNotifyWait(0, 1, &task_value, 0) == pdTRUE) {
            if (task_value)
                break;
        }
    }

    INFO("Stopping DNS server");

    lwip_close(fd);

    vTaskDelete(NULL);
}


static void dns_start() {
    xTaskCreate(dns_task, "wifi_config DNS", 384, NULL, 2, &context->dns_task_handle);
}


static void dns_stop() {
    if (!context->dns_task_handle)
        return;

    xTaskNotify(context->dns_task_handle, 1, eSetValueWithOverwrite);
}


static void wifi_config_softap_start() {
    INFO("Starting AP mode");

    sdk_wifi_set_opmode(STATIONAP_MODE);

    uint8_t macaddr[6];
    sdk_wifi_get_macaddr(SOFTAP_IF, macaddr);

    struct sdk_softap_config softap_config;
    sdk_wifi_softap_get_config(&softap_config);
    softap_config.ssid_len = snprintf(
        (char *)softap_config.ssid, sizeof(softap_config.ssid),
        "%s-%02X%02X%02X", context->ssid_prefix, macaddr[3], macaddr[4], macaddr[5]
    );
    softap_config.ssid_hidden = 0;
    if (context->password) {
        softap_config.authmode = AUTH_WPA_WPA2_PSK;
        strncpy((char *)softap_config.password,
                context->password, sizeof(softap_config.password));
    } else {
        softap_config.authmode = AUTH_OPEN;
    }
    softap_config.max_connection = 2;
    softap_config.beacon_interval = 100;

    DEBUG("Starting AP SSID=%s", softap_config.ssid);

    sdk_wifi_softap_set_config(&softap_config);

    wifi_networks_mutex = xSemaphoreCreateBinary();
    xSemaphoreGive(wifi_networks_mutex);

    xTaskCreate(wifi_scan_task, "wifi_config scan", 384, NULL, 2, NULL);

    INFO("Starting DHCP server");
    struct ip_info ap_ip;
    IP4_ADDR(&ap_ip.ip, 192, 168, 4, 1);
    IP4_ADDR(&ap_ip.netmask, 255, 255, 255, 0);
    IP4_ADDR(&ap_ip.gw, 0, 0, 0, 0);
    sdk_wifi_set_ip_info(SOFTAP_IF, &ap_ip);

    ip4_addr_t first_client_ip;
    first_client_ip.addr = ap_ip.ip.addr + htonl(1);

    dhcpserver_start(&first_client_ip, 4);
    dhcpserver_set_router(&ap_ip.ip);
    dhcpserver_set_dns(&ap_ip.ip);

    dns_start();
    http_start();
}


static void wifi_config_softap_stop() {
    dhcpserver_stop();
    dns_stop();
    http_stop();
    sdk_wifi_set_opmode(STATION_MODE);
}


static void wifi_config_monitor_callback(void *arg) {
    if (sdk_wifi_station_get_connect_status() == STATION_GOT_IP) {
        if (sdk_wifi_get_opmode() == STATION_MODE && !context->first_time)
            return;

        // Connected to station, all is dandy
        INFO("Connected to WiFi network");

        wifi_config_softap_stop();
        sdk_wifi_station_set_auto_connect(false);

        if (context->on_event)
            context->on_event(WIFI_CONFIG_CONNECTED);

        context->first_time = false;

        // change monitoring poll interval
        sdk_os_timer_arm(&context->network_monitor_timer,
                         WIFI_CONFIG_CONNECTED_MONITOR_INTERVAL, 1);

        return;
    } else {
        if (wifi_config_has_configuration())
            wifi_config_station_connect();

        if (sdk_wifi_get_opmode() != STATION_MODE)
            return;

        INFO("Disconnected from WiFi network");

        if (!context->first_time && context->on_event)
            context->on_event(WIFI_CONFIG_DISCONNECTED);

        // change monitoring poll interval
        sdk_os_timer_arm(&context->network_monitor_timer,
                         WIFI_CONFIG_DISCONNECTED_MONITOR_INTERVAL, 1);

        wifi_config_softap_start();
    }
}


static int wifi_config_has_configuration() {
    char *wifi_ssid = NULL;
    sysparam_get_string("wifi_ssid", &wifi_ssid);

    if (!wifi_ssid) {
        return 0;
    }

    free(wifi_ssid);

    return 1;
}


static int wifi_config_station_connect() {
    char *wifi_ssid = NULL;
    char *wifi_password = NULL;
    sysparam_get_string("wifi_ssid", &wifi_ssid);
    sysparam_get_string("wifi_password", &wifi_password);

    if (!wifi_ssid) {
        ERROR("No configuration found");
        if (wifi_password)
            free(wifi_password);
        return -1;
    }

    INFO("Connecting to %s", wifi_ssid);

    struct sdk_station_config sta_config;
    memset(&sta_config, 0, sizeof(sta_config));
    strncpy((char *)sta_config.ssid, wifi_ssid, sizeof(sta_config.ssid));
    sta_config.ssid[sizeof(sta_config.ssid)-1] = 0;
    if (wifi_password)
        strncpy((char *)sta_config.password, wifi_password, sizeof(sta_config.password));

    sdk_wifi_station_set_config(&sta_config);

    sdk_wifi_station_connect();
    sdk_wifi_station_set_auto_connect(true);

    free(wifi_ssid);
    if (wifi_password)
        free(wifi_password);

    return 0;
}


void wifi_config_start() {
    sdk_wifi_set_opmode(STATION_MODE);

    context->first_time = true;

    if (wifi_config_station_connect()) {
        wifi_config_softap_start();
    }

    sdk_os_timer_setfn(&context->network_monitor_timer, wifi_config_monitor_callback, NULL);
    sdk_os_timer_arm(&context->network_monitor_timer,
                     WIFI_CONFIG_DISCONNECTED_MONITOR_INTERVAL, 1);
}


void wifi_config_legacy_support_on_event(wifi_config_event_t event) {
    if (event == WIFI_CONFIG_CONNECTED) {
        if (context->on_wifi_ready) {
            context->on_wifi_ready();
        }
    }
#ifndef WIFI_CONFIG_NO_RESTART
    else if (event == WIFI_CONFIG_DISCONNECTED) {
        sdk_system_restart();
    }
#endif
}


void wifi_config_init(const char *ssid_prefix, const char *password, void (*on_wifi_ready)()) {
    INFO("Initializing WiFi config");
    if (password && strlen(password) < 8) {
        ERROR("Password should be at least 8 characters");
        return;
    }

    context = malloc(sizeof(wifi_config_context_t));
    memset(context, 0, sizeof(*context));

    context->ssid_prefix = strndup(ssid_prefix, 33-7);
    if (password)
        context->password = strdup(password);

    context->on_wifi_ready = on_wifi_ready;
    context->on_event = wifi_config_legacy_support_on_event;

    wifi_config_start();
}


void wifi_config_init2(const char *ssid_prefix, const char *password,
                       void (*on_event)(wifi_config_event_t))
{
    INFO("Initializing WiFi config");
    if (password && strlen(password) < 8) {
        ERROR("Password should be at least 8 characters");
        return;
    }

    context = malloc(sizeof(wifi_config_context_t));
    memset(context, 0, sizeof(*context));

    context->ssid_prefix = strndup(ssid_prefix, 33-7);
    if (password)
        context->password = strdup(password);

    context->on_event = on_event;

    wifi_config_start();
}


void wifi_config_reset() {
    sysparam_set_string("wifi_ssid", "");
    sysparam_set_string("wifi_password", "");
}


void wifi_config_get(char **ssid, char **password) {
    if (ssid)
        sysparam_get_string("wifi_ssid", ssid);

    if (password)
        sysparam_get_string("wifi_password", password);
}


void wifi_config_set(const char *ssid, const char *password) {
    sysparam_set_string("wifi_ssid", ssid);
    sysparam_set_string("wifi_password", password);
}

void wifi_config_set_custom_html(char *html) {
    if (context == NULL) { 
        ERROR("Cannot set custom html content, WiFi configuration not initialised yet");
        return;
    }

    context->custom_html = html;
}
