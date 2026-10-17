.PHONY: all test test-c test-py storage clean

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

test-py:
	python3 -m unittest discover -s tools/provision -p "test_*.py" -v

storage:
	python3 tools/pack_storage/pack_storage.py --out build/storage.bin

clean:
	$(MAKE) -C components/nf_proto/test clean
	$(MAKE) -C components/prov_nvs/test clean
	$(MAKE) -C components/bsp_143c/test clean
	$(MAKE) -C apps/nevoflux/test clean
	rm -rf build/
