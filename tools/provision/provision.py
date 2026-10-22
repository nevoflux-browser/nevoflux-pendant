#!/usr/bin/env python3
"""
NevoFlux Voice Pendant Provisioning Tool (USB Serial / BLE Gateway)

Responsibilities:
1. Run an independent OAuth2 / Better-Auth Device Authorization Grant to mint a
   dedicated 7-day `account_token` for this pendant (with custom User-Agent
   `nevoflux-pendant/0.1` so Cloudflare 1010 does not return 403).
2. Derive dual channel IDs and 32-byte AES-256-GCM keys on the PC from the
   `/pair-device` code, matching `nevoflux-portal/src/lib/chat/crypto.ts`:
     - control_channel_id = Hex(HMAC-SHA256(code, "nevoflux-channel-v1")[0..16])
     - control_key        = Argon2id(password=code, salt=f"{code}|{control_channel_id}", m=65536, t=3, p=4)
     - data_channel_id    = Hex(HMAC-SHA256(code, "nevoflux-data-channel-v1")[0..16])
     - data_key           = Argon2id(password=code, salt=f"{code}|{data_channel_id}",     m=65536, t=3, p=4)
3. Perform the physical-presence handshake over USB Serial (`prov.offer` -> wait
   for user to click BOOT button on pendant within 15s -> `prov.write`).
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.util
import hashlib
import hmac
import json
import socket
import sys
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Callable, Optional

USER_AGENT = "nevoflux-pendant/0.1"
DEFAULT_BASE_URL = "https://nevoflux.app"
DEFAULT_RELAY_URL = "wss://relay.nevoflux.app"

KDF_MEMORY_KIB = 65536
KDF_ITERATIONS = 3
KDF_PARALLELISM = 4
KDF_HASH_LEN = 32


@dataclass(frozen=True)
class DerivedPairingMaterial:
    control_channel_id: str
    control_key: bytes
    data_channel_id: str
    data_key: bytes

    @property
    def control_key_hex(self) -> str:
        return self.control_key.hex()

    @property
    def data_key_hex(self) -> str:
        return self.data_key.hex()


def derive_channel_id(pairing_code: str, domain_label: str) -> str:
    """
    Matches `Hex(HMAC-SHA256(code, domain_label)[0..16])` (32 lowercase hex chars).
    """
    mac = hmac.new(
        key=pairing_code.encode("utf-8"),
        msg=domain_label.encode("utf-8"),
        digestmod=hashlib.sha256,
    ).digest()
    return mac[:16].hex()


def argon2id_raw(
    password: bytes,
    salt: bytes,
    memory_kib: int = KDF_MEMORY_KIB,
    iterations: int = KDF_ITERATIONS,
    parallelism: int = KDF_PARALLELISM,
    hash_len: int = KDF_HASH_LEN,
) -> bytes:
    """
    Compute raw Argon2id v0x13 hash using either `argon2-cffi` (if installed)
    or system `libargon2.so.1` via ctypes (zero external pip dependencies).
    """
    try:
        from argon2.low_level import Type, hash_secret_raw  # type: ignore

        return hash_secret_raw(
            secret=password,
            salt=salt,
            time_cost=iterations,
            memory_cost=memory_kib,
            parallelism=parallelism,
            hash_len=hash_len,
            type=Type.ID,
        )
    except ImportError:
        pass

    lib_path = ctypes.util.find_library("argon2") or "libargon2.so.1"
    lib = ctypes.CDLL(lib_path)

    # int argon2id_hash_raw(const uint32_t t_cost, const uint32_t m_cost,
    #                       const uint32_t parallelism, const void *pwd,
    #                       const size_t pwdlen, const void *salt,
    #                       const size_t saltlen, void *hash, const size_t hashlen);
    fn = lib.argon2id_hash_raw
    fn.argtypes = [
        ctypes.c_uint32,
        ctypes.c_uint32,
        ctypes.c_uint32,
        ctypes.c_char_p,
        ctypes.c_size_t,
        ctypes.c_char_p,
        ctypes.c_size_t,
        ctypes.c_void_p,
        ctypes.c_size_t,
    ]
    fn.restype = ctypes.c_int

    out_buf = ctypes.create_string_buffer(hash_len)
    rc = fn(
        iterations,
        memory_kib,
        parallelism,
        password,
        len(password),
        salt,
        len(salt),
        out_buf,
        hash_len,
    )
    if rc != 0:
        raise RuntimeError(f"argon2id_hash_raw failed with error code {rc}")
    return out_buf.raw


def derive_pairing_material(
    pairing_code: str,
    memory_kib: int = KDF_MEMORY_KIB,
    iterations: int = KDF_ITERATIONS,
    parallelism: int = KDF_PARALLELISM,
) -> DerivedPairingMaterial:
    """
    Derive both control and data channel IDs and 32-byte keys from `pairing_code`.
    """
    if not pairing_code:
        raise ValueError("pairing_code must not be empty")

    ctrl_cid = derive_channel_id(pairing_code, "nevoflux-channel-v1")
    data_cid = derive_channel_id(pairing_code, "nevoflux-data-channel-v1")

    pwd_bytes = pairing_code.encode("utf-8")
    ctrl_salt = f"{pairing_code}|{ctrl_cid}".encode("utf-8")
    data_salt = f"{pairing_code}|{data_cid}".encode("utf-8")

    ctrl_key = argon2id_raw(pwd_bytes, ctrl_salt, memory_kib, iterations, parallelism)
    data_key = argon2id_raw(pwd_bytes, data_salt, memory_kib, iterations, parallelism)

    return DerivedPairingMaterial(
        control_channel_id=ctrl_cid,
        control_key=ctrl_key,
        data_channel_id=data_cid,
        data_key=data_key,
    )


def request_independent_device_token(
    base_url: str = DEFAULT_BASE_URL,
    client_id: str = "nevoflux-pendant",
    timeout_sec: int = 180,
) -> str:
    """
    Run Device Authorization Grant to issue a standalone 7-day session token
    for this specific pendant (never reusing the desktop token).
    """
    code_url = f"{base_url.rstrip('/')}/api/auth/device/code"
    token_url = f"{base_url.rstrip('/')}/api/auth/device/token"

    req = urllib.request.Request(
        code_url,
        data=json.dumps({"client_id": client_id, "scope": "relay:token"}).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "User-Agent": USER_AGENT,
        },
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=15) as resp:
        info = json.loads(resp.read().decode("utf-8"))

    device_code = info["device_code"]
    user_code = info.get("user_code", "")
    verify_uri = info.get("verification_uri_complete") or info.get("verification_uri", "")
    interval = int(info.get("interval", 5))

    print(f"\n[Device Grant] 请在浏览器打开以下链接为本吊坠授权独立会话：")
    print(f"  URL : {verify_uri}")
    if user_code:
        print(f"  Code: {user_code}\n")

    deadline = time.monotonic() + timeout_sec
    while time.monotonic() < deadline:
        time.sleep(interval)
        poll_req = urllib.request.Request(
            token_url,
            data=json.dumps(
                {
                    "client_id": client_id,
                    "device_code": device_code,
                    "grant_type": "urn:ietf:params:oauth:grant-type:device_code",
                }
            ).encode("utf-8"),
            headers={
                "Content-Type": "application/json",
                "User-Agent": USER_AGENT,
            },
            method="POST",
        )
        try:
            with urllib.request.urlopen(poll_req, timeout=15) as poll_resp:
                tok_data = json.loads(poll_resp.read().decode("utf-8"))
                token = tok_data.get("access_token") or tok_data.get("token")
                if token:
                    return str(token)
        except urllib.error.HTTPError as e:
            body = e.read().decode("utf-8", errors="ignore")
            if "authorization_pending" in body:
                continue
            if "slow_down" in body:
                interval += 2
                continue
            raise RuntimeError(f"Device token exchange failed ({e.code}): {body}") from e

    raise TimeoutError("Timed out waiting for user browser approval in Device Grant")


def execute_provisioning_handshake(
    send_line: Callable[[str], str],
    material: DerivedPairingMaterial,
    account_token: str,
    wifi_ssid: str = "",
    wifi_pass: str = "",
    relay_url: str = DEFAULT_RELAY_URL,
    device_name: Optional[str] = None,
    poll_interval_sec: float = 0.5,
    confirm_timeout_sec: float = 15.0,
) -> None:
    """
    Execute the 3-step physical-presence provisioning handshake over a line-oriented
    transport (`send_line(request_json) -> response_json`):
      1. `prov.offer` -> triggers prompt on pendant screen
      2. Poll `prov.status` until user clicks BOOT button (`status == "ready"`)
      3. `prov.write` -> writes WiFi + NevoFlux credentials into NVS
    """
    _offer_and_wait_boot_confirm(send_line, device_name, poll_interval_sec, confirm_timeout_sec)

    write_payload = {
        "cmd": "prov.write",
        "wifi_ssid": wifi_ssid,
        "wifi_pass": wifi_pass,
        "relay_url": relay_url,
        "account_token": account_token,
        "control_channel_id": material.control_channel_id,
        "control_key_hex": material.control_key_hex,
        "data_channel_id": material.data_channel_id,
        "data_key_hex": material.data_key_hex,
    }
    write_resp = json.loads(send_line(json.dumps(write_payload, ensure_ascii=False)))
    if write_resp.get("status") != "ok":
        raise RuntimeError(f"prov.write failed: {write_resp}")


def execute_wifi_provisioning(
    send_line: Callable[[str], str],
    wifi_ssid: str,
    wifi_pass: str = "",
    device_name: Optional[str] = None,
    poll_interval_sec: float = 0.5,
    confirm_timeout_sec: float = 15.0,
) -> None:
    """WiFi-only variant: `prov.offer` -> BOOT confirm -> `prov.wifi` (NevoFlux pairing untouched)."""
    if not wifi_ssid:
        raise ValueError("wifi_ssid is required")
    _offer_and_wait_boot_confirm(send_line, device_name, poll_interval_sec, confirm_timeout_sec)
    resp = json.loads(
        send_line(
            json.dumps({"cmd": "prov.wifi", "wifi_ssid": wifi_ssid, "wifi_pass": wifi_pass}, ensure_ascii=False)
        )
    )
    if resp.get("status") != "ok":
        raise RuntimeError(f"prov.wifi failed: {resp}")


def _offer_and_wait_boot_confirm(
    send_line: Callable[[str], str],
    device_name: Optional[str],
    poll_interval_sec: float,
    confirm_timeout_sec: float,
) -> None:
    host_label = device_name or socket.gethostname() or "NevoFlux-PC"
    offer_resp = json.loads(
        send_line(json.dumps({"cmd": "prov.offer", "device": host_label}, ensure_ascii=False))
    )
    if offer_resp.get("status") not in ("wait_boot", "ready"):
        raise RuntimeError(f"Unexpected prov.offer response: {offer_resp}")

    deadline = time.monotonic() + confirm_timeout_sec
    confirmed = offer_resp.get("status") == "ready"
    while not confirmed:
        if time.monotonic() >= deadline:
            raise TimeoutError("Pendant BOOT button confirmation timed out (15s)")
        if poll_interval_sec > 0:
            time.sleep(poll_interval_sec)
        st_resp = json.loads(send_line(json.dumps({"cmd": "prov.status"})))
        status = st_resp.get("status")
        if status == "ready":
            confirmed = True
        elif status == "idle":
            raise RuntimeError("Provisioning offer expired or was cancelled on pendant")


def main(argv: Optional[list[str]] = None) -> int:
    parser = argparse.ArgumentParser(description="NevoFlux Voice Pendant Provisioning Tool")
    parser.add_argument("--code", help="Pairing code from desktop /pair-device (not needed with --wifi-only)")
    parser.add_argument(
        "--wifi-only",
        action="store_true",
        help="Only write WiFi SSID/password (prov.wifi); keeps existing NevoFlux pairing",
    )
    parser.add_argument("--token", help="7-day independent account_token (if omitted, runs Device Grant)")
    parser.add_argument("--ssid", default="", help="WiFi SSID to write to shared pendant NVS")
    parser.add_argument("--password", default="", help="WiFi password to write to shared pendant NVS")
    parser.add_argument("--relay-url", default=DEFAULT_RELAY_URL, help="Relay WebSocket URL")
    parser.add_argument("--base-url", default=DEFAULT_BASE_URL, help="NevoFlux web base URL for Device Grant")
    parser.add_argument("--port", help="USB Serial port (e.g. /dev/ttyACM0); if omitted, prints JSON bundle")
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate")
    args = parser.parse_args(argv)

    if args.wifi_only:
        return _main_wifi_only(args)
    if not args.code:
        parser.error("--code is required unless --wifi-only is given")

    print("[1/3] 正在派生双通道 ID 与 AES-256-GCM 密钥 (Argon2id m=65536, t=3, p=4)...")
    material = derive_pairing_material(args.code)

    account_token = args.token
    if not account_token:
        print("[2/3] 正在通过 Device Grant 为本吊坠签发独立 7 天 session token...")
        account_token = request_independent_device_token(args.base_url)
    else:
        print("[2/3] 使用已提供的独立 account_token...")

    if not args.port:
        print("[3/3] 未指定 --port，输出派生结果摘要：")
        print(
            json.dumps(
                {
                    "relay_url": args.relay_url,
                    "control_channel_id": material.control_channel_id,
                    "control_key_hex": material.control_key_hex,
                    "data_channel_id": material.data_channel_id,
                    "data_key_hex": material.data_key_hex,
                },
                indent=2,
            )
        )
        return 0

    if args.port.startswith(("tcp://", "socket://")):
        addr_part = args.port.split("://", 1)[1]
        host, port_str = addr_part.rsplit(":", 1)
        print(f"[3/3] 正在连接模拟器 UART TCP 端口 {host}:{port_str}...")
        with socket.create_connection((host, int(port_str)), timeout=5.0) as sock:
            sock_file = sock.makefile("rwb", buffering=0)

            def tcp_rpc(line: str) -> str:
                sock_file.write((line + "\n").encode("utf-8"))
                while True:
                    raw_bytes = sock_file.readline()
                    if not raw_bytes:
                        raise RuntimeError("UART TCP connection closed before JSON response")
                    raw = raw_bytes.decode("utf-8", errors="ignore").strip()
                    if raw.startswith("{") and raw.endswith("}"):
                        return raw

            execute_provisioning_handshake(
                send_line=tcp_rpc,
                material=material,
                account_token=account_token,
                wifi_ssid=args.ssid,
                wifi_pass=args.password,
                relay_url=args.relay_url,
            )
        print("[OK] 配对凭证与 WiFi 配置已成功写入模拟器 NVS！")
        return 0

    import serial  # type: ignore

    print(f"[3/3] 正在连接串口 {args.port}，请在吊坠提示后 15 秒内按一下 BOOT 键确认...")
    with serial.Serial(args.port, args.baud, timeout=2.0) as ser:
        def serial_rpc(line: str) -> str:
            ser.write((line + "\n").encode("utf-8"))
            ser.flush()
            while True:
                raw = ser.readline().decode("utf-8", errors="ignore").strip()
                if raw.startswith("{") and raw.endswith("}"):
                    return raw

        execute_provisioning_handshake(
            send_line=serial_rpc,
            material=material,
            account_token=account_token,
            wifi_ssid=args.ssid,
            wifi_pass=args.password,
            relay_url=args.relay_url,
        )
    print("[OK] 配对凭证与 WiFi 配置已成功写入吊坠 NVS！")
    return 0


def _open_line_transport(port: str, baud: int):
    """Return a context manager yielding `send_line(request_json) -> response_json`."""
    import contextlib

    @contextlib.contextmanager
    def _ctx():
        if port.startswith(("tcp://", "socket://")):
            host, port_str = port.split("://", 1)[1].rsplit(":", 1)
            with socket.create_connection((host, int(port_str)), timeout=5.0) as sock:
                f = sock.makefile("rwb", buffering=0)

                def rpc(line: str) -> str:
                    f.write((line + "\n").encode("utf-8"))
                    while True:
                        raw_bytes = f.readline()
                        if not raw_bytes:
                            raise RuntimeError("UART TCP connection closed before JSON response")
                        raw = raw_bytes.decode("utf-8", errors="ignore").strip()
                        if raw.startswith("{") and raw.endswith("}"):
                            return raw

                yield rpc
            return

        import serial  # type: ignore

        with serial.Serial(port, baud, timeout=1.0) as ser:
            ser.reset_input_buffer()

            def rpc(line: str) -> str:
                ser.write((line + "\n").encode("utf-8"))
                ser.flush()
                deadline = time.monotonic() + 5.0
                while time.monotonic() < deadline:
                    raw = ser.readline().decode("utf-8", errors="ignore").strip()
                    # Firmware log lines share the console; only JSON lines are RPC replies.
                    if raw.startswith("{") and raw.endswith("}"):
                        return raw
                raise TimeoutError(f"No JSON reply from pendant on {port} (is the Launcher running?)")

            yield rpc

    return _ctx()


def _main_wifi_only(args: argparse.Namespace) -> int:
    if not args.ssid:
        print("[错误] --wifi-only 需要 --ssid", file=sys.stderr)
        return 2
    if not args.port:
        print("[错误] --wifi-only 需要 --port (例如 /dev/ttyACM0 或 COM5)", file=sys.stderr)
        return 2
    print(f"[1/2] 正在连接 {args.port}，请在吊坠屏幕提示后 15 秒内单击 BOOT 键确认...")
    with _open_line_transport(args.port, args.baud) as rpc:
        execute_wifi_provisioning(send_line=rpc, wifi_ssid=args.ssid, wifi_pass=args.password)
    print(f"[2/2] [OK] WiFi \"{args.ssid}\" 已写入吊坠 NVS，设备将立即尝试连接。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
