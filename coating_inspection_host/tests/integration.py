#!/usr/bin/env python3
"""进程集成验收：真实 SQLite、真实 libmodbus 主站、独立 PTY 模拟器。

每个测试使用临时配置和数据库；只读取逐行 JSON 事件，不猜测终端字符串。
故障注入结果明确标记仿真，不能据此声明电气、仪器或 STM32 实机验收。
"""
import json
import os
from pathlib import Path
import queue
import signal
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time

ROOT = Path(__file__).resolve().parents[1]


sys.path.insert(0, str(ROOT / 'tools'))
from process_harness import Process, Station


def scenario_quality(binary, backend, value, mode, result):
    station = Station(binary, backend)
    try:
        station.begin()
        station.point(value, mode)
        completed = station.service.wait('inspection_completed')
        assert completed['result'] == result
        ack = station.service.wait('command_done', predicate=lambda x: x['opcode'] == 'APPLY_RESULT')
        assert ack['status'] == 'ACKNOWLEDGED'
        records = station.records()
        assert records[0]['result'] == result and len(records[0]['measurements']) == 1
        station.service.send('outbox')
        events = station.service.wait('outbox')['records']
        assert len(events) == 1 and events[0]['state'] == 'PENDING' and events[0]['attempt_count'] == 0
        assert events[0]['payload']['event_id'] == events[0]['event_id']
        if station.sim:
            assert station.control()['outputs'] == (1 if result == 'PASS' else 2)
        station.present(False)
    finally:
        station.close()


def scenario_candidates(binary):
    s = Station(binary, points=2)
    try:
        s.begin()
        s.service.send('sim measure 150 FE')
        s.service.wait('measurement_ignored')
        s.service.send('arm')
        s.service.wait('window_armed')
        time.sleep(.003)
        s.service.send('sim measure 150 FE')
        first = s.service.wait('candidate')['candidate_id']
        s.service.send('sim measure 250 FE')
        s.service.wait('candidate')
        s.service.send('confirm ' + first)
        s.service.wait('point_saved')
        s.service.send('confirm ' + first)
        s.service.wait('cli_error')
        s.point(150)
        assert s.service.wait('inspection_completed')['result'] == 'PASS'
        s.service.wait('command_done', predicate=lambda x: x['opcode'] == 'APPLY_RESULT')
        assert [p['thickness_milli_um'] for p in s.records()[0]['measurements']] == [150000, 150000]
    finally:
        s.close()


def scenario_abort(binary, cause):
    s = Station(binary, points=5, measurement_ms=200 if cause == 'timeout' else 30000)
    try:
        s.begin()
        if cause != 'timeout':
            s.point()
        if cause == 'ble':
            s.service.send('sim connection 0')
        elif cause == 'removed':
            s.present(False)
        elif cause == 'cancel':
            s.service.send('cancel')
        elif cause == 'mcu_reset':
            s.control(reset_mcu=True)
        elif cause == 'overflow':
            s.service.send('sim overflow')
        elif cause == 'timeout':
            s.service.send('arm')
            s.service.wait('window_armed')
        elif cause == 'disconnect':
            s.control(disconnect=True)
        s.service.wait('inspection_aborted')
        records = s.records()
        assert records[0]['state'] == 'ABORTED' and not records[0]['result']
        assert len(records[0]['measurements']) == (0 if cause == 'timeout' else 1)
    finally:
        s.close()


def scenario_execution_unknown(binary):
    s = Station(binary)
    try:
        s.begin()
        s.control(drop_ack=True)
        s.point()
        s.service.wait('inspection_completed')
        ack = s.service.wait('command_done', predicate=lambda x: x['opcode'] == 'APPLY_RESULT')
        assert ack['status'] == 'UNKNOWN'
        records = s.records()
        assert records[0]['result'] == 'PASS'
        assert any(c['command'] == 'APPLY_RESULT' and c['status'] == 'UNKNOWN' for c in records[0]['actuation'])
        transmissions = [x for x in s.service.history if x.get('event') == 'command_transmit' and x['opcode'] == 'APPLY_RESULT']
        assert len(transmissions) == 3 and len({x['command_seq'] for x in transmissions}) == 1
    finally:
        s.close()


def scenario_heartbeat(binary, kill=False):
    s = Station(binary)
    try:
        s.begin()
        s.point()
        s.service.wait('inspection_completed')
        s.service.wait('command_done', predicate=lambda x: x['opcode'] == 'APPLY_RESULT')
        assert s.control()['outputs'] == 1
        if kill:
            s.service.proc.kill()
            s.service.proc.wait(timeout=3)
        else:
            s.service.send('sim stall 2500')
        time.sleep(.8)
        first = s.control()['heartbeat_writes']
        time.sleep(1.05)
        state = s.control()
        assert state['heartbeat_writes'] == first, state
        assert state['outputs'] == 4 and state['state'] == 5, state
        if not kill:
            s.service.wait('sync_complete')
            s.service.send('reset')
            s.service.wait('command_done', predicate=lambda x: x['opcode'] == 'RESET_FAULT' and x['status'] == 'ACKNOWLEDGED')
            assert s.records()[0]['result'] == 'PASS'
            assert s.control()['outputs'] == 0
    finally:
        s.close()


def scenario_recovery(binary, crash):
    with tempfile.TemporaryDirectory(prefix='inspection-crash-') as directory:
        s = Station(binary, backend='mock', crash=crash, directory=directory, points=1 if crash else 5)
        try:
            s.begin()
            if crash:
                s.point()
                assert s.service.proc.wait(timeout=5) == 86
            else:
                for _ in range(3):
                    s.point()
                s.service.proc.kill()
                s.service.proc.wait(timeout=3)
            database = sqlite3.connect(s.db)
            state, result = database.execute('SELECT state,result FROM inspection').fetchone()
            count = database.execute('SELECT count(*) FROM outbox').fetchone()[0]
            assert count == (1 if crash == 'after_final_commit' else 0)
            database.close()
            s.service.close()
            s.service = Process([binary, '--config', str(s.config)])
            s.service.wait('sync_complete')
            records = s.records()
            if crash == 'after_final_commit':
                assert records[0]['result'] == 'PASS' and records[0]['state'] == 'COMPLETED'
            else:
                assert records[0]['state'] == 'ABORTED' and records[0]['abort_reason'] == 'service_restart'
            assert len(records[0]['measurements']) == (1 if crash else 3)
            assert not any(x.get('event') == 'command_transmit' and x['opcode'] == 'APPLY_RESULT' for x in s.service.history)
        finally:
            s.close()


def scenario_reject_start(binary):
    s = Station(binary)
    try:
        s.control(reject_opcode=2)
        s.service.send('start reject demo')
        s.service.wait('inspection_aborted')
        assert s.records()[0]['state'] == 'ABORTED'
    finally:
        s.close()


def scenario_button(binary):
    s = Station(binary)
    try:
        s.begin()
        s.control(button='action')
        s.service.wait('window_armed')
        time.sleep(.003)
        s.service.send('sim measure 150 FE')
        s.service.wait('candidate')
        # 等待开启窗口的按钮已被确认，避免第二个按钮被合法忙碌策略拒绝。
        deadline = time.monotonic() + 3
        while s.control()['event_code'] and time.monotonic() < deadline:
            time.sleep(.02)
        s.control(button='action')
        s.service.wait('point_saved')
        s.service.wait('inspection_completed')
        s.service.wait('command_done', predicate=lambda x: x['opcode'] == 'APPLY_RESULT')
        assert len(s.records()[0]['measurements']) == 1
    finally:
        s.close()


def scenario_database_lock(binary):
    s = Station(binary)
    try:
        s.begin()
        s.service.send('arm')
        s.service.wait('window_armed')
        time.sleep(.003)
        s.service.send('sim measure 150 FE')
        candidate = s.service.wait('candidate')['candidate_id']
        db = sqlite3.connect(s.db)
        db.execute('BEGIN IMMEDIATE')
        s.service.send('confirm ' + candidate)
        s.service.wait('fatal')
        db.rollback()
        db.close()
        assert not any(x.get('event') == 'command_transmit' and x['opcode'] == 'APPLY_RESULT' for x in s.service.history)
    finally:
        s.close()


def scenario_ack_delay(binary):
    s = Station(binary)
    try:
        s.begin()
        s.control(ack_delay_ms=500)
        s.point()
        s.service.wait('inspection_completed')
        ack = s.service.wait('command_done', predicate=lambda x: x['opcode'] == 'APPLY_RESULT')
        assert ack['status'] == 'ACKNOWLEDGED'
        assert len([x for x in s.service.history if x.get('event') == 'command_transmit' and x['opcode'] == 'APPLY_RESULT']) >= 2
    finally:
        s.close()


def scenario_old_ack(binary):
    s = Station(binary)
    try:
        s.begin()
        current = s.control()
        s.control(old_ack=[current['epoch'] - 1, current['ack_sequence'] + 1, 1])
        s.point()
        s.service.wait('inspection_completed')
        ack = s.service.wait('command_done', predicate=lambda x: x['opcode'] == 'APPLY_RESULT')
        assert ack['status'] == 'UNKNOWN'
        assert s.records()[0]['result'] == 'PASS'
    finally:
        s.close()


def scenario_storage_limit(binary):
    s = Station(binary)
    try:
        s.begin()
        s.service.send('arm')
        s.service.wait('window_armed')
        time.sleep(.003)
        s.service.send('sim measure 150 FE')
        candidate = s.service.wait('candidate')['candidate_id']
        s.service.send('sim file_limit')
        s.service.wait('file_limit_enabled')
        s.service.send('confirm ' + candidate)
        s.service.wait('fatal')
        assert not any(x.get('event') == 'command_transmit' and x['opcode'] == 'APPLY_RESULT' for x in s.service.history)
        db = sqlite3.connect(s.db)
        assert db.execute('SELECT count(*) FROM outbox').fetchone()[0] == 0
        assert db.execute('SELECT count(*) FROM measurement').fetchone()[0] == 0
        db.close()
    finally:
        s.close()


def scenario_sigterm(binary):
    s = Station(binary, points=5)
    try:
        s.begin()
        s.point()
        s.service.proc.terminate()
        assert s.service.proc.wait(timeout=5) == 0
        assert s.control()['outputs'] == 4
        db = sqlite3.connect(s.db)
        assert db.execute('SELECT state,abort_reason FROM inspection').fetchone() == ('ABORTED', 'service_shutdown')
        assert db.execute('SELECT count(*) FROM measurement').fetchone()[0] == 1
        db.close()
    finally:
        s.close()


def scenario_cloud_rejected_config(binary):
    with tempfile.TemporaryDirectory() as directory:
        p = Path(directory) / 'config.yaml'
        p.write_text(json.dumps(dict(cloud=dict(adapter='onenet'))))
        result = subprocess.run([binary, '--config', str(p)], stdout=subprocess.PIPE, text=True, timeout=3)
        assert result.returncode == 1 and 'OneNET' in result.stdout


def scenario_unknown_version(binary):
    s = Station(binary, points=5)
    try:
        s.begin()
        s.control(version=0x0300)
        s.service.wait('fatal')
        assert s.records()[0]['state'] == 'ABORTED'
        time.sleep(1.7)
        assert s.control()['outputs'] == 4
        assert not any(x.get('event') == 'command_transmit' and x['opcode'] == 'APPLY_RESULT' for x in s.service.history)
    finally:
        s.close()


def main():
    binary = sys.argv[1]
    scenarios = []
    for backend in ('mock', 'modbus'):
        for value, mode, result in ((100, 'FE', 'PASS'), (200, 'FE', 'PASS'), (99.999, 'FE', 'FAIL'), (150, 'NFE', 'FAIL')):
            scenarios.append((f'{backend} quality {value}/{mode}', lambda b=backend,v=value,m=mode,r=result: scenario_quality(binary,b,v,m,r)))
    scenarios += [('candidate binding/same values', lambda: scenario_candidates(binary)),
                  ('button events', lambda: scenario_button(binary)),
                  ('start reject', lambda: scenario_reject_start(binary)),
                  ('execution unknown/retransmit', lambda: scenario_execution_unknown(binary)),
                  ('business stall heartbeat', lambda: scenario_heartbeat(binary)),
                  ('kill heartbeat', lambda: scenario_heartbeat(binary, True)),
                  ('database lock', lambda: scenario_database_lock(binary)),
                  ('delayed ack', lambda: scenario_ack_delay(binary)),
                  ('old epoch ack', lambda: scenario_old_ack(binary)),
                  ('real storage write limit', lambda: scenario_storage_limit(binary)),
                  ('SIGTERM abort', lambda: scenario_sigterm(binary)),
                  ('OneNET disabled config', lambda: scenario_cloud_rejected_config(binary)),
                  ('unknown protocol version', lambda: scenario_unknown_version(binary))]
    for cause in ('ble', 'removed', 'cancel', 'mcu_reset', 'overflow', 'timeout', 'disconnect'):
        scenarios.append((f'abort {cause}', lambda c=cause: scenario_abort(binary,c)))
    for crash in (None, 'before_final_commit', 'after_final_commit'):
        scenarios.append((f'recovery {crash}', lambda c=crash: scenario_recovery(binary,c)))
    for name, run in scenarios:
        run()
        print('PASS ' + name, flush=True)
    print(f'{len(scenarios)} 个进程集成场景通过；均为软件/协议仿真。', flush=True)


if __name__ == '__main__':
    main()
