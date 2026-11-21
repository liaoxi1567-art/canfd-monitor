#!/usr/bin/env python3
"""Local CAN FD simulator: HTTP transport only; rules live in the shared C services.

No remote device is accessed. Imported logs and recorded samples are bounded,
in-memory simulation data and disappear when the simulator is reset.
"""
import ctypes
import base64
import binascii
import json
import math
import os
import sys
import secrets
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit, quote

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0,HERE)
from database_import import parse_database, DatabaseImportError
from send_plan_format import PlanStore, PlanFormatError, decode_plan, from_flat, to_flat
FW = os.path.abspath(os.path.join(HERE, "..", ".."))
LIBNAME = "libcanfdsim.dylib" if sys.platform == "darwin" else "libcanfdsim.so"
PORT = int(os.environ.get("PORT", "8765"))
# Millisecond send intervals need a millisecond simulation clock. Board timing
# remains owned by the firmware and is not inferred from this host model.
TICK_MS = 1
MAX_REQUEST = 3 * 1024 * 1024
MAX_LOG_TEXT = 131072
lib = ctypes.CDLL(os.path.join(FW, "build", "host", LIBNAME))


def bind(name, args=(), result=None):
    fn = getattr(lib, name)
    fn.argtypes, fn.restype = list(args), result
    return fn


I, U, P, B = ctypes.c_int, ctypes.c_uint32, ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint8)
bind("sim_reset")
bind("sim_tick", [I])
bind("sim_key", [I, I])
bind("sim_touch", [I, I])
bind("sim_local_backspace", [I,U], I)
bind("sim_local_keyboard_replace", [U,P], I)
bind("sim_env", [P, I, I], I)
bind("sim_host", [P, I, I], I)
bind("sim_snapshot", [P, I], I)
bind("sim_tx_entry", [I, I, U, I, I, B, U, U], I)
bind("sim_tx_result_text", [I], P)
bind("sim_tx_edit", [I, I], I)
bind("sim_filter", [I, I, U, U, I, I], I)
bind("sim_ui_action", [I, I, U, I], I)
bind("sim_rec_action", [I], I)
class SequenceEntry(ctypes.Structure):
    _fields_ = [("enabled",ctypes.c_bool),("repeat_count",U),("frame_gap_ms",U),("cycle_ms",U)]
class TxUpdate(ctypes.Structure):
    _fields_ = [("id",U),("revision",U),("period_ms",U),("index",I),("flags",I),("len",I),("data",ctypes.c_uint8 * 64)]
class DatabaseFrame(ctypes.Structure):
    _fields_ = [("id",U),("flags",U),("length",U),("cycle_ms",U),("name",ctypes.c_char * 64)]
PlanWire = U * 259
class PlanFile(ctypes.Structure):
    _fields_ = [("name",ctypes.c_char * 48),("project",ctypes.c_char * 48),("plan",PlanWire),("token",U)]
class PlanIO(ctypes.Structure):
    _fields_ = [("kind",U),("id",U),("name",ctypes.c_char * 48),("plan",PlanWire)]
assert ctypes.sizeof(PlanFile) == 1136 and ctypes.sizeof(PlanIO) == 1092
bind("sim_plan_open", [I,I], I)
bind("sim_plan_refresh", [], I)
bind("sim_plan_io_peek", [ctypes.POINTER(PlanIO)], I)
bind("sim_plan_io_complete", [U,I,P], I)
bind("sim_plan_catalog_replace", [ctypes.POINTER(PlanFile),U], I)
bind("sim_plan_result_text", [I], P)
bind("sim_sequence", [I,I,ctypes.POINTER(SequenceEntry),U,U], I)
bind("sim_sequence_text", [I], P)
bind("sim_tx_control", [I,I,I], I)
bind("sim_tx_select", [I,I], I)
bind("sim_tx_scope_select", [U], I)
bind("sim_tx_target_select", [U], I)
bind("sim_tx_channel_send", [I,I], I)
bind("sim_scope_control", [I,U,U], I)
bind("sim_scope_text", [I], P)
bind("sim_local_open", [I,I,I,U], I)
bind("sim_detail_to_tx", [I,I], I)
bind("sim_database_store", [P,ctypes.POINTER(DatabaseFrame),U], I)
bind("sim_local_result_text", [I], P)
bind("sim_sequence_defaults_all", [U,U], I)
bind("sim_tx_batch", [I,ctypes.POINTER(TxUpdate),I], I)
bind("sim_record_name", [P], I)
bind("sim_log_rename", [I,U,P], I)
bind("sim_log_name", [I,U,P,U], I)
bind("sim_log_import", [P, P, U, ctypes.POINTER(U), ctypes.POINTER(U)], I)
bind("sim_log_export", [I, P, U], I)
bind("sim_log_error_text", [I], P)

lock = threading.Lock()
state = {"speed": 1.0, "paused": False, "epoch": 0, "generation": 0}
databases = {}
plan_store = PlanStore(os.environ.get("CANFD_SIM_PLAN_DIR",os.path.join(FW,"sim","sdcard","send_plans")),defer_errors=True)
snap_buf = ctypes.create_string_buffer(512 * 1024)
export_buf = ctypes.create_string_buffer(MAX_LOG_TEXT)
lib.sim_reset()


class InputError(ValueError):
    pass


def integer(req, key, low, high, default=None):
    value = req.get(key, default)
    if isinstance(value, bool) or not isinstance(value, int) or not low <= value <= high:
        raise InputError(f"{key} 必须是 {low} 到 {high} 的整数")
    return value


def hex_id(req, key):
    value = req.get(key, "0")
    if not isinstance(value, str) or not value or len(value) > 10:
        raise InputError(f"{key} 必须是十六进制 ID")
    text = value[2:] if value.lower().startswith("0x") else value
    if not text or any(c not in "0123456789abcdefABCDEF" for c in text):
        raise InputError(f"{key} 包含非十六进制字符")
    number = int(text, 16)
    if number > 0x1FFFFFFF:
        raise InputError(f"{key} 超过 29 位范围")
    return number


def log_error(rc):
    names = {
        -1: "日志参数无效", -2: "日志超过帧数或文件大小限制", -3: "日志格式错误，仅接受 candump -L 文本",
        -4: "日志时间戳倒退，请先按时间排序", -5: "日志样例槽已满，请重置模拟器后重试",
        -6: "没有可导出的真实样例", -7: "导出缓冲容量不足", -8: "SD 卡正在使用或尚未就绪",
        -9: "已存在同名文件，请换一个名称", -10: "内置演示或受保护文件不能改名",
        -11: "文件已变化，请重新选择", -12: "名称含非法字符、路径或无效 UTF-8",
        -13: "日志文件名应以 .log 或 .txt 结尾",
    }
    return names.get(rc, "日志操作失败")


def install_local_database(parsed):
    """Copy parsed frame definitions into the device's simulated SD catalogue."""
    frames = parsed["frames"]
    if not 1 <= len(frames) <= 256:
        raise InputError("本地模拟 SD 每份数据库支持 1～256 帧；未放入文件，也未截断")
    rows = (DatabaseFrame * len(frames))()
    for n,frame in enumerate(frames):
        name = frame["name"].encode()
        if len(name)>63:
            raise InputError("本地数据库帧名称最多63个 UTF-8 字节，未截断名称")
        rows[n].id,rows[n].length,rows[n].cycle_ms = frame["id"],frame["length"],frame["cycle_ms"]
        rows[n].flags = int(frame["extended"]) | (2 if frame["fd"] else 0) | (4 if frame["brs"] else 0)
        rows[n].name = name
    rc = lib.sim_database_store(parsed["name"].encode(),rows,len(rows))
    if rc<0: raise InputError(lib.sim_local_result_text(rc).decode())
    return rc


def load_builtin_databases():
    for fmt in ("dbc","dbf"):
        try:
            with open(os.path.join(HERE,"examples",f"review.{fmt}"),"rb") as source:
                parsed = parse_database(f"review.{fmt}",source.read(),"auto")
            install_local_database(parsed)
        except (OSError,ValueError) as error:
            print(f"模拟 SD 数据库样例未载入：{error}",file=sys.stderr)


def refresh_plans():
    files, errors = plan_store.scan()
    rows = (PlanFile * len(files))()
    for row, file in zip(rows, files):
        row.name = file["name"].encode()
        row.project = file.get("project",file["plan"].get("project","未分组")).encode()
        row.plan[:] = to_flat(file["plan"])
    rc = lib.sim_plan_catalog_replace(rows,len(rows))
    if rc: raise PlanFormatError(lib.sim_plan_result_text(rc).decode())
    return files, errors


def service_plan_io():
    """Fulfil an immutable request from the device UI under the runtime lock."""
    request = PlanIO()
    if not lib.sim_plan_io_peek(ctypes.byref(request)): return
    try:
        if request.kind == 1:
            plan = from_flat(request.plan)
            plan["project"] = "本机保存"
            plan_store.save(request.name.decode(),plan)
        files, errors = refresh_plans()
        message = "方案已保存到模拟SD" if request.kind == 1 else f"已刷新 {len(files)} 个方案"
        if errors: message += f"；{len(errors)}个无效文件未载入：{errors[0]}"
        rc = 0
    except (PlanFormatError,OSError,UnicodeError) as error:
        rc, message = -47, str(error)
    lib.sim_plan_io_complete(request.id,rc,message.encode()[:180].decode("utf-8","ignore").encode())


def ticker():
    last, carry, epoch = time.monotonic(), 0.0, state["epoch"]
    while True:
        time.sleep(0.004)
        now = time.monotonic()
        dt, last = (now - last) * 1000.0, now
        with lock:
            if epoch != state["epoch"]:
                carry, epoch = 0.0, state["epoch"]
                continue
            if state["paused"]:
                carry = 0.0
                continue
            carry += dt * state["speed"]
            steps = min(int(carry // TICK_MS), 50 * max(1, int(state["speed"])))
            carry = min(carry - steps * TICK_MS, TICK_MS)
            for _ in range(steps):
                lib.sim_tick(TICK_MS)
            service_plan_io()


def snapshot():
    with lock:
        service_plan_io()
        n = lib.sim_snapshot(snap_buf, len(snap_buf))
        if n < 0:
            raise RuntimeError("snapshot overflow")
        body = snap_buf.raw[:n]
        sim = json.dumps({"speed": state["speed"], "paused": state["paused"], "generation":state["generation"]}).encode()
        return b'{"sim":' + sim + b',' + body[1:]


def execute(path, req):
    """Called under the C runtime lock. Return the status code and JSON object."""
    if path in ("/api/touch", "/api/local/input", "/api/local/backspace", "/api/ui", "/api/tx-entry", "/api/tx/edit", "/api/tx/channel", "/api/tx/target", "/api/tx/scope", "/api/tx/scope/control", "/api/plan/open", "/api/plan/import", "/api/plan/refresh", "/api/local", "/api/local/detail-copy", "/api/record", "/api/tx/select", "/api/sequence", "/api/tx/control", "/api/record/name", "/api/log/rename", "/api/database/apply") and "generation" in req:
        if integer(req, "generation", 0, 0xFFFFFFFF) != state["generation"]:
            return 409, {"rc":-7,"error":"模拟器已重置，请刷新后再操作"}
    if path == "/api/key":
        k = integer(req, "k", 0, 7)
        if not isinstance(req.get("down"), bool):
            raise InputError("down 必须为布尔值")
        lib.sim_key(k, int(req["down"]))
    elif path == "/api/touch":
        kind, row = integer(req, "kind", 0, 2), integer(req, "a", 0, 65535, 0)
        if "keyboard_token" in req:
            token = integer(req, "keyboard_token", 1, 0xFFFFFFFF)
            if "generation" not in req: raise InputError("键盘操作须包含模拟器代次")
            n = lib.sim_snapshot(snap_buf, len(snap_buf))
            if n < 0: return 409, {"error":"无法核对当前键盘"}
            keyboard = json.loads(snap_buf.raw[:n])["dev"]["ui"].get("keyboard", {})
            if keyboard.get("token") != token:
                return 409, {"error":"输入草稿已变化，请重新操作"}
        if "plan_token" in req or "plan_name" in req:
            token = integer(req, "plan_token", 1, 0xFFFFFFFF)
            name = req.get("plan_name")
            if kind != 0 or not isinstance(name, str) or "generation" not in req:
                raise InputError("发送文件选择须包含名称、版本和模拟器代次")
            n = lib.sim_snapshot(snap_buf, len(snap_buf))
            if n < 0: return 409, {"error":"无法核对发送文件，请重新打开列表"}
            current = json.loads(snap_buf.raw[:n])
            file = next((file for file in current["work"]["send_plans"]["files"]
                         if file["index"] == row), None)
            if current["dev"]["ui"]["page"] != "send_files" or not file or file["token"] != token or file["name"] != name:
                return 409, {"error":"发送文件列表已变化，请重新选择"}
        lib.sim_touch(kind, row)
    elif path == "/api/local/input":
        action, value = req.get("action"), req.get("text")
        token = integer(req, "token", 1, 0xFFFFFFFF)
        if action not in ("append", "replace") or not isinstance(value, str) or "\0" in value or len(value.encode("utf-8")) > 192:
            raise InputError("键盘输入格式无效或超过容量")
        if "generation" not in req: raise InputError("键盘输入须包含模拟器代次")
        if action == "append":
            n = lib.sim_snapshot(snap_buf, len(snap_buf))
            if n < 0: return 409, {"error":"无法读取当前键盘"}
            keyboard = json.loads(snap_buf.raw[:n])["dev"]["ui"].get("keyboard", {})
            if keyboard.get("token") != token:
                return 409, {"error":"输入草稿已变化，请重新操作"}
            value = keyboard.get("value", "") + value
        rc = lib.sim_local_keyboard_replace(token, value.encode("utf-8"))
        if rc: return 409, {"rc":rc,"error":lib.sim_local_result_text(rc).decode()}
    elif path == "/api/local/backspace":
        action=req.get("action")
        if action not in ("begin","end"): raise InputError("退格操作无效")
        rc=lib.sim_local_backspace(0 if action=="begin" else 1,integer(req,"token",1,0xFFFFFFFF))
        if rc: return 409,{"rc":rc,"error":lib.sim_local_result_text(rc).decode()}
    elif path in ("/api/env", "/api/host"):
        cmd = req.get("cmd")
        if not isinstance(cmd, str) or not cmd or len(cmd) > 40:
            raise InputError("cmd 无效")
        a, b = integer(req, "a", 0, 10000000, 0), integer(req, "b", 0, 10000000, 0)
        if path == "/api/env":
            # Bound values before narrowing to uint16/uint8 fields in the board simulator.
            ranges = {"batt":(2500,4500), "chip":(86,89), "sd_werr":(0,2)}
            bus_ranges = {"bus_conn":(0,1), "bus_nbr":(10000,1000000), "bus_dbr":(50000,5000000),
                          "bus_fd":(0,1), "bus_nodes":(0,2), "bus_fps":(0,10000),
                          "bus_err":(0,60000), "bus_overload":(0,100000)}
            toggles = {"ext","sd_wp","sd_unsupported","sd_frag","sd_busy","usb","host","screen"}
            if cmd in ranges and not ranges[cmd][0] <= a <= ranges[cmd][1]:
                raise InputError("环境数值超出范围")
            if cmd in toggles and a not in (0,1):
                raise InputError("环境开关必须为 0 或 1")
            if cmd in bus_ranges and (a not in (0,1) or not bus_ranges[cmd][0] <= b <= bus_ranges[cmd][1]):
                raise InputError("总线环境数值超出范围")
        rc = getattr(lib, "sim_env" if path == "/api/env" else "sim_host")(cmd.encode(), a, b)
        if rc:
            message = "请先开机、插上 USB 并连接上位机" if path == "/api/host" and rc == -2 else "命令或通道无效"
            return 409, {"rc": rc, "error": message}
    elif path == "/api/tx-entry":
        ch, idx = integer(req,"ch",0,1), integer(req,"index",0,3)
        ident, flags, length = hex_id(req,"id"), integer(req,"flags",0,255), integer(req,"len",0,64)
        period = integer(req,"period",0,65535)
        raw = req.get("data", "")
        if not isinstance(raw,str) or len(raw)>192:
            raise InputError("报文数据格式无效")
        try:
            data = bytes.fromhex(raw)
        except ValueError as exc:
            raise InputError("数据必须是完整的十六进制字节") from exc
        if len(data) != (0 if flags & 16 else length):
            raise InputError("数据字节数与长度不一致；RTR 不携带数据")
        payload = (ctypes.c_uint8 * 64)(*data)
        revision = integer(req,"revision",0,0xFFFFFFFF)
        rc = lib.sim_tx_entry(ch,idx,ident,flags,length,payload,period,revision)
        if rc:
            return 422, {"rc":rc,"error":lib.sim_tx_result_text(rc).decode()}
    elif path == "/api/tx/edit":
        rc = lib.sim_tx_edit(integer(req,"ch",0,1),integer(req,"index",0,3))
        if rc:
            return 409, {"rc":rc,"error":lib.sim_tx_result_text(rc).decode()}
    elif path == "/api/filter":
        rc = lib.sim_filter(integer(req,"channel",0,2),integer(req,"type",0,3),hex_id(req,"id"),hex_id(req,"mask"),integer(req,"direction",0,2),integer(req,"format",0,2))
        if rc:
            return 422, {"rc":rc,"error":lib.sim_tx_result_text(rc).decode()}
    elif path == "/api/ui":
        # Select a view, never perform its operation or discard an edit draft.
        # Parse all consumed parameters before entering the shared C UI.
        action = req.get("action")
        pages = {"home":0, "live":1, "tx":2, "rec":3,
                 "diag":5, "files":6, "chset":7, "sysset":8,
                 "send":18, "send_files":20, "keysim":22}
        actions = {"navigate":0, "live_view":1, "live_pause":2, "live_detail":3, "filter_open":4, "live_channel":5, "live_scroll":6}
        if not isinstance(action, str) or action not in actions:
            raise InputError("界面操作无效")
        value, sequence, use_sequence = 0, 0, 0
        if action == "navigate":
            page = req.get("page")
            if not isinstance(page, str) or page not in pages:
                raise InputError("页面无效")
            value = pages[page]
        elif action == "live_view":
            value = integer(req, "view", 0, 1)
        elif action == "live_channel":
            value = integer(req,"channel",1,2)
        elif action == "live_scroll":
            value = integer(req,"rows",-128,128)
            if "channel" in req or "view" in req:
                channel, view = integer(req,"channel",0,2), integer(req,"view",0,1)
                n = lib.sim_snapshot(snap_buf, len(snap_buf))
                if n < 0: return 409, {"error":"无法核对报文列表"}
                ui = json.loads(snap_buf.raw[:n])["dev"]["ui"]
                live = ui.get("live", {})
                if ui["page"] != "live" or live.get("channel") != channel or live.get("view") != ("byid" if view else "scroll"):
                    return 409, {"error":"报文视图已切换，请重新滑动"}
        elif action == "live_pause":
            if not isinstance(req.get("paused"), bool):
                raise InputError("paused 必须为布尔值")
            value = int(req["paused"])
        elif action == "live_detail":
            if "seq" in req:
                sequence = integer(req, "seq", 0, 0xFFFFFFFF)
                use_sequence = 1
            else:
                value = integer(req, "index", 0, 65535)
        if "generation" in req:
            generation = integer(req, "generation", 0, 0xFFFFFFFF)
            if generation != state["generation"]:
                return 409, {"rc":-7, "error":"模拟器已重置，请刷新后再操作"}
        rc = lib.sim_ui_action(actions[action], value, sequence, use_sequence)
        if rc:
            messages = {-1:"请先开机并唤醒屏幕", -2:"请先解除按键锁",
                        -3:"请先处理设备屏幕上的确认提示", -4:"请先保存或取消当前草稿",
                        -5:"此操作仅在实时显示页可用", -6:"报文已移出显示缓存，或选择超出范围"}
            return 409, {"rc":rc, "error":messages.get(rc, "界面操作未完成")}
    elif path == "/api/record":
        actions = {"start":0, "stop":1, "mark":2}
        action = req.get("action")
        if not isinstance(action,str) or action not in actions:
            raise InputError("录制操作必须是 start、stop 或 mark")
        rc = lib.sim_rec_action(actions[action])
        if rc:
            messages = {-1:"请先开机并唤醒屏幕",-2:"请先解除按键锁",-3:"请先处理屏幕确认",-4:"请先保存或取消当前草稿",-8:"当前录制状态不允许此操作，请等待文件关闭后再开始",-9:"SD 卡未就绪或不可写",-10:"模拟日志样例容量已满，请先导出已保留样例，再重置模拟；这不是实际 SD 卡容量"}
            return 409,{"rc":rc,"error":messages.get(rc,"录制请求未执行")}
    elif path == "/api/local/detail-copy":
        rc = lib.sim_detail_to_tx(integer(req,"ch",0,1),integer(req,"index",0,3))
        if rc: return 409,{"rc":rc,"error":lib.sim_local_result_text(rc).decode()}
    elif path == "/api/local":
        kind = integer(req,"kind",0,5)
        ch = integer(req,"ch",0,1,0)
        index = integer(req,"index",0,23 if kind==4 else 3,0)
        token = integer(req,"token",1,0xFFFFFFFF) if kind==4 else 0
        rc = lib.sim_local_open(kind,ch,index,token)
        if rc: return 409,{"rc":rc,"error":lib.sim_local_result_text(rc).decode()}
    elif path == "/api/sequence":
        actions = {"configure":0,"defaults":1,"start":2,"stop":3,"defaults_all":4}
        action = req.get("action")
        if not isinstance(action,str) or action not in actions: raise InputError("循环操作无效")
        if action == "defaults_all":
            revisions = req.get("revisions")
            if not isinstance(revisions,list) or len(revisions)!=2 or not all(type(r) is int and 0<=r<=0xFFFFFFFF for r in revisions): raise InputError("两路配置版本无效")
            rc = lib.sim_sequence_defaults_all(*revisions)
            if rc: return 409,{"rc":rc,"error":lib.sim_sequence_text(rc).decode()}
            return 200,{"rc":0}
        ch = integer(req,"ch",0,1)
        revision = integer(req,"revision",0,0xFFFFFFFF)
        entries, gap = None, 0
        if action == "configure":
            rows = req.get("entries")
            if not isinstance(rows,list) or len(rows) != 4 or not all(isinstance(r,dict) for r in rows):
                raise InputError("循环配置必须包含本路4条发送槽")
            entries = (SequenceEntry * 4)()
            for n,row in enumerate(rows):
                if not isinstance(row.get("enabled"),bool): raise InputError("enabled 必须为布尔值")
                entries[n] = SequenceEntry(row["enabled"],integer(row,"repeat_count",1,10000),integer(row,"frame_gap_ms",0,3600000),integer(row,"cycle_ms",1,3600000))
            gap = integer(req,"id_gap_ms",0,3600000)
        rc = lib.sim_sequence(actions[action],ch,entries,gap,revision)
        if rc < 0: return 409,{"rc":rc,"error":lib.sim_sequence_text(rc).decode()}
        return 200,{"rc":rc,"pending_confirmation":rc==1}
    elif path == "/api/tx/select":
        rc = lib.sim_tx_select(integer(req,"ch",0,1),integer(req,"index",0,3))
        if rc: return 409,{"rc":rc,"error":"请先进入自定义发送页，并处理当前屏幕状态"}
    elif path == "/api/tx/scope":
        rc = lib.sim_tx_scope_select(integer(req,"mask",1,3))
        if rc: return 409,{"rc":rc,"error":"请先进入手动编辑报文页，并处理当前确认或草稿"}
    elif path == "/api/tx/target":
        rc = lib.sim_tx_target_select(integer(req,"mask",1,3))
        if rc: return 409,{"rc":rc,"error":"请先进入发送报文页，并处理当前确认或草稿"}
    elif path == "/api/tx/channel":
        enabled = req.get("enabled")
        if not isinstance(enabled, bool): raise InputError("发送开关须为 true 或 false")
        rc = lib.sim_tx_channel_send(integer(req,"ch",0,1),int(enabled))
        if rc < 0:
            message = "请在发送报文页操作通道开关" if rc == -104 else lib.sim_scope_text(rc).decode()
            return 409,{"rc":rc,"error":message}
        return 200,{"rc":rc,"pending_confirmation":rc==1}
    elif path == "/api/tx/scope/control":
        action = req.get("action")
        if action not in ("start","single","stop"): raise InputError("发送范围操作无效")
        rc = lib.sim_scope_control({"start":0,"single":1,"stop":2}[action],integer(req,"mask",1,3),integer(req,"index",0,3,0))
        if rc<0: return 409,{"rc":rc,"error":lib.sim_scope_text(rc).decode()}
        return 200,{"rc":rc,"pending_confirmation":rc==1}
    elif path == "/api/tx/control":
        action = req.get("action")
        if action not in ("single","stop","stop_all"): raise InputError("发送操作无效")
        rc = lib.sim_tx_control({"single":0,"stop":1,"stop_all":2}[action],integer(req,"ch",0,1),integer(req,"index",0,3,0))
        if rc: return 409,{"rc":rc,"error":lib.sim_tx_result_text(rc).decode()}
    elif path == "/api/record/name":
        prefix = req.get("prefix")
        if not isinstance(prefix,str) or "\0" in prefix or len(prefix.encode()) > 18:
            raise InputError("录制名称前缀最多18个 UTF-8 字节（中文通常每字3字节）")
        rc = lib.sim_record_name(prefix.encode())
        if rc: return (422 if rc in (-1,-2,-9,-12,-13) else 409),{"rc":rc,"error":lib.sim_tx_result_text(rc).decode() if rc <= -100 else log_error(rc)}
    elif path == "/api/log/rename":
        name = req.get("name")
        if not isinstance(name,str) or "\0" in name or not name or len(name.encode()) > 47:
            raise InputError("日志名称应为1到47个 UTF-8 字节")
        rc = lib.sim_log_rename(integer(req,"index",0,23),integer(req,"token",1,0xFFFFFFFF),name.encode())
        if rc: return (422 if rc in (-1,-2,-9,-12,-13) else 409),{"rc":rc,"error":lib.sim_tx_result_text(rc).decode() if rc <= -100 else log_error(rc)}
    elif path == "/api/database/import":
        name, content, encoding = req.get("name"), req.get("content"), req.get("encoding","auto")
        if not isinstance(name,str) or not isinstance(content,str) or not isinstance(encoding,str):
            raise InputError("数据库名称、文件内容或编码无效")
        try:
            raw = base64.b64decode(content,validate=True)
            parsed = parse_database(name,raw,encoding)
            local_slot = install_local_database(parsed)
        except (DatabaseImportError,binascii.Error,ValueError) as error:
            return 422,{"error":str(error)}
        if len(databases) >= 2: databases.pop(next(iter(databases)))
        token = secrets.token_urlsafe(16)
        databases[token] = parsed
        return 200,{"database":token,"local_slot":local_slot,**parsed}
    elif path == "/api/database/apply":
        token = req.get("database")
        if not isinstance(token,str) or token not in databases:
            return 409,{"error":"数据库预览已过期，请重新导入"}
        ch, start = integer(req,"ch",0,1), integer(req,"start_index",0,3)
        keys, revisions = req.get("frames"), req.get("revisions")
        if not isinstance(keys,list) or not 1 <= len(keys) <= 4-start or not all(isinstance(k,str) for k in keys) or len(set(keys)) != len(keys):
            raise InputError("请选择不重复的报文，且不要超出本路4个发送槽")
        if not isinstance(revisions,list) or len(revisions) != len(keys) or not all(type(r) is int and 0<=r<=0xFFFFFFFF for r in revisions):
            raise InputError("发送槽版本无效，请重新读取")
        frames = {f["key"]:f for f in databases[token]["frames"]}
        if any(k not in frames for k in keys): raise InputError("数据库报文已失效")
        rows = (TxUpdate * len(keys))()
        for n,key in enumerate(keys):
            frame = frames[key]
            row = rows[n]
            row.id,row.revision,row.period_ms,row.index = frame["id"],revisions[n],0,start+n
            row.flags = int(frame["extended"]) | (2 if frame["fd"] else 0) | (4 if frame["brs"] else 0)
            row.len = frame["length"]
        rc = lib.sim_tx_batch(ch,rows,len(keys))
        if rc: return 409,{"rc":rc,"error":lib.sim_tx_result_text(rc).decode()}
        return 200,{"rc":0,"added":len(keys),"note":"帧定义已加入，数据初值为00；未启动发送"}
    elif path == "/api/log/import":
        name, text = req.get("name"), req.get("text")
        if not isinstance(name,str) or not isinstance(text,str) or "\0" in name or "\0" in text:
            raise InputError("文件名和日志内容无效")
        raw = text.encode()
        if len(raw)>MAX_LOG_TEXT:
            raise InputError("日志超过 128 KiB 上限")
        line, frames = U(), U()
        rc = lib.sim_log_import(name.encode(),raw,len(raw),ctypes.byref(line),ctypes.byref(frames))
        if rc<0:
            message = log_error(rc) + (f"（第 {line.value} 行）" if line.value else "")
            return 422,{"rc":rc,"error":message,"line":line.value}
        return 200,{"rc":0,"index":rc,"frames":frames.value}
    elif path == "/api/plan/open":
        rc = lib.sim_plan_open(integer(req,"mode",0,2,0),integer(req,"ch",0,1,0))
        if rc: return 409,{"rc":rc,"error":(lib.sim_plan_result_text(rc) if rc<=-40 else lib.sim_local_result_text(rc)).decode()}
    elif path == "/api/plan/import":
        name, text = req.get("name"), req.get("text")
        if not isinstance(name,str) or not isinstance(text,str): raise InputError("请提供方案文件名和统一模板内容")
        try:
            plan = decode_plan(text)
            if "project" in req:
                if not isinstance(req["project"],str): raise InputError("项目名称必须是文本")
                if req["project"]: plan["project"] = req["project"]
            plan_store.save(name,plan)
            files, errors = refresh_plans()
        except (PlanFormatError,OSError) as error:
            return 422,{"error":str(error)}
        return 200,{"rc":0,"name":name,"files":len(files),"warnings":errors,"note":"已放入模拟SD；请在本机选方案加载，未启动发送"}
    elif path == "/api/plan/refresh":
        try: files, errors = refresh_plans()
        except (PlanFormatError,OSError) as error: return 422,{"error":str(error)}
        return 200,{"rc":0,"files":len(files),"warnings":errors}
    elif path == "/api/sim":
        # Validate the whole request before resetting or changing any state.
        speed, paused = state["speed"], state["paused"]
        if "speed" in req:
            speed = req["speed"]
            if isinstance(speed,bool) or not isinstance(speed,(int,float)) or not math.isfinite(speed) or not 0.25<=speed<=50:
                raise InputError("速度范围为 0.25 到 50")
        if "pause" in req:
            if not isinstance(req["pause"],bool): raise InputError("pause 必须为布尔值")
            paused = req["pause"]
        if "reset" in req and not isinstance(req["reset"],bool):
            raise InputError("reset 必须为布尔值")
        ms = None
        if "step" in req:
            if not paused: raise InputError("请先暂停模拟时间再单步")
            ms = integer(req,"step",1,1000)
        state["speed"], state["paused"] = float(speed), paused
        state["epoch"] += 1
        if req.get("reset"):
            lib.sim_reset()
            state["generation"] += 1
            databases.clear()
            load_builtin_databases()
            # A broken SD directory must not leave a reset with the previous
            # generation. Report file errors through the device's IO result.
            lib.sim_plan_refresh()
            service_plan_io()
        if ms is not None:
            for n in range(0,ms,TICK_MS): lib.sim_tick(min(TICK_MS,ms-n))
    else:
        return 404,{"error":"接口不存在"}
    return 200,{"rc":0}


class Handler(BaseHTTPRequestHandler):
    def log_message(self, fmt, *args):
        pass

    def _send(self, code, body, ctype="application/json; charset=utf-8", extra=None):
        self.send_response(code)
        for key, value in {"Content-Type":ctype,"Content-Length":str(len(body)),"Cache-Control":"no-store", "X-Content-Type-Options":"nosniff", **(extra or {})}.items():
            self.send_header(key,value)
        self.end_headers()
        try: self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError): pass

    def _json(self, code, obj):
        self._send(code,json.dumps(obj,ensure_ascii=False,allow_nan=False).encode())

    def do_GET(self):
        target = urlsplit(self.path)
        if target.path in ("/","/index.html","/workbench.js","/controls.js","/scroll.js"):
            name = target.path[1:] if target.path.endswith(".js") else "index.html"
            with open(os.path.join(HERE,name),"rb") as f:
                self._send(200,f.read(),"text/javascript; charset=utf-8" if name.endswith(".js") else "text/html; charset=utf-8")
        elif target.path == "/api/state":
            try: self._send(200,snapshot())
            except RuntimeError: self._json(500,{"error":"状态快照超过容量"})
        elif target.path == "/api/plan/template":
            example = parse_qs(target.query).get("example",["single"])[0]
            name = {"single":"pc-template.canplan.json","dual":"dual-channel-example.canplan.json","schema":"send-plan.schema.json"}.get(example)
            if not name:
                self._json(400,{"error":"模板类型无效"}); return
            try:
                with open(os.path.join(HERE,"examples",name),"rb") as source: data = source.read()
            except OSError:
                self._json(404,{"error":"模板未安装"}); return
            self._send(200,data,extra={"Content-Disposition":f'attachment; filename="{name}"'})
        elif target.path == "/api/plan/export":
            try:
                params = parse_qs(target.query)
                name = params["name"][0]
                token, generation = int(params["token"][0]),int(params["generation"][0])
                if not 1<=token<=0xFFFFFFFF or generation != state["generation"]: raise ValueError
                with lock:
                    if generation != state["generation"]: raise ValueError
                    refresh_plans()
                    n = lib.sim_snapshot(snap_buf,len(snap_buf))
                    if n<0: raise PlanFormatError("无法读取方案目录")
                    files = json.loads(snap_buf.raw[:n])["work"]["send_plans"]["files"]
                    if not any(f["name"]==name and f["token"]==token for f in files):
                        self._json(409,{"error":"方案文件已变化，请重新选择"}); return
                    data = plan_store.export(name)
            except (KeyError,ValueError,OSError) as error:
                self._json(409,{"error":str(error) or "文件选择已过期，请刷新后重试"}); return
            self._send(200,data,extra={"Content-Disposition":f"attachment; filename=\"send-plan.canplan.json\"; filename*=UTF-8''{quote(name,safe='')}"})
        elif target.path == "/api/log/export":
            try:
                params = parse_qs(target.query)
                idx = int(params.get("index",[""])[0])
                if not 0<=idx<24: raise ValueError
                token = int(params["token"][0]) if "token" in params else None
                generation = int(params["generation"][0]) if "generation" in params else None
                if token is not None and not 1<=token<=0xFFFFFFFF: raise ValueError
                if generation is not None and not 0<=generation<=0xFFFFFFFF: raise ValueError
            except ValueError:
                self._json(400,{"error":"文件索引无效"}); return
            with lock:
                if generation is not None and generation != state["generation"]:
                    self._json(409,{"error":"模拟器已重置，请重新选择文件"}); return
                filename = f"capture-{idx}.log"
                if token is not None:
                    name_buf = ctypes.create_string_buffer(256)
                    rc = lib.sim_log_name(idx,token,name_buf,len(name_buf))
                    if rc:
                        self._json(409,{"error":log_error(rc)}); return
                    filename = name_buf.value.decode().rsplit("/",1)[-1]
                    if not filename.lower().endswith((".log",".txt")): filename += ".log"
                n = lib.sim_log_export(idx,export_buf,len(export_buf))
                data = export_buf.raw[:n] if n>=0 else b""
            if n<0:
                self._json(422,{"error":log_error(n)}); return
            self._send(200,data,"text/plain; charset=utf-8",{"Content-Disposition":f"attachment; filename=\"capture-{idx}.log\"; filename*=UTF-8''{quote(filename,safe='')}"})
        elif target.path == "/api/database/example":
            fmt = parse_qs(target.query).get("format",[""])[0]
            if fmt not in ("dbc","dbf"):
                self._json(400,{"error":"示例格式应为 dbc 或 dbf"}); return
            path = os.path.join(HERE,"examples",f"review.{fmt}")
            try:
                with open(path,"rb") as f: data = f.read()
            except FileNotFoundError:
                self._json(404,{"error":"示例未安装"}); return
            self._send(200,data,"text/plain; charset=utf-8",{"Content-Disposition":f'attachment; filename="review.{fmt}"'})
        else:
            self._json(404,{"error":"页面不存在"})

    def do_POST(self):
        origin = self.headers.get("Origin")
        if origin and origin not in (f"http://localhost:{PORT}",f"http://127.0.0.1:{PORT}"):
            self._json(403,{"error":"仅接受本地模拟器页面的请求"}); return
        try:
            n = int(self.headers.get("Content-Length","0"))
            if not 0<n<=MAX_REQUEST: raise InputError("请求体为空或超过容量")
            if self.headers.get("Content-Type","").split(";")[0] != "application/json": raise InputError("请求须使用 JSON")
            req = json.loads(self.rfile.read(n),parse_constant=lambda _: (_ for _ in ()).throw(InputError("JSON 数值必须有限")))
            if not isinstance(req,dict): raise InputError("请求须为 JSON 对象")
            with lock:
                code, result = execute(urlsplit(self.path).path,req)
                service_plan_io()
        except (InputError,ValueError,UnicodeDecodeError) as exc:
            self._json(400,{"error":str(exc) or "JSON 请求无效"}); return
        self._json(code,result)


def main():
    load_builtin_databases()
    lib.sim_plan_refresh()
    service_plan_io()
    srv = ThreadingHTTPServer(("127.0.0.1",PORT),Handler)
    threading.Thread(target=ticker,daemon=True).start()
    print(f"模拟器已启动：http://localhost:{PORT} （Ctrl+C 退出）",flush=True)
    try: srv.serve_forever()
    except KeyboardInterrupt: pass
    finally: srv.server_close()


if __name__ == "__main__":
    main()
