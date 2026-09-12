#!/usr/bin/env python3
"""Decode OptiScaler_XeFGTrace.bin without third-party dependencies."""

from __future__ import annotations

import argparse
import csv
import io
import struct
import sys
from dataclasses import dataclass


MAGIC = 0x54474658
VERSION = 1
HEADER = struct.Struct("<IIIIIIQQQ")
RECORD = struct.Struct("<QQQQQQIIIIiIII")

EVENT_NAMES = {
    1: "PresentEnter", 2: "Present1Enter", 3: "PresentInternalBypass", 4: "Present1InternalBypass",
    5: "PresentDispatchBegin", 6: "PresentDispatchEnd", 7: "DxgiPresentBegin", 8: "DxgiPresentEnd",
    9: "DxgiPresent1Begin", 10: "DxgiPresent1End", 11: "PresentMutexDecision",
    12: "PresentMutexWaitBegin", 13: "PresentMutexAcquired", 14: "PresentMutexSameThreadBypass",
    15: "PresentMutexUnlock", 16: "ResizeEnter", 17: "Resize1Enter", 18: "ResizeInternalBypass",
    19: "Resize1InternalBypass", 20: "ResizeDxgiBegin", 21: "ResizeDxgiEnd",
    22: "Resize1DxgiBegin", 23: "Resize1DxgiEnd", 24: "FenceSignalBegin", 25: "FenceSignalEnd",
    26: "FenceWaitBegin", 27: "FenceWaitEnd", 28: "FenceRecreateBegin", 29: "FenceRecreateEnd",
    30: "SwapchainCreateBegin", 31: "SwapchainCreateEnd", 32: "SwapchainCreate1Begin",
    33: "SwapchainCreate1End", 34: "SwapchainReleaseEnter", 35: "SwapchainFinalProxyRelease",
    36: "SwapchainReleaseLifecycleBegin", 37: "SwapchainReleaseLifecycleEnd",
    38: "XeFGCreateContextResult", 39: "XeFGInitSwapchainResult", 40: "XeFGGetSwapchainPtrResult",
    41: "XeFGDestroyBegin", 42: "XeFGDestroyResult", 43: "XeFGSetEnabledResult",
    44: "XeFGTagFrameConstantsResult", 45: "XeFGSetPresentIdResult", 46: "XeFGTagFrameResourceResult",
    47: "XeFGSetNumInterpolatedFramesResult", 48: "XeFGSetUiCompositionResult",
}

FLAG_NAMES = ((1, "skipResize"), (2, "skipResize1"), (4, "skipPresent"), (8, "skipPresent1"),
              (16, "xeFGActive"), (32, "xeFGPaused"))


@dataclass(frozen=True)
class Trace:
    header: tuple[int, ...]
    records: list[tuple[int, ...]]


def parse_bytes(data: bytes) -> Trace:
    if len(data) < HEADER.size:
        raise ValueError("trace is shorter than its header")
    header = HEADER.unpack_from(data)
    magic, version, header_size, record_size, capacity, _pid, _freq, _start, _latest = header
    if magic != MAGIC:
        raise ValueError(f"invalid magic 0x{magic:08x}")
    if version != VERSION:
        raise ValueError(f"unsupported version {version}")
    if header_size != HEADER.size:
        raise ValueError(f"invalid header size {header_size}")
    if record_size != RECORD.size:
        raise ValueError(f"unsupported record size {record_size}")
    required = header_size + capacity * record_size
    if capacity == 0 or len(data) < required:
        raise ValueError("trace is shorter than the declared ring buffer")

    records = []
    for slot in range(capacity):
        record = RECORD.unpack_from(data, header_size + slot * record_size)
        sequence = record[0]
        if sequence and sequence % capacity == slot:
            records.append(record)
    records.sort(key=lambda record: record[0])
    return Trace(header, records)


def flags_text(flags: int) -> str:
    names = [name for bit, name in FLAG_NAMES if flags & bit]
    return "|".join(names) if names else "-"


def rows(trace: Trace):
    _magic, _version, _header_size, _record_size, _capacity, _pid, frequency, start, _latest = trace.header
    for record in trace.records:
        sequence, qpc, swapchain, obj, aux_pointer, fence, thread, event, owner, owner_thread, result, flags, aux0, aux1 = record
        delta_ms = ((qpc - start) * 1000.0 / frequency) if frequency else 0.0
        yield {
            "seq": sequence,
            "qpc_delta_ms": f"{delta_ms:.3f}",
            "thread": thread,
            "event": EVENT_NAMES.get(event, f"Unknown({event})"),
            "swapchain": f"0x{swapchain:x}",
            "object_or_context": f"0x{obj:x}",
            "aux_pointer": f"0x{aux_pointer:x}",
            "mutex_owner": owner,
            "mutex_owner_thread": owner_thread,
            "fence": fence,
            "result": result,
            "flags": flags_text(flags),
            "aux0": aux0,
            "aux1": aux1,
        }


def write_output(trace: Trace, output_format: str) -> None:
    data = list(rows(trace))
    fields = ["seq", "qpc_delta_ms", "thread", "event", "swapchain", "object_or_context", "aux_pointer",
              "mutex_owner", "mutex_owner_thread", "fence", "result", "flags", "aux0", "aux1"]
    if output_format == "csv":
        writer = csv.DictWriter(sys.stdout, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        writer.writerows(data)
        return
    if output_format == "tsv":
        writer = csv.DictWriter(sys.stdout, fieldnames=fields, delimiter="\t", lineterminator="\n")
        writer.writeheader()
        writer.writerows(data)
        return
    for item in data:
        print(" ".join(f"{key}={value}" for key, value in item.items()))


def self_test() -> None:
    header = HEADER.pack(MAGIC, VERSION, HEADER.size, RECORD.size, 4, 1, 1000, 100, 3)
    records = bytearray(RECORD.size * 4)
    for sequence, event in ((1, 1), (2, 16), (3, 43)):
        RECORD.pack_into(records, (sequence % 4) * RECORD.size, sequence, 100 + sequence, 0x10, 0x20, 0x30,
                         sequence, 42, event, 2, 99, -7 if sequence == 3 else 0, 4, 0, sequence)
    trace = parse_bytes(header + records)
    assert [record[0] for record in trace.records] == [1, 2, 3]
    assert list(rows(trace))[2]["result"] == -7
    print("self-test: PASS")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("trace", nargs="?", help="path to OptiScaler_XeFGTrace.bin")
    parser.add_argument("--format", choices=("text", "csv", "tsv"), default="text")
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
        return 0
    if not args.trace:
        parser.error("trace is required unless --self-test is used")
    with open(args.trace, "rb") as stream:
        trace = parse_bytes(stream.read())
    write_output(trace, args.format)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
