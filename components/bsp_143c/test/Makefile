CC ?= gcc
CFLAGS := -std=c11 -Wall -Wextra -Werror -O2 -I../include

SRCS := \
	../src/bsp_audio.c \
	../src/bsp_display.c \
	../src/bsp_power_key.c \
	test_bsp_143c.c

TARGET := test_bsp_143c.out

.PHONY: all test clean

all: $(TARGET)

$(TARGET): $(SRCS) ../include/bsp_143c.h ../include/bsp_143c_pins.h
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET)

test: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
