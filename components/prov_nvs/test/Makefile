CC ?= gcc
CFLAGS := -std=c11 -Wall -Wextra -Werror -O2 -I../include

SRCS := \
	../src/prov_nvs.c \
	test_prov_nvs.c

TARGET := test_prov_nvs.out

.PHONY: all test clean

all: $(TARGET)

$(TARGET): $(SRCS) ../include/prov_nvs.h
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET)

test: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
