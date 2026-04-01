HTSLIB_VERSION ?= 1.23
SAMTOOLS_VERSION ?= 1.23
EXTERNAL_DIR ?= external
HTSLIB_DIR ?= $(EXTERNAL_DIR)/htslib-$(HTSLIB_VERSION)
SAMTOOLS_DIR ?= $(EXTERNAL_DIR)/samtools-$(SAMTOOLS_VERSION)
BUILD_DIR ?= build

UNAME_S := $(shell uname -s)

CFLAGS ?= -O2 -g
CFLAGS += -std=c11 -Wall -Wextra -pedantic -I$(HTSLIB_DIR) -I$(HTSLIB_DIR)/htslib
LIBS := -lz

ifeq ($(UNAME_S),Darwin)
SO_EXT := bundle
SHARED_FLAGS := -bundle -Wl,-undefined,dynamic_lookup
else
SO_EXT := so
SHARED_FLAGS := -shared
endif

PLUGIN := $(BUILD_DIR)/hfile_cram2excel.$(SO_EXT)

.PHONY: all plugin toolchain validate clean

all: plugin

plugin: $(PLUGIN)

toolchain:
	./scripts/bootstrap_toolchain.sh

$(BUILD_DIR):
	mkdir -p $(BUILD_DIR)

$(PLUGIN): hfile_cram2excel.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) $(SHARED_FLAGS) -o $@ $< $(LIBS)

validate: $(PLUGIN)
	./validate.sh

clean:
	rm -rf $(BUILD_DIR)
