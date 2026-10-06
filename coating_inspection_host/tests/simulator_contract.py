#!/usr/bin/env python3
"""模拟器协议契约测试：独立黄金CRC向量、命令幂等及故障后的输出门控。"""
import struct
import sys
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from workstation_sim import Model, RequestParser, crc16, framed, split


def command(epoch, token, sequence, opcode, arg=0):
    return [*split(epoch), *split(token), *split(sequence), opcode, *split(arg)]


def main():
    assert crc16(bytes.fromhex('01 03 00 00 00 0a')) == 0xCDC5
    request = bytes.fromhex('01 03 00 00 00 0a c5 cd')
    parser = RequestParser()
    assert not parser.feed(request[:3])
    assert parser.feed(request[3:]) == [request]
    assert parser.feed(request + request) == [request, request]
    bad = request[:-1] + bytes([request[-1] ^ 1])
    assert parser.feed(bad + request) == [request] and parser.crc_errors > 0
    now = [10.0]
    with patch('workstation_sim.time.monotonic', lambda: now[0]):
        model = Model()
        sync = command(1, 0, 1, 1)
        model.execute(sync)
        assert model.ack_status == 1 and model.outputs == 4
        hb = framed(struct.pack('>BBHH', 1, 6, 0x100, 1))
        model.request(hb)
        model.control(dict(present=True))
        model.execute(command(1, 0, 2, 5))
        start = command(1, 99, 3, 2)
        model.execute(start)
        model.execute(start)
        assert model.state == 3 and model.token == 99
        model.execute(command(1, 100, 3, 2))
        assert model.ack_status == 4 and model.token == 99
        model.execute(command(1, 100, 4, 3, 1))
        assert model.ack_status == 3 and model.outputs == 0
        result = command(1, 99, 5, 3, 1)
        model.execute(result)
        assert model.outputs == 1
        now[0] += 1.6
        model.request(hb)  # 相同心跳不能掩盖失联。
        assert model.outputs == 4 and model.state == 5
        model.execute(result)
        assert model.ack_status == 1 and model.outputs == 4  # 原ACK不会恢复PASS。
        model.execute(command(2, 0, 1, 1))
        assert model.ack_epoch == 2 and model.ack_status == 1 and model.token == 0
        model.execute(command(1, 99, 6, 2))
        assert model.ack_status == 4
        model.control(dict(button='action'))
        seq = model.event_sequence
        model.control(dict(button='reset'))
        assert model.event_sequence == seq and model.event_code == 1 and model.errors == 1
        model.execute(command(2, 0, 2, 4, seq))
        assert model.event_code == 0
        assert model.request(framed(struct.pack('>BBHH', 1, 4, 0, 0)))[2] == 3
        assert model.request(framed(struct.pack('>BBHH', 1, 6, 0x101, 1)))[2] == 2
        assert model.request(framed(struct.pack('>BBHH', 1, 7, 0, 0)))[2] == 1
    print('模拟器协议契约通过：CRC、分片、幂等、旧token/epoch、按钮保持、相同心跳及故障覆盖')


if __name__ == '__main__':
    main()
