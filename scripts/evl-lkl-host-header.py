#!/usr/bin/env python3
"""Import the host ABI and selected syscall numbers from this LKL source."""
import re
import sys
from pathlib import Path

source = Path(sys.argv[1])
abi = (source / "arch/lkl/include/uapi/asm/host_ops.h").read_text()
unistd = (source / "include/uapi/asm-generic/unistd.h").read_text()
print("/* Generated from LKL; do not include its internal Linux headers. */")
print(abi)
for name in ("getpid", "nanosleep", "clock_gettime", "openat", "close", "read", "write", "unlinkat"):
    match = re.search(r"^#define __NR_" + name + r"\s+(\d+)\s*$", unistd, re.M)
    if not match:
        raise SystemExit(f"missing generic LKL syscall number: {name}")
    print(f"#define EVL_LKL_NR_{name} {match[1]}")
print("long lkl_syscall(long nr, long *args);")
print("long lkl_sys_halt(void);")
