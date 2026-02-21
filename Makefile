CC ?= gcc
CFLAGS ?= -Wall -Wextra -std=c11 -O2 -D_POSIX_C_SOURCE=200809L
INCLUDES := -Iinclude
LDFLAGS := -Llib
LDLIBS := -lraylib -lm -ldl -lpthread -lGL -lrt -lX11

TARGET := build/gitviz
SRC := gitviz.c
REPO ?= $(CURDIR)

.PHONY: all run clean

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) $(INCLUDES) $< -o $@ $(LDFLAGS) $(LDLIBS)

run: $(TARGET)
	./$(TARGET) "$(REPO)"

clean:
	rm -f $(TARGET)
