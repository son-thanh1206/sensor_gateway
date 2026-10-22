CC      := gcc
CFLAGS  := -std=gnu11 -Wall -Wextra -pthread -Iinclude
LDFLAGS := -pthread
LDLIBS  :=

SRC_DIR     := src
ADAPTER_DIR := $(SRC_DIR)/adapter
TEST_DIR    := test
TEST_OUT    := $(TEST_DIR)/out
BUILD_DIR   := build
BIN_DIR     := bin

TARGET    := $(BIN_DIR)/server
CLIENT    := $(BIN_DIR)/sensor_node
TEST_SBUF := $(BIN_DIR)/test_sbuffer
TEST_SENS := $(BIN_DIR)/test_sensors
TEST_ADPT := $(BIN_DIR)/test_adapters
TEST_DMGR := $(BIN_DIR)/test_datamgr

# sensor_node.c has its own main(): built separately, not part of the server.
CLIENT_SRC := $(SRC_DIR)/sensor_node.c
SRCS := $(filter-out $(CLIENT_SRC), $(wildcard $(SRC_DIR)/*.c)) \
        $(wildcard $(ADAPTER_DIR)/*.c)
OBJS := $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
ADAPTER_OBJS := $(filter $(BUILD_DIR)/adapter/%, $(OBJS))
DEPS := $(OBJS:.o=.d)

PORT      ?= 1234
HTTP_PORT ?= 8080

.PHONY: all client test test-sbuffer test-adapters test-datamgr test-sensors debug run clean clean-test

all: $(TARGET) $(CLIENT)

client: $(CLIENT)

$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# sensor_node uses the frame constants, byte order and CRC helpers of these headers.
$(CLIENT): $(CLIENT_SRC) include/adapters.h include/byteorder.h include/crc16.h | $(BIN_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

$(TEST_SBUF): $(TEST_DIR)/test_sbuffer.c $(BUILD_DIR)/sbuffer.o | $(BIN_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

# Unit tests only: test_sensors starts sensor nodes against a running server.
test: test-sbuffer test-adapters test-datamgr

test-sbuffer: $(TEST_SBUF)
	$(TEST_SBUF)

$(TEST_SENS): $(TEST_DIR)/test_sensors.c | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $<

test-sensors: $(CLIENT) $(TEST_SENS)
	$(TEST_SENS)

$(TEST_ADPT): $(TEST_DIR)/test_adapters.c $(ADAPTER_OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ -lm

test-adapters: $(TEST_ADPT)
	$(TEST_ADPT)

# test_datamgr.c stubs printlog(), so logger.o is not linked.
$(TEST_DMGR): $(TEST_DIR)/test_datamgr.c $(BUILD_DIR)/datamgr.o $(BUILD_DIR)/sbuffer.o | $(BIN_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $^ $(LDLIBS)

test-datamgr: $(TEST_DMGR)
	@mkdir -p $(TEST_OUT)
	$(TEST_DMGR)

# mkdir per object: sources in subdirectories (src/adapter/) map to
# matching subdirectories of build/.
$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BIN_DIR):
	mkdir -p $@

debug:
	$(MAKE) clean
	$(MAKE) all CFLAGS="$(CFLAGS) -DDEBUG -g"

run: $(TARGET)
	$(TARGET) $(PORT) $(HTTP_PORT)

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

# Separate from clean: test/step3_run.sh runs "make debug" (which calls
# clean) and must keep the output directory it is writing to.
clean-test:
	rm -rf $(TEST_OUT)

-include $(DEPS)
