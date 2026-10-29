CC ?= gcc
CFLAGS := -std=c11 -Wall -Wextra -Werror -O2 \
	-I../main \
	-I../../../components/nf_proto/include \
	-I../../../components/prov_nvs/include \
	-I../../../components/bsp_143c/include
LDFLAGS := -lcrypto

SRCS := \
	../../../components/nf_proto/src/nfa1.c \
	../../../components/nf_proto/src/nfm1.c \
	../../../components/nf_proto/src/wav_header.c \
	../../../components/nf_proto/src/wire_seq.c \
	../../../components/nf_proto/src/nf_crypto.c \
	../../../components/nf_proto/src/nf_codec.c \
	../../../components/prov_nvs/src/prov_nvs.c \
	../../../components/bsp_143c/src/bsp_audio.c \
	../../../components/bsp_143c/src/bsp_display.c \
	../../../components/bsp_143c/src/bsp_power_key.c \
	../main/token_mgr.c \
	../main/relay_ws.c \
	../main/audio_out.c \
	../main/app_state.c \
	../main/nf_addr.c \
	test_nevoflux_app.c

TARGET := test_nevoflux_app.out

.PHONY: all test clean

all: $(TARGET)

$(TARGET): $(SRCS) ../main/nevoflux_app.h ../main/nf_addr.h
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)

test: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
