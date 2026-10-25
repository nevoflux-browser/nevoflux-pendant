#!/usr/bin/env python3
import json
import os
import unittest

from provision import (
    _BleLineClient,
    DerivedPairingMaterial,
    control_channel_from_link,
    derive_channel_key,
    derive_pairing_material,
    execute_provisioning_handshake,
    normalize_pairing_code,
    open_frame,
    seal_frame,
)

FIXTURES = os.path.join(
    os.path.dirname(__file__), "..", "..", "..", "nevoflux-agent", "crates", "daemon", "tests", "fixtures", "muse"
)


def _fixture(name: str):
    path = os.path.join(FIXTURES, name)
    if not os.path.exists(path):
        return None
    with open(path, encoding="utf-8") as f:
        return json.load(f)


class TestProvisioning(unittest.TestCase):
    def test_pairing_code_normalization_matches_agent_fixture(self) -> None:
        fx = _fixture("pairing_code.json") or {
            "valid": [{"input": "x abcd efgh jkmn", "canonical": "X-ABCD-EFGH-JKMN"},
                      {"input": "X-ILOI-OLIO-1234", "canonical": "X-1101-0110-1234"}],
            "invalid": ["X-ABCD-EFGH-JKM", "X-ABCU-EFGH-JKMN"],
        }
        for case in fx["valid"]:
            self.assertEqual(normalize_pairing_code(case["input"]), case["canonical"], case["input"])
        for bad in fx["invalid"]:
            with self.assertRaises(ValueError):
                normalize_pairing_code(bad)

    def test_link_names_the_control_channel(self) -> None:
        cid = "2f1c4a90-7b3e-4d1a-9c58-0e6a2b7d4f31"
        self.assertEqual(control_channel_from_link(f"https://portal.nevoflux.app/d/{cid}/pair"), cid)
        with self.assertRaises(ValueError):
            control_channel_from_link("https://portal.nevoflux.app/settings")

    def test_channel_key_matches_agent_kdf_vectors(self) -> None:
        fx = _fixture("kdf.json")
        if fx is None:
            self.skipTest("nevoflux-agent fixtures not checked out next to this repo")
        for v in fx["vectors"]:
            self.assertEqual(derive_channel_key(v["code"], v["channel_id"]).hex(), v["key_hex"])

    def test_seal_matches_agent_vector(self) -> None:
        try:
            import cryptography  # type: ignore  # noqa: F401
        except ImportError:
            self.skipTest("cryptography not installed")
        fx = _fixture("seal.json")
        if fx is None:
            self.skipTest("nevoflux-agent fixtures not checked out next to this repo")
        key = bytes.fromhex(fx["key_hex"])
        plain = json.dumps(fx["plaintext"], separators=(",", ":")).encode()
        sealed = seal_frame(key, plain, bytes.fromhex(fx["nonce_hex"]))
        self.assertEqual(sealed.hex(), fx["sealed_hex"])
        self.assertEqual(open_frame(key, sealed), plain)
        self.assertIsNone(open_frame(bytes(32), sealed))

    def test_material_for_both_channels(self) -> None:
        mat = derive_pairing_material("X-ABCD-EFGH-JKMN", "ctl-uuid", "data-uuid",
                                      memory_kib=1024, iterations=1, parallelism=4)
        self.assertEqual(mat.control_channel_id, "ctl-uuid")
        self.assertEqual(len(mat.control_key), 32)
        self.assertNotEqual(mat.control_key, mat.data_key)

    def test_handshake_with_boot_confirmation(self) -> None:
        mat = derive_pairing_material("X-ABCD-EFGH-JKMN", "2f1c4a90-7b3e-4d1a-9c58-0e6a2b7d4f31",
                                      "9a8b7c6d-7b3e-4d1a-9c58-0e6a2b7d4f31", memory_kib=1024, iterations=1, parallelism=4)
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
