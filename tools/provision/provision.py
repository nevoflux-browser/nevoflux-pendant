#!/usr/bin/env python3
"""
NevoFlux Voice Pendant Provisioning Tool (USB Serial / BLE Gateway)

Responsibilities:
1. Run an independent Device Authorization Grant to mint a dedicated account
   token for this pendant (custom User-Agent `nevoflux-pendant/0.1`, or
   Cloudflare 1010 answers 403).
2. Turn the desktop `/pair-device` output (link `.../d/<control_channel_id>/pair`
   + code `X-XXXX-XXXX-XXXX`) into what the pendant stores, matching
   nevoflux-agent `remote/pairing.rs` and `docs/muse-client-protocol.md`:
     - the code is normalized (fixtures/muse/pairing_code.json)
     - channel key = Argon2id(password=code, salt=f"{code}|{channel_id}", m=65536, t=3, p=4)
     - the data channel id is not in the link: it is asked of the head over the
       sealed control channel (`sync` -> `sessions.dataChannelId`), exactly as
       the phone portal learns it.
   ESP32-S3 never runs Argon2id (64 MiB).
3. Perform the physical-presence handshake over USB Serial / BLE (`prov.offer`
   -> BOOT click on the pendant within 15s -> `prov.write`).
"""

from __future__ import annotations

import argparse
import ctypes
import ctypes.util
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


PAIRING_ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ"


def normalize_pairing_code(raw: str) -> str:
    """Canonical `X-XXXX-XXXX-XXXX` form of a pairing code (agent fixtures/muse/pairing_code.json)."""
    chars = [c for c in raw if c.isascii() and c.isalnum()]
    text = "".join(chars).upper().translate(str.maketrans({"I": "1", "L": "1", "O": "0"}))
    if len(text) != 13 or any(c not in PAIRING_ALPHABET for c in text):
        raise ValueError(f"not a pairing code: {raw!r} (expected X-XXXX-XXXX-XXXX)")
    return f"{text[0]}-{text[1:5]}-{text[5:9]}-{text[9:13]}"


def control_channel_from_link(link: str) -> str:
    """The control channel id in a `/pair-device` link (`https://portal.../d/<id>/pair`)."""
    import re

    m = re.search(r"/d/([0-9A-Za-z-]{8,64})(?:/|$)", link.strip())
    if not m:
        raise ValueError(f"not a /pair-device link: {link!r}")
    return m.group(1)


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

    lib = _load_libargon2()
    if lib is None:
        raise RuntimeError("需要 argon2-cffi：pip install argon2-cffi")

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


def _load_libargon2():
    """The system libargon2 (Linux), or None when it is not installed (e.g. Windows)."""
    for name in (ctypes.util.find_library("argon2"), "libargon2.so.1"):
        if not name:
            continue
        try:
            return ctypes.CDLL(name)
        except OSError:
            continue
    return None


def missing_dependencies(need_websocket: bool, need_serial: bool) -> list[str]:
    """pip packages this run needs but cannot import, checked before any network step."""
    import importlib.util

    missing = []
    if importlib.util.find_spec("argon2") is None and _load_libargon2() is None:
        missing.append("argon2-cffi")
    if importlib.util.find_spec("cryptography") is None:
        missing.append("cryptography")
    if need_websocket and importlib.util.find_spec("websocket") is None:
        missing.append("websocket-client")
    if need_serial and importlib.util.find_spec("serial") is None:
        missing.append("pyserial")
    return missing


def derive_channel_key(
    code: str,
    channel_id: str,
    memory_kib: int = KDF_MEMORY_KIB,
    iterations: int = KDF_ITERATIONS,
    parallelism: int = KDF_PARALLELISM,
) -> bytes:
    """Argon2id(code, f"{code}|{channel_id}") — nevoflux-agent remote/crypto.rs `derive_channel_key`."""
    return argon2id_raw(
        code.encode("utf-8"), f"{code}|{channel_id}".encode("utf-8"), memory_kib, iterations, parallelism
    )


def derive_pairing_material(
    pairing_code: str,
    control_channel_id: str,
    data_channel_id: str,
    memory_kib: int = KDF_MEMORY_KIB,
    iterations: int = KDF_ITERATIONS,
    parallelism: int = KDF_PARALLELISM,
) -> DerivedPairingMaterial:
    """Both channel keys for a pairing whose two channel ids are known."""
    if not control_channel_id or not data_channel_id:
        raise ValueError("both channel ids are required")
    code = normalize_pairing_code(pairing_code)
    return DerivedPairingMaterial(
        control_channel_id=control_channel_id,
        control_key=derive_channel_key(code, control_channel_id, memory_kib, iterations, parallelism),
        data_channel_id=data_channel_id,
        data_key=derive_channel_key(code, data_channel_id, memory_kib, iterations, parallelism),
    )


def seal_frame(key: bytes, plaintext: bytes, nonce: Optional[bytes] = None) -> bytes:
    """nonce(12) || AES-256-GCM(ciphertext || tag), no AAD — remote/channel_codec.rs."""
    import os

    from cryptography.hazmat.primitives.ciphers.aead import AESGCM  # type: ignore

    n = nonce if nonce is not None else os.urandom(12)
    return n + AESGCM(key).encrypt(n, plaintext, None)


def open_frame(key: bytes, wire: bytes) -> Optional[bytes]:
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM  # type: ignore

    if len(wire) < 28:
        return None
    try:
        return AESGCM(key).decrypt(wire[:12], wire[12:], None)
    except Exception:  # noqa: BLE001 - a frame that does not open is just dropped
        return None


def mint_relay_jwt(account_token: str, base_url: str = DEFAULT_BASE_URL) -> str:
    """A 15-minute relay JWT (`set-auth-jwt` header, or `{token}` body)."""
    req = urllib.request.Request(
        f"{base_url.rstrip('/')}/api/auth/token",
        headers={"Authorization": f"Bearer {account_token}", "User-Agent": USER_AGENT, "Accept": "application/json"},
    )
    with urllib.request.urlopen(req, timeout=15) as resp:
        jwt = resp.headers.get("set-auth-jwt")
        body = resp.read().decode("utf-8", errors="ignore")
    if jwt:
        return jwt
    token = json.loads(body or "{}").get("token")
    if not token:
        raise RuntimeError("the account did not mint a relay token (signed in to the same account as the desktop?)")
    return str(token)


def fetch_data_channel_id(
    control_channel_id: str,
    control_key: bytes,
    jwt: str,
    relay_url: str = DEFAULT_RELAY_URL,
    timeout_sec: float = 25.0,
) -> str:
    """Ask the head for this pairing's data channel id over the sealed control channel."""
    try:
        import websocket  # type: ignore  # pip install websocket-client
    except ImportError as exc:  # pragma: no cover - depends on host packages
        raise RuntimeError("需要 websocket-client：pip install websocket-client cryptography") from exc

    url = f"{relay_url.rstrip('/')}/?c={control_channel_id}&t={jwt}"
    ws = websocket.create_connection(url, timeout=10, header=[f"User-Agent: {USER_AGENT}"])
    try:
        sync = json.dumps({"k": "frame", "frame": {"kind": "sync"}}, separators=(",", ":")).encode()
        ws.send_binary(seal_frame(control_key, sync))
        deadline = time.monotonic() + timeout_sec
        warned = False
        while time.monotonic() < deadline:
            ws.settimeout(max(0.5, deadline - time.monotonic()))
            try:
                opcode, data = ws.recv_data()
            except websocket.WebSocketTimeoutException:
                break
            if opcode == websocket.ABNF.OPCODE_TEXT:
                text = data.decode("utf-8", errors="ignore")
                if '"peers"' in text and '"n":0' in text and not warned:
                    print("    电脑端 NevoFlux 尚未连上 Relay，等待中…")
                    warned = True
                continue
            plain = open_frame(control_key, data)
            if plain is None:
                raise RuntimeError("控制通道的回复解不开：配对码与链接不是同一次 /pair-device 生成的？")
            frame = json.loads(plain).get("frame") or {}
            if frame.get("kind") == "sessions" and frame.get("dataChannelId"):
                return str(frame["dataChannelId"])
    finally:
        ws.close()
    raise TimeoutError("电脑端没有回复会话列表（NevoFlux 是否在运行并已登录？）")


def request_independent_device_token(
    base_url: str = DEFAULT_BASE_URL,
    client_id: str = "nevoflux-daemon",
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
        data=json.dumps({"client_id": client_id, "scope": "openid profile email"}).encode("utf-8"),
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
    parser.add_argument("--link", help="The link /pair-device printed (https://.../d/<control_channel_id>/pair)")
    parser.add_argument("--control-channel", help="Control channel id (instead of --link)")
    parser.add_argument(
        "--data-channel", help="Data channel id, if known (otherwise asked of the head over the control channel)"
    )
    parser.add_argument("--client-id", default="nevoflux-daemon", help="Device Grant client_id")
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
    parser.add_argument(
        "--port",
        help="USB serial port (/dev/ttyACM0, COM5), tcp://host:port, or ble / ble:NevoFlux-XXXX; "
        "if omitted, prints JSON bundle",
    )
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate")
    args = parser.parse_args(argv)

    if args.wifi_only:
        return _main_wifi_only(args)
    if not args.code:
        parser.error("--code is required unless --wifi-only is given")
    control_channel = args.control_channel or (control_channel_from_link(args.link) if args.link else "")
    if not control_channel:
        parser.error("--link (or --control-channel) from /pair-device is required")
    code = normalize_pairing_code(args.code)

    port = args.port or ""
    missing = missing_dependencies(
        need_websocket=not args.data_channel,
        need_serial=bool(port) and not port.startswith(("tcp://", "ble")),
    )
    if missing:
        print(f"缺少 Python 依赖，请先运行：pip install {' '.join(missing)}", file=sys.stderr)
        return 2

    account_token = args.token
    if not account_token:
        print("[1/4] 正在通过 Device Grant 为本吊坠签发独立账号会话...")
        account_token = request_independent_device_token(args.base_url, client_id=args.client_id)
    else:
        print("[1/4] 使用已提供的独立 account_token...")

    print("[2/4] 正在派生控制通道密钥 (Argon2id m=65536, t=3, p=4)...")
    control_key = derive_channel_key(code, control_channel)

    data_channel = args.data_channel
    if not data_channel:
        print("[3/4] 正在经 Relay 向电脑端询问数据通道 ID...")
        jwt = mint_relay_jwt(account_token, args.base_url)
        data_channel = fetch_data_channel_id(control_channel, control_key, jwt, args.relay_url)
    material = DerivedPairingMaterial(
        control_channel_id=control_channel,
        control_key=control_key,
        data_channel_id=data_channel,
        data_key=derive_channel_key(code, data_channel),
    )

    if not args.port:
        print("[4/4] 未指定 --port，输出派生结果摘要：")
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

    print(f"[4/4] 正在连接 {args.port}，请在吊坠提示后 15 秒内按一下 BOOT 键确认...")
    with _open_line_transport(args.port, args.baud) as rpc:
        execute_provisioning_handshake(
            send_line=rpc,
            material=material,
            account_token=account_token,
            wifi_ssid=args.ssid,
            wifi_pass=args.password,
            relay_url=args.relay_url,
        )
    print("[OK] 配对凭证与 WiFi 配置已成功写入吊坠 NVS！")
    return 0


BLE_NUS_SERVICE = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
BLE_NUS_RX = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # write: PC -> pendant
BLE_NUS_TX = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # notify: pendant -> PC
BLE_NAME_PREFIX = "NevoFlux-"


class _BleLineClient:
    """Blocking newline-delimited JSON-RPC over the pendant's BLE Nordic UART Service.

    `target` is "" (first device advertising "NevoFlux-*"), a full device name
    such as "NevoFlux-1A2B", or a BLE address. Requires `pip install bleak`.
    """

    def __init__(self, target: str, reply_timeout_sec: float = 5.0) -> None:
        import asyncio
        import queue
        import threading

        try:
            import bleak  # type: ignore  # noqa: F401
        except ImportError as exc:  # pragma: no cover - depends on host packages
            raise RuntimeError("BLE 配网需要 bleak：pip install bleak") from exc

        self._target = target
        self._timeout = reply_timeout_sec
        self._lines: "queue.Queue[str]" = queue.Queue()
        self._buf = b""
        self._loop = asyncio.new_event_loop()
        self._thread = threading.Thread(target=self._loop.run_forever, daemon=True)
        self._thread.start()
        self._client = None
        self._run(self._connect(), timeout=40.0)

    def _run(self, coro, timeout: float):
        import asyncio

        return asyncio.run_coroutine_threadsafe(coro, self._loop).result(timeout)

    async def _connect(self) -> None:
        from bleak import BleakClient, BleakScanner  # type: ignore

        target = self._target
        # MAC address (Windows/Linux) or CoreBluetooth UUID (macOS) -> connect directly
        is_address = ":" in target or (len(target) == 36 and target.count("-") == 4)
        if is_address:
            device = await BleakScanner.find_device_by_address(target, timeout=15.0)
        else:
            want = target or BLE_NAME_PREFIX

            def match(dev, adv) -> bool:
                name = (adv.local_name or dev.name or "")
                return name == want if target else name.startswith(BLE_NAME_PREFIX)

            device = await BleakScanner.find_device_by_filter(match, timeout=15.0)
        if device is None:
            raise RuntimeError(
                f"未找到蓝牙设备 {target or BLE_NAME_PREFIX + '*'}"
                "（请在吊坠 Launcher 界面点「开启蓝牙配网」，3 分钟内有效）"
            )
        print(f"    已找到 {device.name or ''} [{device.address}]，正在连接...")
        self._client = BleakClient(device)
        await self._client.connect()
        await self._client.start_notify(BLE_NUS_TX, self._on_notify)

    def _on_notify(self, _sender, data: bytearray) -> None:
        self._buf += bytes(data)
        while b"\n" in self._buf:
            raw, self._buf = self._buf.split(b"\n", 1)
            text = raw.decode("utf-8", errors="ignore").strip()
            if text:
                self._lines.put(text)

    async def _write(self, payload: bytes) -> None:
        chunk = max(20, (getattr(self._client, "mtu_size", 23) or 23) - 3)
        for i in range(0, len(payload), chunk):
            await self._client.write_gatt_char(BLE_NUS_RX, payload[i : i + chunk], response=True)

    def rpc(self, line: str) -> str:
        import queue

        while not self._lines.empty():
            self._lines.get_nowait()
        self._run(self._write((line + "\n").encode("utf-8")), timeout=self._timeout + 10.0)
        deadline = time.monotonic() + self._timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError("No JSON reply from pendant over BLE (is the Launcher running?)")
            try:
                raw = self._lines.get(timeout=remaining)
            except queue.Empty:
                continue
            if raw.startswith("{") and raw.endswith("}"):
                return raw

    def close(self) -> None:
        try:
            if self._client is not None:
                self._run(self._client.disconnect(), timeout=10.0)
        finally:
            self._loop.call_soon_threadsafe(self._loop.stop)
            self._thread.join(timeout=5.0)


def _open_line_transport(port: str, baud: int):
    """Return a context manager yielding `send_line(request_json) -> response_json`.

    `port` is a serial port (`/dev/ttyACM0`, `COM5`), `tcp://host:port` (emulator)
    or `ble` / `ble:<NevoFlux-XXXX or address>` (Bluetooth LE).
    """
    import contextlib

    @contextlib.contextmanager
    def _ctx():
        if port == "ble" or port.startswith("ble:"):
            client = _BleLineClient(port[4:] if port.startswith("ble:") else "")
            try:
                yield client.rpc
            finally:
                client.close()
            return

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
        print("[错误] --wifi-only 需要 --port (例如 /dev/ttyACM0、COM5 或 ble)", file=sys.stderr)
        return 2
    print(f"[1/2] 正在连接 {args.port}，请在吊坠屏幕提示后 15 秒内单击 BOOT 键确认...")
    with _open_line_transport(args.port, args.baud) as rpc:
        execute_wifi_provisioning(send_line=rpc, wifi_ssid=args.ssid, wifi_pass=args.password)
    print(f"[2/2] [OK] WiFi \"{args.ssid}\" 已写入吊坠 NVS，设备将立即尝试连接。")
    return 0


if __name__ == "__main__":
    sys.exit(main())
