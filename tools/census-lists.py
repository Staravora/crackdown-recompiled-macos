#!/usr/bin/env python3
"""Summarise census-<label>.json and .lists from a --crackdown_d3d_census benchmark.

The game only copies out the command lists of the precompiled command buffers; they are parsed here, because following
them live froze the game. Usage: census-lists.py <logs folder> <label>
"""
import collections
import json
import struct
import sys

RECORD = struct.Struct('<6I')
NAMES = {0x22: 'DRAW_INDX', 0x27: 'IM_LOAD', 0x2D: 'SET_CONSTANT', 0x2F: 'LOAD_ALU_CONSTANT', 0x36: 'DRAW_INDX_2',
         0x3C: 'WAIT_REG_MEM', 0x3F: 'INDIRECT_BUFFER', 0x60: 'SET_BIN_MASK_LO', 0x61: 'SET_BIN_MASK_HI'}


def main():
    logs, label = sys.argv[1:3]
    report = json.load(open(f'{logs}/census-{label}.json'))
    per_frame = report['per_frame']
    frames = report['frames']
    data = open(f'{logs}/census-{label}.lists', 'rb').read()

    packets = collections.Counter()
    registers = collections.Counter()
    draws_per_run = collections.Counter()
    runs = draws = lists = truncated = unknown = 0
    offset = 0
    while offset + RECORD.size <= len(data):
        _, _, _, list_runs, total, copied = RECORD.unpack_from(data, offset)
        offset += RECORD.size
        words = struct.unpack_from(f'>{copied}I', data, offset)
        offset += copied * 4
        lists += 1
        truncated += copied < total
        runs += list_runs
        list_draws = at = 0
        while at < len(words):
            header = words[at]
            kind = header >> 30
            count = ((header >> 16) & 0x3FFF) + 1
            if kind == 0:
                registers[(header & 0x7FFF) >> 8] += count * list_runs
                at += 1 + count
            elif kind == 3:
                op = (header >> 8) & 0x7F
                packets[op] += list_runs
                list_draws += op in (0x22, 0x36)
                at += 1 + count
            elif kind == 2:
                at += 1
            else:
                unknown += 1
                at += 3
        draws += list_draws * list_runs
        draws_per_run[list_draws] += list_runs

    direct = per_frame['draws']['median']
    in_buffers = draws / frames
    print(f'{label}: {frames} frames, {lists} lists, {len(data) / 1e6:.1f} MB, truncated {truncated}, '
          f'unknown packets {unknown}, buffers changing {report["command_buffer_contents"]["buffers_changing"]}')
    print(f'unique draws per frame: {direct + in_buffers:.0f} ({direct} direct, {in_buffers:.0f} in {runs / frames:.0f} '
          f'buffer runs)')
    print('packets per frame:', [(NAMES.get(op, hex(op)), round(n / frames)) for op, n in packets.most_common()])
    print('register pages per frame:', [(hex(p), round(n / frames)) for p, n in registers.most_common(8)])
    print('share of runs by draws in the buffer:',
          [(n, f'{100 * r / runs:.0f}%') for n, r in sorted(draws_per_run.items())[:6]])


if __name__ == '__main__':
    main()
