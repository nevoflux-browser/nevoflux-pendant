#include "prov_nvs.h"

#include <string.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"

static bool   s_usb_ready;
static char   s_line[1024];
static size_t s_len;

bool prov_usb_line_init(void)
{
    if (s_usb_ready) {
        return true;
    }
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = 2048;
    cfg.tx_buffer_size = 2048;
    if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
        return false;
    }
    /* Route printf/ESP_LOG through the interrupt-driven driver so console output and RPC replies coexist. */
    usb_serial_jtag_vfs_use_driver();
    s_usb_ready = true;
    return true;
}

bool prov_usb_line_poll(char *out_line, size_t out_cap)
{
    if (!s_usb_ready || !out_line || out_cap == 0U) {
        return false;
    }
    uint8_t ch;
    while (usb_serial_jtag_read_bytes(&ch, 1, 0) == 1) {
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            if (s_len == 0U) {
                continue;
            }
            size_t n = (s_len < out_cap - 1U) ? s_len : out_cap - 1U;
            memcpy(out_line, s_line, n);
            out_line[n] = '\0';
            s_len = 0U;
            return true;
        }
        if (s_len < sizeof(s_line) - 1U) {
            s_line[s_len++] = (char)ch;
        } else {
            s_len = 0U; /* overlong line: drop it */
        }
    }
    return false;
}
