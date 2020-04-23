#pragma once

typedef enum {
    WIFI_CONFIG_CONNECTED = 1,
    WIFI_CONFIG_DISCONNECTED = 2,
} wifi_config_event_t;

void wifi_config_init(const char *ssid_prefix, const char *password, void (*on_wifi_ready)());
void wifi_config_init2(const char *ssid_prefix, const char *password, void (*on_event)(wifi_config_event_t));

void wifi_config_reset();
void wifi_config_get(char **ssid, char **password);
void wifi_config_set(const char *ssid, const char *password);

void wifi_config_set_custom_html(char *html);
