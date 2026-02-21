CC ?= gcc
CFLAGS ?= -Wall -Wextra -std=c11 -O2 -D_POSIX_C_SOURCE=200809L
INCLUDES := -Iinclude
LDFLAGS := -Llib
LDLIBS := -lraylib -lm -ldl -lpthread -lGL -lrt -lX11

TARGET := build/gitviz
TEST_TARGET := build/test_gitviz
SRC := gitviz.c
TEST_SRC := tests/test_gitviz.c
REPO ?= $(CURDIR)

.PHONY: all run test clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(INCLUDES) $< -o $@ $(LDFLAGS) $(LDLIBS)

$(TEST_TARGET): $(TEST_SRC)
	$(CC) $(CFLAGS) $(INCLUDES) $< -o $@ $(LDFLAGS) $(LDLIBS)

test: $(TEST_TARGET)
	./$(TEST_TARGET)

run: $(TARGET)
	./$(TARGET) "$(REPO)"

clean:
	rm -f $(TARGET) $(TEST_TARGET)
