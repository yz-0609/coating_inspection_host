"""测试/演示共享的子进程工具：隔离临时数据库、配置和PTY，按JSON事件等待。

仅用于Linux软件验证，不属于正式服务。工具持有子进程并负责有界退出。
"""
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time
ROOT = Path(__file__).resolve().parents[1]


class Process:
    def __init__(self, command, env=None):
        self.events = queue.Queue()
        self.history = []
        self.buffered = []
        self.proc = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, text=True, bufsize=1, env=env)
        def reader():
            for line in self.proc.stdout:
                try:
                    value = json.loads(line)
                except ValueError:
                    value = dict(event='non_json', message=line.rstrip())
                self.history.append(value)
                self.events.put(value)
        self.thread = threading.Thread(target=reader, daemon=True)
        self.thread.start()

    def send(self, line):
        self.proc.stdin.write(line + '\n')
        self.proc.stdin.flush()

    def wait(self, event, timeout=5, predicate=lambda x: True):
        for index, value in enumerate(self.buffered):
            if value.get('event') == event and predicate(value):
                return self.buffered.pop(index)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            try:
                value = self.events.get(timeout=max(.001, deadline - time.monotonic()))
            except queue.Empty:
                break
            if value.get('event') == event and predicate(value):
                return value
            self.buffered.append(value)
        raise AssertionError(f'等待 {event} 超时；末尾日志={self.history[-12:]}')

    def close(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=3)
        self.thread.join(timeout=1)
        self.proc.stdin.close()
        self.proc.stdout.close()


class Station:
    def __init__(self, binary, backend='modbus', crash=None, points=1, measurement_ms=30000,
                 directory=None):
        self.tmp = tempfile.TemporaryDirectory(prefix='inspection-test-') if directory is None else None
        self.dir = Path(self.tmp.name if self.tmp else directory)
        self.dir.mkdir(parents=True, exist_ok=True)
        self.db = self.dir / 'inspection.db'
        self.sim = Process([sys.executable, str(ROOT / 'tools/workstation_sim.py')]) if backend == 'modbus' else None
        device = self.sim.wait('ready')['device'] if self.sim else ''
        (self.dir / 'replay.jsonl').write_text('{"delay_ms":0,"connection":true}\n')
        config = dict(station_id='test', measurement_timeout_ms=measurement_ms,
                      instrument=dict(type='replay', replay_file='replay.jsonl', loop=False, device_name='Synthetic'),
                      workstation=dict(type=backend, device=device),
                      database=dict(path=str(self.db), min_free_mb=0), cloud=dict(adapter='disabled'),
                      products=dict(demo=dict(rule_version='demo-v1', required_points=points, min_um=100, max_um=200, substrate='FE')),
                      testing=dict(allow_controls=True))
        self.config = self.dir / 'config.yaml'
        self.config.write_text(json.dumps(config))
        env = os.environ.copy()
        if crash:
            env['INSPECTION_TEST_CRASH_AT'] = crash
        self.service = Process([binary, '--config', str(self.config)], env)
        self.service.wait('sync_complete')
        self.present(True)
        self.service.send('reset')
        self.service.wait('command_done', predicate=lambda x: x['opcode'] == 'RESET_FAULT' and x['status'] == 'ACKNOWLEDGED')

    def control(self, **values):
        if not self.sim:
            raise AssertionError('此场景需要 PTY 模拟器')
        self.sim.send(json.dumps(values))
        return self.sim.wait('control')

    def present(self, value):
        if self.sim:
            self.control(present=value)
        else:
            self.service.send('sim present ' + str(int(value)))
        # 等待业务确实看到了输入，不依赖任意固定休眠。
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline:
            self.service.send('status')
            state = self.service.wait('status')
            if state['present'] == value:
                return
            time.sleep(.03)
        raise AssertionError('到位状态没有被主站读取')

    def begin(self, name='A'):
        self.service.send(f'start {name} demo')
        return self.service.wait('inspection_started')['inspection_id']

    def point(self, value=150, mode='FE', button=False):
        self.service.send('arm')
        self.service.wait('window_armed')
        # 单调时间必须晚于窗口开启；仪器线程异步处理此控制请求。
        time.sleep(.003)
        self.service.send(f'sim measure {value} {mode}')
        candidate = self.service.wait('candidate')['candidate_id']
        if button:
            self.control(button='action')
        else:
            self.service.send('confirm ' + candidate)
        self.service.wait('point_saved')
        return candidate

    def records(self):
        self.service.send('query')
        return self.service.wait('query')['records']

    def close(self):
        self.service.close()
        if self.sim:
            self.sim.close()
        if self.tmp:
            self.tmp.cleanup()


