CC ?= gcc
CFLAGS = -Wall -Wextra -Werror -std=c11 -O2 \
         -I../main \
         -I../../../components/prov_nvs/include \
         -I../../../components/bsp_143c/include

SRCS = test_muse_app.c \
       ../main/muse_oauth_device.c \
       ../../../components/prov_nvs/src/prov_nvs.c

TARGET = test_muse_app.out

.PHONY: all test clean

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET)

test: $(TARGET)
	./$(TARGET)

clean:
	rm -f $(TARGET)
