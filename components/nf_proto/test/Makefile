CC ?= gcc
CFLAGS := -std=c11 -Wall -Wextra -Werror -O2 -I../include
LDFLAGS := -lcrypto

SRCS := \
	../src/nfa1.c \
	../src/nfm1.c \
	../src/wav_header.c \
	../src/wire_seq.c \
	../src/nf_crypto.c \
	test_nf_proto.c

TARGET := test_nf_proto.out

.PHONY: all test clean

all: $(TARGET)

$(TARGET): $(SRCS) ../include/nf_proto.h
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)

test: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
