#!/usr/bin/env python3
import json
import unittest

from provision import (
    _BleLineClient,
    DerivedPairingMaterial,
    derive_channel_id,
    derive_pairing_material,
    execute_provisioning_handshake,
)


class TestProvisioning(unittest.TestCase):
    def test_channel_id_and_argon2id_derivation(self) -> None:
        code = "nevo-test-pair-2026"
        # Use smaller memory in unit test for fast execution, then verify full 64MB KDF once
        mat_fast = derive_pairing_material(code, memory_kib=1024, iterations=2, parallelism=4)
        self.assertEqual(len(mat_fast.control_channel_id), 32)
        self.assertEqual(len(mat_fast.data_channel_id), 32)
        self.assertNotEqual(mat_fast.control_channel_id, mat_fast.data_channel_id)
        self.assertEqual(len(mat_fast.control_key), 32)
        self.assertEqual(len(mat_fast.data_key), 32)
        self.assertNotEqual(mat_fast.control_key, mat_fast.data_key)

        # Verify production parameters (m=65536, t=3, p=4) determinism
        mat_prod1 = derive_pairing_material(code)
        mat_prod2 = derive_pairing_material(code)
        self.assertEqual(mat_prod1, mat_prod2)
        self.assertEqual(len(mat_prod1.data_key_hex), 64)

    def test_handshake_with_boot_confirmation(self) -> None:
        mat = derive_pairing_material("code-123", memory_kib=1024, iterations=1, parallelism=4)
        state = {"polls": 0, "written": None}

        def mock_transport(line: str) -> str:
            req = json.loads(line)
            cmd = req["cmd"]
            if cmd == "prov.offer":
                return json.dumps({"status": "wait_boot", "timeout_s": 15})
            if cmd == "prov.status":
                state["polls"] += 1
                # Simulate user clicking BOOT on 2nd poll
                return json.dumps({"status": "ready" if state["polls"] >= 2 else "wait_boot"})
            if cmd == "prov.write":
                state["written"] = req
                return json.dumps({"status": "ok"})
            return json.dumps({"status": "error"})

        execute_provisioning_handshake(
            send_line=mock_transport,
            material=mat,
            account_token="tok_independent_7d",
            wifi_ssid="OfficeWiFi",
            wifi_pass="pass1234",
            poll_interval_sec=0.0,
        )
        self.assertEqual(state["polls"], 2)
        self.assertIsNotNone(state["written"])
        self.assertEqual(state["written"]["wifi_ssid"], "OfficeWiFi")
        self.assertEqual(state["written"]["account_token"], "tok_independent_7d")
        self.assertEqual(state["written"]["data_channel_id"], mat.data_channel_id)
        self.assertEqual(state["written"]["data_key_hex"], mat.data_key_hex)


    def test_ble_notify_reassembles_chunked_lines(self) -> None:
        import queue

        client = object.__new__(_BleLineClient)  # skip __init__ (no BLE adapter / bleak needed)
        client._buf = b""
        client._lines = queue.Queue()
        reply = '{"status":"ok","ssid":"家里的WiFi"}\n'.encode("utf-8")
        # MTU-sized notifications may split a UTF-8 character and carry two replies at once
        for chunk in (reply[:7], reply[7:20], reply[20:] + b'{"status":"idle"}\n'):
            client._on_notify(None, bytearray(chunk))
        self.assertEqual(json.loads(client._lines.get_nowait())["ssid"], "家里的WiFi")
        self.assertEqual(client._lines.get_nowait(), '{"status":"idle"}')
        self.assertTrue(client._lines.empty())


if __name__ == "__main__":
    unittest.main()
