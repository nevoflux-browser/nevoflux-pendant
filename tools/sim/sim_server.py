#!/usr/bin/env python3
"""
Interactive Web Simulator for the NevoFlux Voice Pendant (1.43" 466x466 Round AMOLED).

Loads `libnevoflux_sim.so` (compiled directly from our C firmware components:
`nf_proto`, `prov_nvs`, `bsp_143c`, `apps/nevoflux`) and serves an interactive
hardware workbench in the browser (default: http://0.0.0.0:8765).
"""

from __future__ import annotations

import argparse
import ctypes
import json
import os
import pathlib
import subprocess
import tempfile
import threading
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs, urlparse

SIM_DIR = pathlib.Path(__file__).resolve().parent
LIB_PATH = SIM_DIR / "libnevoflux_sim.so"

HTML_PAGE = """<!DOCTYPE html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8" />
  <title>NevoFlux Pendant 1.43" (466×466) LVGL / Firmware Simulator</title>
  <style>
    :root {
      --bg: #0b0f17;
      --panel: #131b29;
      --border: #25344d;
      --accent: #22d3ee;
      --text: #e2e8f0;
      --muted: #94a3b8;
    }
    * { box-sizing: border-box; }
    body {
      margin: 0;
      padding: 28px;
      background: var(--bg);
      color: var(--text);
      font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
      display: flex;
      gap: 28px;
      justify-content: center;
      align-items: flex-start;
      flex-wrap: wrap;
    }
    .watch-housing {
      background: radial-gradient(circle at 35% 30%, #1e293b, #090d14 75%);
      border: 3px solid #334155;
      border-radius: 50%;
      padding: 22px;
      box-shadow: 0 20px 50px rgba(0, 0, 0, 0.75);
      position: relative;
      display: flex;
      align-items: center;
      justify-content: center;
    }
    #amoled {
      width: 466px;
      height: 466px;
      border-radius: 50%;
      cursor: pointer;
      user-select: none;
      display: block;
    }
    .side-buttons {
      display: flex;
      flex-direction: column;
      gap: 14px;
      margin-top: 16px;
    }
    .panel {
      background: var(--panel);
      border: 1px solid var(--border);
      border-radius: 16px;
      padding: 20px;
      width: 460px;
    }
    h2 {
      margin: 0 0 12px 0;
      font-size: 17px;
      color: var(--accent);
    }
    .btn-row {
      display: flex;
      flex-wrap: wrap;
      gap: 10px;
      margin-bottom: 14px;
    }
    button {
      background: #1e293b;
      color: var(--text);
      border: 1px solid #3b4f6b;
      border-radius: 10px;
      padding: 9px 14px;
      font-size: 13px;
      font-weight: 600;
      cursor: pointer;
      transition: all 0.15s;
    }
    button:hover { border-color: var(--accent); background: #26354d; }
    button.boot-hold {
      background: #065f46;
      border-color: #10b981;
      font-size: 14px;
      padding: 12px 18px;
    }
    button.boot-hold:active { background: #059669; transform: scale(0.98); }
    pre {
      background: #080c12;
      border: 1px solid #1e293b;
      border-radius: 10px;
      padding: 12px;
      font-size: 12px;
      color: #a5f3fc;
      overflow-x: auto;
      margin: 0;
      white-space: pre-wrap;
      word-break: break-all;
    }
    .hint { font-size: 12px; color: var(--muted); margin-bottom: 12px; line-height: 1.5; }
  </style>
</head>
<body>
  <div>
    <div class="watch-housing">
      <img id="amoled" src="/frame.png" width="466" height="466" alt="466x466 Round Screen" />
    </div>
    <div class="hint" style="text-align:center; margin-top:12px;">
      可直接点击 466×466 圆屏上的卡片或底部触控按钮（坐标实时映射至 C 固件）
    </div>
  </div>

  <div class="panel">
    <h2>1. 物理按键模拟 (BOOT GPIO0 / PWR GPIO1)</h2>
    <div class="btn-row">
      <button class="boot-hold"
              onmousedown="api('/api/boot_down')"
              onmouseup="api('/api/boot_up')"
              ontouchstart="api('/api/boot_down'); event.preventDefault();"
              ontouchend="api('/api/boot_up'); event.preventDefault();">
        🎙️ 按住 BOOT 说话 (松手成轮)
      </button>
      <button onclick="api('/api/boot_click')">单击 BOOT (确认配对)</button>
      <button onclick="api('/api/boot_double')">双击 BOOT (切换会议模式)</button>
      <button onclick="api('/api/pwr_click')">按 PWR 键 (取消/停止)</button>
    </div>

    <h2>2. 模拟 Daemon / Muse OAuth2 (RFC 8628) 事件</h2>
    <div class="btn-row">
      <button onclick="api('/api/sim_partial')">推 speech_partial (流式转写)</button>
      <button onclick="api('/api/sim_final')">推 speech_final (定稿触发 send)</button>
      <button onclick="api('/api/sim_tts_24k')">推 24kHz TTS (Kokoro 播报)</button>
      <button onclick="api('/api/sim_tts_48k')">推 48kHz TTS (MOSS 播报)</button>
      <button onclick="api('/api/sim_prov_offer')">发起 USB prov.offer 配对请求</button>
      <button onclick="api('/api/sim_muse_qr')">进入 Muse (ota_0) RFC 8628 二维码授权</button>
      <button onclick="api('/api/sim_muse_auth_ok')">模拟浏览器完成 Muse 授权 (存 NVS)</button>
      <button onclick="api('/api/reset_launcher')">重启回 Launcher (factory)</button>
    </div>

    <h2>3. C 固件实时内部状态 (nf_proto / app_state)</h2>
    <pre id="telemetry">加载中...</pre>
  </div>

  <script>
    const img = document.getElementById('amoled');
    const tele = document.getElementById('telemetry');

    async function refresh() {
      img.src = '/frame.png?t=' + Date.now();
      const res = await fetch('/api/state');
      const data = await res.json();
      tele.textContent = JSON.stringify(data, null, 2);
    }

    async function api(path) {
      await fetch(path, { method: 'POST' });
      await refresh();
    }

    img.addEventListener('click', async (ev) => {
      const rect = img.getBoundingClientRect();
      const x = Math.round((ev.clientX - rect.left) * (466 / rect.width));
      const y = Math.round((ev.clientY - rect.top) * (466 / rect.height));
      await api(`/api/touch?x=${x}&y=${y}`);
    });

    setInterval(refresh, 400);
    refresh();
  </script>
</body>
</html>
"""


class SimEngine:
    def __init__(self) -> None:
        if not LIB_PATH.exists():
            subprocess.run(["make", "-C", str(SIM_DIR)], check=True)
        self.lib = ctypes.CDLL(str(LIB_PATH))
        self.lock = threading.Lock()

        self.lib.sim_init.argtypes = [ctypes.c_int]
        self.lib.sim_step_ms.argtypes = [ctypes.c_uint32]
        self.lib.sim_set_boot_button.argtypes = [ctypes.c_bool]
        self.lib.sim_trigger_single_click_boot.argtypes = []
        self.lib.sim_trigger_double_click_boot.argtypes = []
        self.lib.sim_trigger_pwr_click.argtypes = []
        self.lib.sim_touch_xy.argtypes = [ctypes.c_int, ctypes.c_int]
        self.lib.sim_inject_speech_partial.argtypes = [ctypes.c_char_p]
        self.lib.sim_inject_speech_final.argtypes = [ctypes.c_char_p, ctypes.c_bool]
        self.lib.sim_inject_tts_asset.argtypes = [ctypes.c_char_p, ctypes.c_uint32]
        self.lib.sim_inject_prov_line.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_size_t]
        self.lib.sim_inject_prov_line.restype = ctypes.c_int
        self.lib.sim_muse_complete_oauth.argtypes = []
        self.lib.sim_render_png.argtypes = [ctypes.c_char_p]
        self.lib.sim_render_png.restype = ctypes.c_bool
        self.lib.sim_get_state_json.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
        self.lib.sim_get_state_json.restype = ctypes.c_int

        self.lib.sim_init(1)  # Start in NevoFlux (ota_1) by default

    def render_png_bytes(self) -> bytes:
        with self.lock:
            self.lib.sim_step_ms(20)
            with tempfile.NamedTemporaryFile(suffix=".png", delete=False) as tmp:
                tmp_path = tmp.name
            try:
                self.lib.sim_render_png(tmp_path.encode("utf-8"))
                return pathlib.Path(tmp_path).read_bytes()
            finally:
                if os.path.exists(tmp_path):
                    os.unlink(tmp_path)

    def get_state(self) -> dict:
        with self.lock:
            buf = ctypes.create_string_buffer(2048)
            self.lib.sim_get_state_json(buf, 2048)
            return json.loads(buf.value.decode("utf-8"))


def run_server(port: int) -> None:
    engine = SimEngine()

    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt: str, *args: object) -> None:
            pass

        def do_GET(self) -> None:
            parsed = urlparse(self.path)
            if parsed.path == "/":
                body = HTML_PAGE.encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
                return

            if parsed.path == "/frame.png":
                png = engine.render_png_bytes()
                self.send_response(200)
                self.send_header("Content-Type", "image/png")
                self.send_header("Cache-Control", "no-store")
                self.send_header("Content-Length", str(len(png)))
                self.end_headers()
                self.wfile.write(png)
                return

            if parsed.path == "/api/state":
                body = json.dumps(engine.get_state()).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.end_headers()
                self.wfile.write(body)
                return

            self.send_error(404)

        def do_POST(self) -> None:
            parsed = urlparse(self.path)
            qs = parse_qs(parsed.query)
            with engine.lock:
                if parsed.path == "/api/touch":
                    x = int(qs.get("x", ["0"])[0])
                    y = int(qs.get("y", ["0"])[0])
                    engine.lib.sim_touch_xy(x, y)
                elif parsed.path == "/api/boot_down":
                    engine.lib.sim_set_boot_button(True)
                    engine.lib.sim_step_ms(350)
                elif parsed.path == "/api/boot_up":
                    engine.lib.sim_set_boot_button(False)
                    engine.lib.sim_step_ms(40)
                elif parsed.path == "/api/boot_click":
                    engine.lib.sim_trigger_single_click_boot()
                elif parsed.path == "/api/boot_double":
                    engine.lib.sim_trigger_double_click_boot()
                elif parsed.path == "/api/pwr_click":
                    engine.lib.sim_trigger_pwr_click()
                elif parsed.path == "/api/sim_partial":
                    engine.lib.sim_inject_speech_partial(
                        "帮我查一下私域知识库里关于语音吊坠的内容…".encode("utf-8")
                    )
                elif parsed.path == "/api/sim_final":
                    engine.lib.sim_inject_speech_final(
                        "帮我查一下私域知识库里关于语音吊坠的内容。".encode("utf-8"), True
                    )
                elif parsed.path == "/api/sim_tts_24k":
                    engine.lib.sim_inject_tts_asset(
                        "【Kokoro 24kHz 播报】已为您检索 GBrain：语音吊坠采用双密钥 AES-256-GCM 直连 Relay。".encode("utf-8"),
                        24000,
                    )
                elif parsed.path == "/api/sim_tts_48k":
                    engine.lib.sim_inject_tts_asset(
                        "【MOSS 48kHz 播报】已为您动态切换 ES8311 至 48kHz 采样率，无需软件重采样。".encode("utf-8"),
                        48000,
                    )
                elif parsed.path == "/api/sim_prov_offer":
                    resp = ctypes.create_string_buffer(256)
                    engine.lib.sim_inject_prov_line(
                        b'{"cmd":"prov.offer","device":"Sam-ThinkPad"}', resp, 256
                    )
                elif parsed.path == "/api/sim_muse_qr":
                    engine.lib.sim_init(2)
                elif parsed.path == "/api/sim_muse_auth_ok":
                    engine.lib.sim_muse_complete_oauth()
                elif parsed.path == "/api/reset_launcher":
                    engine.lib.sim_init(0)

            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(b'{"ok":true}')

    server = HTTPServer(("0.0.0.0", port), Handler)
    print(f"[OK] NevoFlux Pendant 466x466 Simulator listening on http://localhost:{port}")
    server.serve_forever()


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=8765)
    args = parser.parse_args()
    run_server(args.port)
