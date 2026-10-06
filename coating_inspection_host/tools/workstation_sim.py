#!/usr/bin/env python3
"""Linux 测试工具：PTY 上的独立 Modbus RTU 工位模拟器。

仅模拟寄存器、状态和逻辑灯；不模拟 STM32 固件、GPIO、DMA 或电气时序。
stdin 接收一行 JSON 控制命令，stdout 输出 ready/control 事件。真实主站通过
ready 中的 device 路径访问，CRC16 与请求解析独立于 libmodbus。
"""
import argparse
import errno
import json
import os
import pty
import selectors
import struct
import sys
import termios
import time
import tty
from collections import deque


def crc16(data):
    """标准 Modbus CRC16：低字节先放入帧尾，不与寄存器大端字序混淆。"""
    value = 0xFFFF
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ (0xA001 if value & 1 else 0)
    return value


def framed(data):
    return data + struct.pack('<H', crc16(data))


def split(value):
    return [(value >> 16) & 0xFFFF, value & 0xFFFF]


class Model:
    """所有模拟状态归单个事件循环所有。健康时间由变化的心跳推进。"""
    def __init__(self):
        self.started = time.monotonic()
        self.state, self.outputs, self.fault = 5, 4, 6
        self.present, self.health = False, 3
        self.epoch = self.token = self.last_sequence = 0
        self.ack_epoch = self.ack_sequence = self.ack_status = 0
        self.event_sequence = self.event_code = self.errors = 0
        self.last_heartbeat = 0
        self.host_health = None
        self.heartbeat_writes = 0
        self.last_command = None
        self.last_status = 0
        self.reset_reason = 0
        self.drop_responses = 0
        self.reject_opcode = 0
        self.drop_ack = False
        self.ack_delay_ms = 0
        self.deferred_ack = None
        self.old_ack = None
        self.version = 0x0200
        self.output_cleared_at = None

    def healthy(self):
        return self.host_health is not None and time.monotonic() - self.host_health <= 1.5

    def tick(self):
        now = time.monotonic()
        if self.epoch and not self.healthy():
            if self.outputs == 1:
                self.output_cleared_at = now
            self.state, self.outputs, self.fault, self.token = 5, 4, 1, 0
        if self.deferred_ack and now >= self.deferred_ack[0]:
            _, self.ack_epoch, self.ack_sequence, self.ack_status = self.deferred_ack
            self.deferred_ack = None

    def snapshot(self):
        self.tick()
        ack = self.old_ack or [self.ack_epoch, self.ack_sequence, self.ack_status]
        uptime = int((time.monotonic() - self.started) * 1000) & 0xFFFFFFFF
        return [self.state, int(self.present), self.outputs, self.fault, self.health,
                self.heartbeat_writes & 0xFFFF, *split(uptime), *split(self.token),
                *split(ack[1]), ack[2], *split(self.event_sequence), self.event_code,
                self.reset_reason, *split(self.errors), self.version, *split(ack[0])]

    def set_ack(self, epoch, sequence, status):
        if self.drop_ack:
            return
        if self.ack_delay_ms:
            # 重传不能不断推迟同一条原确认，保持原始执行时间。
            if self.deferred_ack is None:
                self.deferred_ack = (time.monotonic() + self.ack_delay_ms / 1000,
                                     epoch, sequence, status)
        else:
            self.ack_epoch, self.ack_sequence, self.ack_status = epoch, sequence, status

    def execute(self, words):
        self.tick()
        epoch = (words[0] << 16) | words[1]
        token = (words[2] << 16) | words[3]
        sequence = (words[4] << 16) | words[5]
        opcode = words[6]
        arg = (words[7] << 16) | words[8]
        if self.last_command and (epoch, sequence) == (self.epoch, self.last_sequence):
            self.set_ack(epoch, sequence, self.last_status if words == self.last_command else 4)
            return
        status = 1
        if opcode != 1 and (epoch != self.epoch or sequence <= self.last_sequence):
            status = 4
        elif opcode == self.reject_opcode:
            status = 2
        elif opcode == 1:
            if not epoch or epoch <= self.epoch or token or arg:
                status = 4 if epoch <= self.epoch else 6
            else:
                self.epoch = epoch
                self.last_sequence = 0
                self.last_command = None
                self.state, self.outputs, self.fault, self.token = 5, 4, 6, 0
                self.event_code = 0
        elif opcode == 4:
            if arg != self.event_sequence or not self.event_code:
                status = 6
            else:
                self.event_code = 0
        elif opcode == 5:
            if self.state != 5:
                status = 2
            elif not self.healthy() or not self.health:
                status = 5
            else:
                self.state = 2 if self.present else 1
                self.outputs = self.fault = self.token = 0
        elif opcode == 2:
            if self.state != 2 or not self.present:
                status = 2
            elif not token:
                status = 3
            elif not self.healthy() or not self.health:
                status = 5
            else:
                self.state, self.token, self.outputs = 3, token, 0
        elif opcode == 3:
            if self.state != 3:
                status = 2
            elif token != self.token:
                status = 3
            elif arg not in (1, 2):
                status = 6
            elif not self.healthy() or not self.health:
                status = 5
            else:
                self.state, self.outputs = 4, arg
        elif opcode == 6:
            if self.token and token != self.token:
                status = 3
            else:
                self.state, self.outputs, self.fault, self.token = 5, 4, 5, 0
        else:
            status = 6
        self.set_ack(epoch, sequence, status)
        if epoch == self.epoch and sequence > self.last_sequence:
            self.last_sequence, self.last_command, self.last_status = sequence, words[:], status

    def control(self, item):
        if 'present' in item:
            self.present = bool(item['present'])
            if not self.present and self.state == 3:
                self.state, self.outputs, self.fault, self.token = 5, 4, 2, 0
            elif not self.present and self.state == 4:
                self.state, self.outputs, self.token = 1, 0, 0
            elif self.state in (1, 2):
                self.state = 2 if self.present else 1
        if 'button' in item:
            if self.event_code:
                self.errors += 1
            else:
                self.event_sequence = (self.event_sequence + 1) & 0xFFFFFFFF
                self.event_code = 1 if item['button'] == 'action' else 2
        if item.get('reset_mcu'):
            self.state, self.outputs, self.fault, self.token = 5, 4, 6, 0
            self.epoch = self.last_sequence = self.ack_epoch = self.ack_sequence = self.ack_status = 0
            self.last_command = self.deferred_ack = self.host_health = None
            self.started = time.monotonic()
            self.reset_reason = 2
        for name in ('drop_responses', 'reject_opcode', 'drop_ack', 'ack_delay_ms', 'old_ack', 'version', 'health'):
            if name in item:
                setattr(self, name, item[name])
        return self.describe()

    def describe(self):
        self.tick()
        return dict(state=self.state, outputs=self.outputs, token=self.token, epoch=self.epoch,
                    ack_sequence=self.ack_sequence, event_code=self.event_code,
                    heartbeat_writes=self.heartbeat_writes, errors=self.errors,
                    output_cleared_at=self.output_cleared_at)

    def request(self, request):
        """处理合法 CRC 的本机请求；异常响应使用标准功能码|0x80。"""
        self.tick()
        address, function = request[0], request[1]
        if address != 1:  # 不执行广播业务命令。
            return None
        def exception(code):
            return framed(bytes([address, function | 0x80, code]))
        if function in (3, 4):
            start, count = struct.unpack('>HH', request[2:6])
            if count == 0 or count > 125:
                return exception(3)
            if function == 4 and start + count <= 22:
                values = self.snapshot()[start:start + count]
            elif function == 3 and start == 0x100 and count == 1:
                values = [self.last_heartbeat]
            elif function == 3 and 0x110 <= start and start + count <= 0x119:
                values = (self.last_command or [0] * 9)[start - 0x110:start - 0x110 + count]
            else:
                return exception(2)
            response = framed(bytes([address, function, count * 2]) + struct.pack('>' + 'H' * count, *values))
        elif function == 6:
            register, value = struct.unpack('>HH', request[2:6])
            if register != 0x100:
                return exception(2)
            if value != self.last_heartbeat:
                self.last_heartbeat = value
                self.host_health = time.monotonic()
                self.heartbeat_writes += 1
            response = request
        elif function == 16:
            start, count, length = struct.unpack('>HHB', request[2:7])
            if start != 0x110:
                return exception(2)
            if count != 9 or length != 18:
                return exception(3)
            self.execute(list(struct.unpack('>9H', request[7:25])))
            response = framed(request[:6])
        else:
            return exception(1)
        if self.drop_responses:
            self.drop_responses -= 1
            return None
        return response


class RequestParser:
    """字节流解析器，按功能码计算请求长度；CRC 错误按字节重新寻找合法请求。"""
    def __init__(self):
        self.buffer = bytearray()
        self.crc_errors = 0

    def feed(self, data):
        self.buffer.extend(data)
        result = []
        while len(self.buffer) >= 8:
            if self.buffer[0] not in (0, 1):
                del self.buffer[0]
                continue
            function = self.buffer[1]
            size = 9 + self.buffer[6] if function == 16 else 8
            if size > 256:
                del self.buffer[0]
                continue
            if len(self.buffer) < size:
                break
            frame = bytes(self.buffer[:size])
            if crc16(frame[:-2]) != struct.unpack('<H', frame[-2:])[0]:
                self.crc_errors += 1
                del self.buffer[0]
                continue
            del self.buffer[:size]
            result.append(frame)
        # 不允许畸形请求无限占用内存。
        if len(self.buffer) > 512:
            self.buffer.clear()
        return result


def emit(event, **fields):
    print(json.dumps(dict(event=event, **fields), ensure_ascii=False), flush=True)


def main():
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument('--scenario', help='JSON 数组，每条控制包含 at_ms 相对启动时间')
    config = args.parse_args()
    scheduled = deque(sorted(json.load(open(config.scenario)), key=lambda x: x['at_ms'])) if config.scenario else deque()
    master, slave = pty.openpty()
    tty.setraw(slave)
    os.set_blocking(master, False)
    device = os.ttyname(slave)
    # 保留 slave，主站暂时断开时 master 不会永久失效。
    model, parser = Model(), RequestParser()
    selection = selectors.DefaultSelector()
    selection.register(master, selectors.EVENT_READ)
    selection.register(sys.stdin, selectors.EVENT_READ)
    control_buffer = b''
    emit('ready', device=device, simulation_only=True)
    try:
        running = True
        while running:
            model.tick()
            while scheduled and (time.monotonic() - model.started) * 1000 >= scheduled[0]['at_ms']:
                model.control(scheduled.popleft())
            for key, _ in selection.select(0.01):
                if key.fd == master:
                    try:
                        data = os.read(master, 4096)
                    except OSError as exc:
                        if exc.errno in (errno.EIO, errno.EAGAIN):
                            continue
                        raise
                    for request in parser.feed(data):
                        response = model.request(request)
                        if response:
                            os.write(master, response)
                else:
                    data = os.read(sys.stdin.fileno(), 4096)
                    if not data:
                        selection.unregister(sys.stdin)
                        continue
                    control_buffer += data
                    while b'\n' in control_buffer:
                        line, control_buffer = control_buffer.split(b'\n', 1)
                        try:
                            item = json.loads(line)
                            if item.get('quit'):
                                running = False
                            if item.get('disconnect'):
                                running = False
                            emit('control', **model.control(item), crc_errors=parser.crc_errors)
                        except (ValueError, TypeError) as exc:
                            emit('control_error', error=str(exc))
    finally:
        selection.close()
        os.close(master)
        os.close(slave)


if __name__ == '__main__':
    main()
