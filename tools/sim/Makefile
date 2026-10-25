CC ?= gcc
PKG_CFLAGS := $(shell pkg-config --cflags pangocairo sdl2)
PKG_LIBS   := $(shell pkg-config --libs pangocairo sdl2) -lcrypto -lm

CFLAGS := -std=c11 -Wall -Wextra -O2 -fPIC -DUSE_SDL2 \
	-I../../components/nf_proto/include \
	-I../../components/prov_nvs/include \
	-I../../components/bsp_143c/include \
	-I../../apps/nevoflux/main \
	-I../../apps/muse/main \
	$(PKG_CFLAGS)

FW_SRCS := \
	../../components/nf_proto/src/nfa1.c \
	../../components/nf_proto/src/nfm1.c \
	../../components/nf_proto/src/wav_header.c \
	../../components/nf_proto/src/wire_seq.c \
	../../components/nf_proto/src/nf_crypto.c \
	../../components/nf_proto/src/nf_codec.c \
	../../components/prov_nvs/src/prov_nvs.c \
	../../components/bsp_143c/src/bsp_audio.c \
	../../components/bsp_143c/src/bsp_display.c \
	../../components/bsp_143c/src/bsp_power_key.c \
	../../apps/nevoflux/main/token_mgr.c \
	../../apps/nevoflux/main/relay_ws.c \
	../../apps/nevoflux/main/audio_out.c \
	../../apps/nevoflux/main/app_state.c \
	../../apps/muse/main/muse_oauth_device.c

LIB_TARGET := libnevoflux_sim.so
BIN_TARGET := nevoflux_sim

.PHONY: all clean snapshots run-sdl web

all: $(LIB_TARGET) $(BIN_TARGET)

$(LIB_TARGET): nevoflux_sim.c $(FW_SRCS)
	$(CC) $(CFLAGS) -shared nevoflux_sim.c $(FW_SRCS) -o $(LIB_TARGET) $(PKG_LIBS)

$(BIN_TARGET): nevoflux_sim.c $(FW_SRCS)
	$(CC) $(CFLAGS) -DSIM_STANDALONE_MAIN nevoflux_sim.c $(FW_SRCS) -o $(BIN_TARGET) $(PKG_LIBS)

run-sdl: $(BIN_TARGET)
	./$(BIN_TARGET)

web: $(LIB_TARGET)
	python3 sim_server.py --port 8765

snapshots: $(BIN_TARGET)
	mkdir -p snapshots
	./$(BIN_TARGET) --dump-snapshots snapshots

clean:
	rm -f $(LIB_TARGET) $(BIN_TARGET)
	rm -rf snapshots

