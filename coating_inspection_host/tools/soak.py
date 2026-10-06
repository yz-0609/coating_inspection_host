#!/usr/bin/env python3
"""24h 协议仿真负载工具；每30秒依次运行PASS、FAIL、中止任务。

报告写入独立目录，不预填24h完成。duration 控制实际运行时长，默认86400秒。
同一个服务与模拟器持续运行，检查 SQLite/Outbox 一致性并采样CPU、内存、磁盘。
"""
import argparse
import hashlib
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import signal
import sqlite3
import sys
import time

from process_harness import Station


def resources(pid, directory):
    status = Path(f'/proc/{pid}/status').read_text()
    memory = {}
    for line in status.splitlines():
        if line.startswith(('VmRSS:', 'VmSize:', 'Threads:')):
            key, value = line.split(':', 1)
            memory[key] = value.strip()
    fields = Path(f'/proc/{pid}/stat').read_text().split()
    memory['cpu_ticks'] = int(fields[13]) + int(fields[14])
    memory['disk_bytes'] = sum(p.stat().st_size for p in directory.glob('inspection.db*'))
    return memory


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', required=True)
    parser.add_argument('--duration', type=float, default=86400)
    parser.add_argument('--interval', type=float, default=30)
    parser.add_argument('--output', required=True)
    args = parser.parse_args()
    if args.duration <= 0 or args.interval < 1:
        parser.error('duration 必须为正数，interval 至少1秒')
    directory = Path(args.output).resolve()
    directory.mkdir(parents=True, exist_ok=True)
    if (directory / 'inspection.db').exists():
        parser.error('输出目录已有检测数据库，请选择新目录以保持计数可解释')
    s = Station(str(Path(args.binary).resolve()), points=5, directory=directory)
    start = time.monotonic()
    report = dict(mode='PTY_SIMULATION', requested_seconds=args.duration, elapsed_seconds=0,
                  completed=False, binary_sha256=hashlib.sha256(Path(args.binary).read_bytes()).hexdigest(),
                  started_at_utc=datetime.now(timezone.utc).isoformat(), reconnects=0, pass_tasks=0, fail_tasks=0, aborted_tasks=0, samples=[], errors=[])
    interrupted = False
    def stop(_signal, _frame):
        nonlocal interrupted
        interrupted = True
    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    try:
        index = 0
        while not interrupted and time.monotonic() - start < args.duration:
            cycle_start = time.monotonic()
            s.begin('SOAK-' + str(index))
            mode = index % 3
            if mode == 2:
                s.point()
                s.service.send('cancel')
                s.service.wait('inspection_aborted')
                s.service.wait('command_done', predicate=lambda x: x['opcode'] == 'ABORT')
                report['aborted_tasks'] += 1
            else:
                for point in range(5):
                    s.point(250 if mode == 1 and point == 4 else 150)
                result = s.service.wait('inspection_completed')['result']
                assert result == ('PASS' if mode == 0 else 'FAIL')
                ack = s.service.wait('command_done', predicate=lambda x: x['opcode'] == 'APPLY_RESULT')
                assert ack['status'] == 'ACKNOWLEDGED'
                report['pass_tasks' if mode == 0 else 'fail_tasks'] += 1
            s.present(False)
            if mode == 2:
                s.service.send('reset')
                s.service.wait('command_done', predicate=lambda x: x['opcode'] == 'RESET_FAULT')
            s.present(True)
            # 数据库验证使用独立只读连接，不借用业务线程连接。
            db = sqlite3.connect(f'file:{s.db}?mode=ro', uri=True)
            complete = db.execute("SELECT count(*) FROM inspection WHERE state='COMPLETED'").fetchone()[0]
            outbox = db.execute('SELECT count(*) FROM outbox').fetchone()[0]
            assert complete == outbox == report['pass_tasks'] + report['fail_tasks']
            assert db.execute('PRAGMA integrity_check').fetchone()[0] == 'ok'
            assert db.execute("SELECT count(*) FROM outbox WHERE state!='PENDING' OR attempt_count!=0").fetchone()[0] == 0
            db.close()
            report['elapsed_seconds'] = round(time.monotonic() - start, 3)
            sample = dict(elapsed_seconds=report['elapsed_seconds'], **resources(s.service.proc.pid, directory))
            report['samples'].append(sample)
            (directory / 'report.json').write_text(json.dumps(report, indent=2, ensure_ascii=False))
            # 测试工具本身不累计24h日志到内存，事件样本保留在最终摘要。
            report['reconnects'] += sum(x.get('event') == 'workstation_error' for x in s.service.history)
            s.service.history.clear()
            s.service.buffered.clear()
            s.sim.history.clear()
            s.sim.buffered.clear()
            index += 1
            while not interrupted and time.monotonic() - cycle_start < args.interval and time.monotonic() - start < args.duration:
                time.sleep(.1)
        report['elapsed_seconds'] = round(time.monotonic() - start, 3)
        report['completed'] = not interrupted and report['elapsed_seconds'] >= args.duration
    except Exception as exc:
        report['errors'].append(str(exc))
        raise
    finally:
        s.close()
        report['elapsed_seconds'] = round(time.monotonic() - start, 3)
        (directory / 'report.json').write_text(json.dumps(report, indent=2, ensure_ascii=False))
        print(json.dumps(report | {'samples': len(report['samples'])}, ensure_ascii=False))
    return 0 if report['completed'] and not report['errors'] else 1


if __name__ == '__main__':
    sys.exit(main())
