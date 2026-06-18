# Build the FreeTASE2 Server tools (server + client + probe).
#
# Static-links against a libIEC61850 build that MUST have been configured with
# CONFIG_MMS_SUPPORT_VMD_SCOPE_NAMED_VARIABLES=1 (TASE.2 reads TASE2_Version /
# Supported_Features at VMD scope). scripts/10_build.sh clones, patches and
# builds libIEC61850 into ../deps and passes LIB61850_HOME for you.

LIB61850_HOME ?= $(abspath $(dir $(lastword $(MAKEFILE_LIST)))/../deps/libiec61850)
BUILD         := $(LIB61850_HOME)/build

INCLUDES := \
	-I$(LIB61850_HOME)/src/common/inc \
	-I$(LIB61850_HOME)/src/mms/inc \
	-I$(LIB61850_HOME)/src/mms/inc_private \
	-I$(LIB61850_HOME)/src/iec61850/inc \
	-I$(LIB61850_HOME)/src/logging \
	-I$(LIB61850_HOME)/hal/inc \
	-I$(BUILD)/config \
	-I$(LIB61850_HOME)/config

LIBA := $(BUILD)/src/libiec61850.a $(BUILD)/hal/libhal.a

CFLAGS  ?= -O2 -g -Wall -Wno-unused-parameter
LDLIBS  := -lpthread -lm

# mbedtls is pulled in because the lib was built with TLS/R-GOOSE support.
MBEDTLS := $(shell ls /usr/lib/x86_64-linux-gnu/libmbedtls.so 2>/dev/null)
ifneq ($(MBEDTLS),)
LDLIBS += -lmbedtls -lmbedx509 -lmbedcrypto
endif

all: tase2_server tase2_client tase2_probe tase2_hmi_agent

tase2_server: tase2_server.c $(LIBA)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(LIBA) $(LDLIBS)

tase2_client: tase2_client.c $(LIBA)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(LIBA) $(LDLIBS)

tase2_probe: tase2_probe.c $(LIBA)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(LIBA) $(LDLIBS)

# Persistent ICCP client that backs the SCADA HMI bridge (hmi/bridge.py).
tase2_hmi_agent: tase2_hmi_agent.c $(LIBA)
	$(CC) $(CFLAGS) $(INCLUDES) -o $@ $< $(LIBA) $(LDLIBS)

clean:
	rm -f tase2_server tase2_client tase2_probe tase2_hmi_agent

.PHONY: clean
