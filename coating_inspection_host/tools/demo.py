#!/usr/bin/env python3
"""可重复的无硬件演示：真实主站或Mock，自动逐点开启和确认，打印持久化结果。"""
import argparse
import json
from pathlib import Path
from process_harness import Station


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', default='build/inspection_service')
    parser.add_argument('--backend', choices=['modbus', 'mock'], default='modbus')
    parser.add_argument('--result', choices=['pass', 'fail', 'aborted'], default='pass')
    parser.add_argument('--output', required=True, help='新的演示数据目录，保留数据库和导出事件')
    args = parser.parse_args()
    output = Path(args.output).resolve()
    if (output / 'inspection.db').exists():
        parser.error('输出目录已有数据库，请为本次演示选择新目录')
    station = Station(str(Path(args.binary).resolve()), backend=args.backend, points=5, directory=output)
    try:
        station.begin('DEMO-' + args.result.upper())
        for index in range(5 if args.result != 'aborted' else 2):
            candidate = station.point(250 if args.result == 'fail' and index == 4 else 150)
            print(f'第{index + 1}点已独立确认并保存，候选编号：{candidate}')
        if args.result == 'aborted':
            station.service.send('cancel')
            station.service.wait('inspection_aborted')
            station.service.wait('command_done', predicate=lambda x: x['opcode'] == 'ABORT')
        else:
            station.service.wait('inspection_completed')
            ack = station.service.wait('command_done', predicate=lambda x: x['opcode'] == 'APPLY_RESULT')
            assert ack['status'] == 'ACKNOWLEDGED'
        records = station.records()
        station.service.send('outbox')
        events = station.service.wait('outbox')['records']
        summary = dict(mode='SOFTWARE_SIMULATION', records=records, outbox=events,
                       cloud_status='NOT_CONFIGURED', hardware_verified=False)
        (output / 'summary.json').write_text(json.dumps(summary, indent=2, ensure_ascii=False))
        print(json.dumps(summary, indent=2, ensure_ascii=False))
        station.present(False)
    finally:
        station.close()


if __name__ == '__main__':
    main()
