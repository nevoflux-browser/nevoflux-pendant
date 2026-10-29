#pragma once
/*
 * The pendant's BLE link to its head, used only while Wi-Fi is off (the radio
 * and internal RAM do not stretch to both). GATT peripheral: the head (central)
 * finds us by the data channel tag in our service data, writes records to RX
 * and gets ours as notifications on TX; nf_ble_frame.h has the record format.
 * ESP only.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Service 6e660001-7c3f-4b8a-9e0d-5f1b2c3d4e5f, RX ...0002 (write), TX ...0003 (notify) */

/* A whole received record; called on nf_ble's own task, buf is freed after it returns */
typedef void (*nf_ble_on_record_t)(uint8_t type, uint8_t *buf, size_t len);

/* Bring up the controller and advertise; tag is the data channel's 8-hex-digit tag */
bool nf_ble_start(const char *tag, nf_ble_on_record_t on_record);
/* Disconnect and release the controller and its memory */
void nf_ble_stop(void);
/* A head is connected and listening to TX */
bool nf_ble_ready(void);
/* One record; false if the link is not ready or it did not all go out in timeout_ms */
bool nf_ble_send(uint8_t type, const uint8_t *payload, size_t len, uint32_t timeout_ms);
