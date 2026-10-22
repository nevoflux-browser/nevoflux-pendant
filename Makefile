.PHONY: all test test-c test-py storage sim sim-sdl sim-web emu emu-ota emu-provision win10-pack firmware clean

all: test

test: test-c test-py

test-c:
	$(MAKE) -C components/nf_proto/test test
	$(MAKE) -C components/nf_proto/test clean
	$(MAKE) -C components/prov_nvs/test test
	$(MAKE) -C components/prov_nvs/test clean
	$(MAKE) -C components/bsp_143c/test test
	$(MAKE) -C components/bsp_143c/test clean
	$(MAKE) -C apps/nevoflux/test test
	$(MAKE) -C apps/nevoflux/test clean
	$(MAKE) -C apps/muse/test test
	$(MAKE) -C apps/muse/test clean

test-py:
	python3 -m unittest discover -s tools/provision -p "test_*.py" -v

storage:
	python3 tools/pack_storage/pack_storage.py --out build/storage.bin

sim:
	$(MAKE) -C tools/sim all

sim-sdl:
	$(MAKE) -C tools/sim run-sdl

sim-web:
	$(MAKE) -C tools/sim web

emu:
	./tools/emu/run_esp_emu.sh

emu-ota:
	./tools/emu/run_esp_emu.sh --demo-ota

emu-provision:
	./tools/emu/run_esp_emu.sh --demo-provision

# Real ESP-IDF firmware for the Waveshare 1.43C board (ESP-IDF via Docker)
firmware:
	./tools/build_firmware.sh

win10-pack:
	python3 tools/emu/pack_win10_flash.py

clean:
	$(MAKE) -C components/nf_proto/test clean
	$(MAKE) -C components/prov_nvs/test clean
	$(MAKE) -C components/bsp_143c/test clean
	$(MAKE) -C apps/nevoflux/test clean
	$(MAKE) -C apps/muse/test clean
	$(MAKE) -C tools/sim clean
	rm -rf build/


