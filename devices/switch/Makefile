PROGRAM ?= switch
 
EXTRA_COMPONENTS = \
	extras/http-parser \
	extras/dhcpserver \
	extras/mbedtls \
	extras/httpd \
    $(abspath ../../external_libs/wolfssl) \
    $(abspath ../../external_libs/cJSON) \
    $(abspath ../../external_libs/homekit) \
    $(abspath ../../external_libs/wifi_config) \
 
FLASH_SIZE ?= 8
FLASH_MODE ?= dout
FLASH_SPEED ?= 40
HOMEKIT_SPI_FLASH_BASE_ADDR ?= 0x7A000
 
EXTRA_CFLAGS += -I../.. -DHOMEKIT_SHORT_APPLE_UUIDS -DLWIP_HTTPD_CGI=1 -DLWIP_HTTPD_SSI=1 -I./fsdata
 
include $(abspath ../../sdk/esp-open-rtos/common.mk)
 
monitor:
	$(FILTEROUTPUT) --port $(ESPPORT) --baud 115200 --elf $(PROGRAM_OUT)
 
html:
	@echo "Generating fsdata.."
	cd fsdata && ./makefsdata && cd -
