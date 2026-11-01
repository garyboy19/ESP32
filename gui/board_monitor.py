#!/usr/bin/env python3
"""ESP32 live telemetry dashboard (Tkinter + pyserial)."""

from __future__ import annotations

import json
import queue
import threading
import time
import tkinter as tk
from tkinter import messagebox, ttk

import serial
from serial.tools import list_ports


BG = "#0b1020"
PANEL = "#121a2d"
PANEL_2 = "#18233a"
TEXT = "#edf3ff"
MUTED = "#8b9ab5"
ACCENT = "#53d6b4"
BLUE = "#74a7ff"
ORANGE = "#ffbf69"


class BoardMonitor:
    def __init__(self, root: tk.Tk) -> None:
        self.root = root
        self.root.title("ESP32 · 实时开发板监控")
        self.root.geometry("1040x720")
        self.root.minsize(850, 620)
        self.root.configure(bg=BG)

        self.messages: queue.Queue[tuple[str, object]] = queue.Queue()
        self.stop_event = threading.Event()
        self.worker: threading.Thread | None = None
        self.port: serial.Serial | None = None
        self.connected = False
        self.last_data: dict[str, object] = {}
        self.heap_history: list[int] = []
        self.min_heap_history: list[int] = []
        self.last_seen: float | None = None
        # 串口异常断开后自动重连
        self.auto_reconnect = True
        self.reconnect_attempts = 0
        self.max_reconnect_attempts = 10
        # 堆泄漏判定阈值：历史最低堆相对窗口内峰值的下降比例
        self.leak_ratio_threshold = 0.10
        self.leak_detected = False
        self.closed = False
        # Wi-Fi 面板状态
        self.wifi_aps: list[dict[str, object]] = []
        self.wifi_selected: str | None = None
        self.wifi_status = "—"
        self.wifi_connected_ssid = ""
        self.wifi_ip = ""
        self.wifi_total = 0

        self._build_ui()
        self.refresh_ports()
        self.root.after(100, self._drain_messages)
        self.root.after(1000, self._update_link_age)
        self.root.protocol("WM_DELETE_WINDOW", self.close)

    def _build_ui(self) -> None:
        outer = tk.Frame(self.root, bg=BG, padx=24, pady=20)
        outer.pack(fill="both", expand=True)

        header = tk.Frame(outer, bg=BG)
        header.pack(fill="x", pady=(0, 18))
        title_box = tk.Frame(header, bg=BG)
        title_box.pack(side="left")
        tk.Label(title_box, text="ESP32 实时监控", bg=BG, fg=TEXT,
                 font=("Arial", 23, "bold")).pack(anchor="w")
        tk.Label(title_box, text="串口遥测 · 芯片状态 · 内存趋势", bg=BG,
                 fg=MUTED, font=("Arial", 10)).pack(anchor="w", pady=(4, 0))

        controls = tk.Frame(header, bg=BG)
        controls.pack(side="right", anchor="ne", pady=5)
        self.port_var = tk.StringVar()
        self.port_box = ttk.Combobox(controls, textvariable=self.port_var,
                                     width=24, state="readonly")
        self.port_box.pack(side="left", padx=(0, 8))
        self.refresh_button = tk.Button(controls, text="↻", command=self.refresh_ports,
                                        bg=PANEL_2, fg=TEXT, activebackground=PANEL,
                                        activeforeground=TEXT, relief="flat", padx=10,
                                        font=("Arial", 12))
        self.refresh_button.pack(side="left", padx=(0, 8))
        self.connect_button = tk.Button(controls, text="连接开发板", command=self.toggle_connection,
                                        bg=ACCENT, fg="#071610", activebackground="#76e8ca",
                                        relief="flat", padx=15, pady=8,
                                        font=("Arial", 10, "bold"))
        self.connect_button.pack(side="left")

        status_row = tk.Frame(outer, bg=BG)
        status_row.pack(fill="x", pady=(0, 16))
        self.status_dot = tk.Label(status_row, text="●", bg=BG, fg="#65738d",
                                   font=("Arial", 11))
        self.status_dot.pack(side="left")
        self.status_text = tk.Label(status_row, text="未连接", bg=BG, fg=MUTED,
                                    font=("Arial", 10))
        self.status_text.pack(side="left", padx=(6, 0))
        self.timestamp = tk.Label(status_row, text="等待串口数据", bg=BG, fg=MUTED,
                                  font=("Arial", 9))
        self.timestamp.pack(side="right")

        cards = tk.Frame(outer, bg=BG)
        cards.pack(fill="x")
        self.metric_labels: dict[str, tk.Label] = {}
        metrics = [
            ("uptime", "运行时间", "—", "自上次启动"),
            ("free_heap", "可用堆内存", "—", "实时剩余内存"),
            ("min_heap", "历史最低堆", "—", "运行期间最低值"),
            ("flash", "Flash 容量", "—", "芯片外部存储"),
        ]
        for index, (key, label, value, foot) in enumerate(metrics):
            card = tk.Frame(cards, bg=PANEL, padx=16, pady=14)
            card.grid(row=0, column=index, sticky="nsew", padx=(0 if index == 0 else 8, 0))
            cards.grid_columnconfigure(index, weight=1, uniform="metric")
            tk.Label(card, text=label, bg=PANEL, fg=MUTED,
                     font=("Arial", 9)).pack(anchor="w")
            value_label = tk.Label(card, text=value, bg=PANEL, fg=TEXT,
                                   font=("Arial", 22, "bold"))
            value_label.pack(anchor="w", pady=(8, 3))
            tk.Label(card, text=foot, bg=PANEL, fg=MUTED,
                     font=("Arial", 8)).pack(anchor="w")
            self.metric_labels[key] = value_label

        middle = tk.Frame(outer, bg=BG)
        middle.pack(fill="both", expand=True, pady=16)
        chart_panel = tk.Frame(middle, bg=PANEL, padx=16, pady=14)
        chart_panel.pack(side="left", fill="both", expand=True, padx=(0, 10))
        chart_header = tk.Frame(chart_panel, bg=PANEL)
        chart_header.pack(fill="x")
        tk.Label(chart_header, text="堆内存趋势", bg=PANEL, fg=TEXT,
                 font=("Arial", 12, "bold")).pack(side="left")
        tk.Label(chart_header, text="最近 60 个采样点", bg=PANEL, fg=MUTED,
                 font=("Arial", 8)).pack(side="right")
        self.chart = tk.Canvas(chart_panel, bg=PANEL, highlightthickness=0, height=250)
        self.chart.pack(fill="both", expand=True, pady=(12, 0))
        self.chart.bind("<Configure>", lambda _event: self._draw_chart())

        side = tk.Frame(middle, bg=PANEL, width=245, padx=16, pady=14)
        side.pack(side="right", fill="y")
        side.pack_propagate(False)
        tk.Label(side, text="板卡状态", bg=PANEL, fg=TEXT,
                 font=("Arial", 12, "bold")).pack(anchor="w", pady=(0, 12))
        self.info_labels: dict[str, tk.Label] = {}
        for key, label in [("chip", "芯片"), ("revision", "硅版本"), ("cores", "CPU 核心"),
                           ("wifi", "Wi‑Fi"), ("bt", "蓝牙"), ("boot", "BOOT 按键"),
                           ("led", "GPIO2 LED"), ("ap", "附近热点"), ("rssi", "最强信号"),
                           ("heap", "堆健康度")]:
            row = tk.Frame(side, bg=PANEL)
            row.pack(fill="x", pady=5)
            tk.Label(row, text=label, bg=PANEL, fg=MUTED,
                     font=("Arial", 9)).pack(side="left")
            value = tk.Label(row, text="—", bg=PANEL, fg=TEXT,
                             font=("Arial", 9, "bold"))
            value.pack(side="right")
            self.info_labels[key] = value

        # ---------------- Wi-Fi panel ----------------
        wifi_panel = tk.Frame(outer, bg=PANEL, padx=14, pady=12)
        wifi_panel.pack(fill="x", pady=(0, 14))

        wifi_head = tk.Frame(wifi_panel, bg=PANEL)
        wifi_head.pack(fill="x")
        tk.Label(wifi_head, text="Wi‑Fi 网络", bg=PANEL, fg=TEXT,
                 font=("Arial", 12, "bold")).pack(side="left")
        self.wifi_status_label = tk.Label(wifi_head, text="—", bg=PANEL, fg=MUTED,
                                          font=("Arial", 9))
        self.wifi_status_label.pack(side="right")

        wifi_body = tk.Frame(wifi_panel, bg=PANEL)
        wifi_body.pack(fill="x", pady=(10, 0))

        # list
        list_frame = tk.Frame(wifi_body, bg=PANEL)
        list_frame.pack(side="left", fill="both", expand=True, padx=(0, 12))
        list_scroll = tk.Scrollbar(list_frame, orient="vertical", width=12)
        list_scroll.pack(side="right", fill="y")
        self.wifi_list = tk.Listbox(list_frame, height=6, bg=PANEL_2, fg=TEXT,
                                    selectbackground="#2f4a7a", highlightthickness=0,
                                    font=("Arial", 9), activestyle="none",
                                    exportselection=False)
        self.wifi_list.pack(side="left", fill="both", expand=True)
        self.wifi_list.config(yscrollcommand=list_scroll.set)
        list_scroll.config(command=self.wifi_list.yview)
        self.wifi_list.bind("<<ListboxSelect>>", self._on_wifi_select)

        # controls
        ctrl = tk.Frame(wifi_body, bg=PANEL, width=250)
        ctrl.pack(side="right", fill="y")
        ctrl.pack_propagate(False)

        tk.Label(ctrl, text="密码", bg=PANEL, fg=MUTED,
                 font=("Arial", 9)).pack(anchor="w")
        self.wifi_password = tk.Entry(ctrl, show="•", bg=PANEL_2, fg=TEXT,
                                      insertbackground=TEXT, relief="flat",
                                      font=("Arial", 9))
        self.wifi_password.pack(fill="x", pady=(3, 8))

        btn_row = tk.Frame(ctrl, bg=PANEL)
        btn_row.pack(fill="x")
        tk.Button(btn_row, text="扫描", command=self.wifi_scan,
                  bg=PANEL_2, fg=TEXT, activebackground=PANEL,
                  relief="flat", padx=10, font=("Arial", 9)).pack(side="left")
        tk.Button(btn_row, text="连接", command=self.wifi_connect,
                  bg=ACCENT, fg="#071610", activebackground="#76e8ca",
                  relief="flat", padx=10, font=("Arial", 9, "bold")).pack(
                      side="left", padx=(8, 0))
        tk.Button(btn_row, text="断开", command=self.wifi_disconnect,
                  bg=PANEL_2, fg=TEXT, activebackground=PANEL,
                  relief="flat", padx=10, font=("Arial", 9)).pack(
                      side="left", padx=(8, 0))

        self.wifi_hint = tk.Label(ctrl, text="选择左侧网络后点连接", bg=PANEL, fg=MUTED,
                                  font=("Arial", 8), wraplength=235,
                                  justify="left")
        self.wifi_hint.pack(anchor="w", pady=(8, 0))

        footer = tk.Frame(outer, bg=BG)
        footer.pack(fill="x")
        tk.Label(footer, text="串口波特率 115200 · 遥测由板载固件每秒发送", bg=BG,
                 fg=MUTED, font=("Arial", 9)).pack(side="left")
        self.uptime_label = tk.Label(footer, text="", bg=BG, fg=MUTED,
                                     font=("Arial", 9))
        self.uptime_label.pack(side="right")

        style = ttk.Style()
        style.theme_use("clam")
        style.configure("TCombobox", fieldbackground=PANEL_2, background=PANEL_2,
                        foreground=TEXT, arrowcolor=TEXT, borderwidth=0, padding=7)
        style.map("TCombobox", fieldbackground=[("readonly", PANEL_2)],
                  foreground=[("readonly", TEXT)])

    def refresh_ports(self) -> None:
        ports = sorted((item.device for item in list_ports.comports()), key=str.lower)
        self.port_box["values"] = ports
        if self.port_var.get() not in ports:
            preferred = next((p for p in ports if "usbserial" in p.lower() or "usbmodem" in p.lower()),
                             ports[0] if ports else "")
            self.port_var.set(preferred)
        if not ports and not self.connected:
            self.status_text.config(text="未发现串口设备")

    def toggle_connection(self) -> None:
        if self.connected:
            self.disconnect()
            return
        self.auto_reconnect = True
        self.reconnect_attempts = 0
        self._open_port()

    def _open_port(self) -> bool:
        """打开串口并启动读线程，返回是否成功。"""
        device = self.port_var.get()
        if not device:
            self.refresh_ports()
            messagebox.showinfo("未选择串口", "请先连接 ESP32 USB 数据线并选择串口。")
            return False
        try:
            self.port = serial.Serial(device, 115200, timeout=0.25)
        except (serial.SerialException, OSError) as error:
            if self.reconnect_attempts == 0:
                messagebox.showerror("串口连接失败", str(error))
            self.status_text.config(text=f"串口连接失败：{error}")
            self.status_dot.config(fg="#ff7b83")
            return False
        self.connected = True
        self.reconnect_attempts = 0
        self.stop_event.clear()
        self.worker = threading.Thread(target=self._read_serial, daemon=True)
        self.worker.start()
        self.connect_button.config(text="断开连接", bg="#ff7b83", fg="#26080a")
        self.port_box.config(state="disabled")
        self.refresh_button.config(state="disabled")
        self.status_dot.config(fg=ORANGE)
        self.status_text.config(text=f"已连接 {device} · 等待遥测数据")
        # 拉一次 Wi-Fi 列表，填充面板
        try:
            self.port.write(b"@CMD LIST\r\n")
            self.port.flush()
        except (serial.SerialException, OSError):
            pass
        return True

    def _read_serial(self) -> None:
        assert self.port is not None
        while not self.stop_event.is_set():
            try:
                line = self.port.readline().decode("utf-8", errors="replace").strip()
            except (serial.SerialException, OSError) as error:
                self.messages.put(("error", str(error)))
                break
            if line.startswith("@DATA "):
                try:
                    data = json.loads(line[6:])
                    self.messages.put(("data", data))
                except json.JSONDecodeError:
                    continue
            elif line.startswith("@WIFI "):
                try:
                    data = json.loads(line[6:])
                    self.messages.put(("wifi", data))
                except json.JSONDecodeError:
                    continue
        # 读线程结束：若不是用户主动断开，则安排自动重连
        if not self.stop_event.is_set():
            self.messages.put(("reconnect", None))

    def _drain_messages(self) -> None:
        while True:
            try:
                kind, payload = self.messages.get_nowait()
            except queue.Empty:
                break
            if kind == "data":
                self._show_data(payload)  # type: ignore[arg-type]
            elif kind == "wifi":
                self._show_wifi(payload)  # type: ignore[arg-type]
            elif kind == "error":
                self.status_text.config(text=f"串口读取错误：{payload}")
                self.status_dot.config(fg="#ff7b83")
            elif kind == "reconnect":
                self._schedule_reconnect()
        self.root.after(100, self._drain_messages)

    def _show_data(self, data: dict[str, object]) -> None:
        self.last_data = data
        self.last_seen = time.time()
        self.status_dot.config(fg=ACCENT)
        self.status_text.config(text=f"实时接收中 · {self.port_var.get()}")
        self.timestamp.config(text=f"最后更新 {time.strftime('%H:%M:%S')}")

        uptime = int(data.get("uptime_s", 0))
        free_heap = int(data.get("free_heap", 0))
        min_heap = int(data.get("min_heap", 0))
        self.metric_labels["uptime"].config(text=self._format_uptime(uptime))
        self.metric_labels["free_heap"].config(text=self._format_bytes(free_heap))
        self.metric_labels["min_heap"].config(text=self._format_bytes(min_heap))
        self.metric_labels["flash"].config(text=f"{data.get('flash_mb', '—')} MB")
        self.heap_history.append(free_heap)
        self.heap_history = self.heap_history[-60:]
        self.min_heap_history.append(min_heap)
        self.min_heap_history = self.min_heap_history[-60:]
        self._update_leak_state()
        self._draw_chart()

        self.info_labels["chip"].config(text="ESP32")
        self.info_labels["revision"].config(text=f"v{data.get('revision', '—')}")
        self.info_labels["cores"].config(text=str(data.get("cores", "—")))
        self.info_labels["wifi"].config(text="可用" if data.get("wifi") else "不可用",
                                         fg=ACCENT if data.get("wifi") else "#ff7b83")
        self.info_labels["bt"].config(text="可用" if data.get("bt") else "不可用")
        boot = int(data.get("boot", 1))
        self.info_labels["boot"].config(text="按下" if boot == 0 else "松开",
                                         fg=ORANGE if boot == 0 else TEXT)
        self.info_labels["led"].config(text="亮" if data.get("led") else "灭",
                                        fg=ACCENT if data.get("led") else MUTED)
        self.info_labels["ap"].config(text=f"{data.get('ap_count', 0)} 个")
        rssi = int(data.get("best_rssi", -127))
        self.info_labels["rssi"].config(text=f"{rssi} dBm" if rssi > -127 else "—")
        self.info_labels["heap"].config(
            text="疑似泄漏" if self.leak_detected else "正常",
            fg="#ff7b83" if self.leak_detected else ACCENT)

    # ---------------- Wi-Fi panel logic ----------------
    def _send_cmd(self, command: str) -> bool:
        """Send a @CMD line to the board. Returns False if not connected."""
        if not self.connected or self.port is None:
            messagebox.showinfo("未连接", "请先连接开发板。")
            return False
        try:
            self.port.write(f"@CMD {command}\r\n".encode("utf-8"))
            self.port.flush()
        except (serial.SerialException, OSError) as error:
            self.status_text.config(text=f"命令发送失败：{error}")
            return False
        return True

    def wifi_scan(self) -> None:
        if self._send_cmd("SCAN"):
            self.wifi_status = "扫描中…"
            self.wifi_status_label.config(text=self.wifi_status, fg=ORANGE)

    def wifi_connect(self) -> None:
        if not self.wifi_selected:
            messagebox.showinfo("未选择网络", "请先在列表中选择一个 Wi‑Fi 网络。")
            return
        password = self.wifi_password.get()
        # SSID 可能含空格，这里用引号包裹并转义
        ssid = self.wifi_selected.replace("\\", "\\\\").replace('"', '\\"')
        command = f'CONNECT "{ssid}"'
        if password:
            password = password.replace("\\", "\\\\").replace('"', '\\"')
            command += f' "{password}"'
        if self._send_cmd(command):
            self.wifi_status = f"正在连接 {self.wifi_selected}…"
            self.wifi_status_label.config(text=self.wifi_status, fg=ORANGE)

    def wifi_disconnect(self) -> None:
        if self._send_cmd("DISCONNECT"):
            self.wifi_status = "已断开"
            self.wifi_status_label.config(text=self.wifi_status, fg=MUTED)

    def _on_wifi_select(self, _event: object) -> None:
        selection = self.wifi_list.curselection()
        if not selection:
            return
        index = selection[0]
        if index >= len(self.wifi_aps):
            return
        self.wifi_selected = str(self.wifi_aps[index].get("ssid", ""))
        self.wifi_hint.config(text=f"已选择：{self.wifi_selected}")

    def _show_wifi(self, data: dict[str, object]) -> None:
        kind = str(data.get("type", ""))
        if kind == "list":
            self.wifi_aps = list(data.get("aps", []) or [])
            self.wifi_total = int(data.get("total", 0) or 0)
            self.wifi_connected_ssid = str(data.get("ssid", "") or "")
            self.wifi_ip = str(data.get("ip", "") or "")
            self._refresh_wifi_list()
            if self.wifi_connected_ssid:
                self.wifi_status = f"已连接 {self.wifi_connected_ssid}"
                if self.wifi_ip:
                    self.wifi_status += f" · {self.wifi_ip}"
                self.wifi_status_label.config(text=self.wifi_status, fg=ACCENT)
            else:
                self.wifi_status = f"发现 {self.wifi_total} 个网络"
                self.wifi_status_label.config(text=self.wifi_status, fg=MUTED)
        elif kind == "scan":
            if int(data.get("err", 0) or 0) == 0:
                self.wifi_status = "扫描中…"
                self.wifi_status_label.config(text=self.wifi_status, fg=ORANGE)
        elif kind == "connecting":
            self.wifi_status = f"正在连接 {data.get('ssid', '')}…"
            self.wifi_status_label.config(text=self.wifi_status, fg=ORANGE)
            # 连接可能成功也可能失败，稍后拉一次列表看最终状态
            self.root.after(4000, self._request_wifi_list)
        elif kind == "disconnected":
            self.wifi_status = "已断开"
            self.wifi_status_label.config(text=self.wifi_status, fg=MUTED)
            self.wifi_connected_ssid = ""
            self.wifi_ip = ""
        elif kind == "error":
            self.wifi_status = f"错误：{data.get('msg', '')}"
            self.wifi_status_label.config(text=self.wifi_status, fg="#ff7b83")

    def _request_wifi_list(self) -> None:
        if self.connected and self.port is not None:
            try:
                self.port.write(b"@CMD LIST\r\n")
                self.port.flush()
            except (serial.SerialException, OSError):
                pass

    def _refresh_wifi_list(self) -> None:
        self.wifi_list.delete(0, "end")
        for ap in self.wifi_aps:
            ssid = str(ap.get("ssid", ""))
            rssi = int(ap.get("rssi", -127) or -127)
            sec = str(ap.get("sec", ""))
            ch = int(ap.get("ch", 0) or 0)
            mark = "*" if ssid and ssid == self.wifi_connected_ssid else " "
            self.wifi_list.insert("end", f"{mark} {ssid:<22} {rssi:>5}dBm  ch{ch:<3} {sec}")

    def _schedule_reconnect(self) -> None:
        """串口意外断开后按退避策略自动重连。"""
        if self.reconnect_attempts >= self.max_reconnect_attempts:
            self.status_text.config(text="串口已断开 · 自动重连次数用尽，请手动重连")
            self.status_dot.config(fg="#ff7b83")
            return
        self.reconnect_attempts += 1
        delay = min(1.0 * self.reconnect_attempts, 5.0)
        self.connected = False
        self.last_seen = None
        self.status_text.config(
            text=f"串口断开 · {delay:.0f}s 后第 {self.reconnect_attempts} 次重连")
        self.status_dot.config(fg=ORANGE)
        self.root.after(int(delay * 1000), self._attempt_reconnect)

    def _attempt_reconnect(self) -> None:
        if self.stop_event.is_set() or self.closed:
            return
        if self._open_port():
            self.status_text.config(text=f"重连成功 {self.port_var.get()} · 等待遥测数据")
        else:
            self._schedule_reconnect()

    def _update_leak_state(self) -> None:
        """依据最低堆相对窗口峰值的下滑判定疑似堆泄漏。"""
        if len(self.min_heap_history) < 10:
            self.leak_detected = False
            return
        peak = max(self.heap_history)
        floor = min(self.min_heap_history)
        if peak <= 0:
            self.leak_detected = False
            return
        drop = (peak - floor) / peak
        self.leak_detected = drop >= self.leak_ratio_threshold

    def _draw_chart(self) -> None:
        canvas = self.chart
        canvas.delete("all")
        width = max(canvas.winfo_width(), 100)
        height = max(canvas.winfo_height(), 100)
        left, right, top, bottom = 50, width - 12, 14, height - 28
        # Y 轴动态定标：覆盖两条曲线，并留出上下各 15% 余量
        series = [v for v in self.heap_history + self.min_heap_history if v > 0]
        if series:
            hi = max(series)
            lo = min(series)
            span = max(hi - lo, hi * 0.05, 1024)
            max_bytes = hi + span * 0.15
            min_bytes = max(0, lo - span * 0.15)
        else:
            max_bytes = 320 * 1024
            min_bytes = 0
        if max_bytes - min_bytes < 1024:
            max_bytes = min_bytes + 1024
        scale = max_bytes - min_bytes

        for fraction in (0, .25, .5, .75, 1):
            y = bottom - fraction * (bottom - top)
            canvas.create_line(left, y, right, y, fill="#29344b")
            value = min_bytes + fraction * scale
            canvas.create_text(left - 8, y, text=f"{value / 1024:.0f}",
                               fill=MUTED, anchor="e", font=("Arial", 8))
        canvas.create_text(10, top, text="KB", fill=MUTED, anchor="nw", font=("Arial", 8))
        if len(self.heap_history) < 2:
            canvas.create_text((left + right) / 2, (top + bottom) / 2,
                               text="连接开发板后显示内存变化", fill=MUTED,
                               font=("Arial", 10))
            return

        def to_points(values: list[int]) -> list[float]:
            points: list[float] = []
            for index, value in enumerate(values):
                x = left + index * (right - left) / max(59, len(values) - 1)
                clipped = min(max(value, min_bytes), max_bytes)
                y = bottom - (clipped - min_bytes) / scale * (bottom - top)
                points.extend((x, y))
            return points

        min_points = to_points(self.min_heap_history)
        if len(min_points) >= 4:
            canvas.create_line(*min_points, fill=ORANGE, width=1, smooth=True)
        points = to_points(self.heap_history)
        canvas.create_line(*points, fill=ACCENT, width=2, smooth=True)
        canvas.create_oval(points[-2] - 3, points[-1] - 3, points[-2] + 3,
                           points[-1] + 3, fill=ACCENT, outline="")

        legend_y = top + 4
        canvas.create_text(right, legend_y, text="— 可用堆  — 最低堆",
                           fill=MUTED, anchor="ne", font=("Arial", 8))

    def _update_link_age(self) -> None:
        if self.connected and self.last_seen is not None:
            age = int(time.time() - self.last_seen)
            if age > 3:
                self.status_dot.config(fg=ORANGE)
                self.status_text.config(text=f"串口已连接 · 遥测中断 {age} 秒")
        self.root.after(1000, self._update_link_age)

    @staticmethod
    def _format_bytes(value: int) -> str:
        return f"{value / 1024:.0f} KB"

    @staticmethod
    def _format_uptime(seconds: int) -> str:
        days, remainder = divmod(seconds, 86400)
        hours, remainder = divmod(remainder, 3600)
        minutes, secs = divmod(remainder, 60)
        return f"{days}天 {hours:02}:{minutes:02}:{secs:02}" if days else f"{hours:02}:{minutes:02}:{secs:02}"

    def disconnect(self) -> None:
        self.stop_event.set()
        self.auto_reconnect = False
        self.reconnect_attempts = self.max_reconnect_attempts
        if self.port:
            try:
                self.port.close()
            except serial.SerialException:
                pass
        self.port = None
        self.connected = False
        self.last_seen = None
        self.heap_history.clear()
        self.min_heap_history.clear()
        self.leak_detected = False
        self.connect_button.config(text="连接开发板", bg=ACCENT, fg="#071610")
        self.port_box.config(state="readonly")
        self.refresh_button.config(state="normal")
        self.status_dot.config(fg="#65738d")
        self.status_text.config(text="未连接")
        self.timestamp.config(text="等待串口数据")
        self.info_labels["heap"].config(text="—", fg=TEXT)
        self.wifi_aps.clear()
        self.wifi_selected = None
        self.wifi_status = "—"
        self.wifi_status_label.config(text="—", fg=MUTED)
        self.wifi_hint.config(text="选择左侧网络后点连接")
        self._refresh_wifi_list()
        self._draw_chart()

    def close(self) -> None:
        self.closed = True
        self.disconnect()
        self.root.destroy()


def main() -> None:
    root = tk.Tk()
    BoardMonitor(root)
    root.mainloop()


if __name__ == "__main__":
    main()
