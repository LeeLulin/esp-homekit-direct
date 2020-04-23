# Component makefile for wifi_config

INC_DIRS += $(wifi_config_ROOT)/include

wifi_config_INC_DIR = $(wifi_config_ROOT)/include $(wifi_config_ROOT)/src $(wifi_config_OBJ_DIR)/content
wifi_config_SRC_DIR = $(wifi_config_ROOT)/src

$(eval $(call component_compile_rules,wifi_config))

ifdef WIFI_CONFIG_DEBUG
wifi_config_CFLAGS += -DWIFI_CONFIG_DEBUG
endif

ifdef WIFI_CONFIG_NO_RESTART
wifi_config_CFLAGS += -DWIFI_CONFIG_NO_RESTART
endif

ifdef WIFI_CONFIG_CONNECT_TIMEOUT
wifi_config_CFLAGS += -DWIFI_CONFIG_CONNECT_TIMEOUT=$(WIFI_CONFIG_CONNECT_TIMEOUT)
endif

ifdef WIFI_CONFIG_CONNECTED_MONITOR_INTERVAL
wifi_config_CFLAGS += -DWIFI_CONFIG_CONNECTED_MONITOR_INTERVAL=$(WIFI_CONFIG_CONNECTED_MONITOR_INTERVAL)
endif

ifdef WIFI_CONFIG_DISCONNECTED_MONITOR_INTERVAL
wifi_config_CFLAGS += -DWIFI_CONFIG_DISCONNECTED_MONITOR_INTERVAL=$(WIFI_CONFIG_DISCONNECTED_MONITOR_INTERVAL)
endif

ifndef WIFI_CONFIG_INDEX_HTML
WIFI_CONFIG_INDEX_HTML = $(wifi_config_ROOT)/content/index.html
endif

$(wifi_config_OBJ_DIR)/src/wifi_config.o: $(wifi_config_OBJ_DIR)/content/index.html.h

$(wifi_config_OBJ_DIR)/content/index.html.h: $(WIFI_CONFIG_INDEX_HTML)
	$(vecho "Embed %<")
	$(Q) mkdir -p $(@D)
	$(Q) $(wifi_config_ROOT)/tools/embed.py $< > $@
