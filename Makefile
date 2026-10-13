CC      := gcc
CFLAGS  := -std=gnu11 -Wall -Wextra -pthread -Iinclude
LDFLAGS := -pthread
LDLIBS  :=

SRC_DIR   := src
TEST_DIR  := test
BUILD_DIR := build
BIN_DIR   := bin

TARGET := $(BIN_DIR)/server
CLIENT := $(BIN_DIR)/sensor_node

SRCS := $(wildcard $(SRC_DIR)/*.c)
OBJS := $(SRCS:$(SRC_DIR)/%.c=$(BUILD_DIR)/%.o)
DEPS := $(OBJS:.o=.d)

PORT      ?= 1234
HTTP_PORT ?= 8080

.PHONY: all client debug run clean

all: $(TARGET)

client: $(CLIENT)

$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

$(CLIENT): $(TEST_DIR)/sensor_node.c | $(BIN_DIR)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LDLIBS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c | $(BUILD_DIR)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(BUILD_DIR) $(BIN_DIR):
	mkdir -p $@

debug:
	$(MAKE) clean
	$(MAKE) all CFLAGS="$(CFLAGS) -DDEBUG -g"

run: $(TARGET)
	$(TARGET) $(PORT) $(HTTP_PORT)

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

-include $(DEPS)
