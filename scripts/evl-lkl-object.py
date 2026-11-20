#!/usr/bin/env python3
"""Build the complete LKL core for a native x86-64 kernel and isolate it."""
import argparse
import filecmp
import json
import os
from pathlib import Path
import re
import shlex
import subprocess


def run(command, **kwargs):
    return subprocess.run(command, check=True, text=True, **kwargs)


def source_view(source, view):
    """Hide in-tree generated files without cleaning the user's LKL tree."""
    split = {"", "include", "arch", "arch/lkl", "arch/lkl/include"}
    omitted = {"include/config", "include/generated", "arch/lkl/include/generated"}

    def populate(relative):
        directory = view / relative
        directory.mkdir(parents=True, exist_ok=True)
        for entry in (source / relative).iterdir():
            rel = entry.relative_to(source).as_posix()
            if rel in omitted or entry.name == ".git":
                continue
            if not relative and (entry.name.startswith(".config") or
                                 entry.name.endswith((".o", ".a", ".cmd")) or
                                 entry.name in {"vmlinux", "System.map", "Module.symvers"}):
                continue
            target = view / rel
            if rel in split:
                populate(rel)
            elif not target.exists() and not target.is_symlink():
                target.symlink_to(entry)
            elif target.is_symlink() and target.resolve() != entry.resolve():
                raise RuntimeError(f"unexpected source view link: {target}")

    populate("")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--build", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cc", default="gcc")
    parser.add_argument("--objcopy", default="objcopy")
    parser.add_argument("--nm", default="nm")
    parser.add_argument("--jobs", type=int, default=8)
    parser.add_argument("--retpoline", action="store_true")
    parser.add_argument("--rethunk", action="store_true")
    parser.add_argument("--ibt", action="store_true")
    parser.add_argument("--runtime", action="store_true")
    parser.add_argument("--clock", action="store_true")
    args = parser.parse_args()
    source = args.source.resolve(strict=True)
    build = args.build.resolve()
    output = args.output.resolve()
    build.mkdir(parents=True, exist_ok=True)
    view = build / "source-view"
    work = build / "kernel"
    work.mkdir(exist_ok=True)
    source_view(source, view)

    # Do not pass the outer kernel's paths/configuration/make state inward.
    env = {key: value for key, value in os.environ.items()
           if not key.startswith(("KBUILD_", "KCONFIG_", "MAKE")) and key not in {
               "MFLAGS", "srctree", "objtree", "sub_make_done",
               "need-sub-make", "no-print-directory", "building_out_of_srctree",
               "ARCH", "SRCARCH", "SUBARCH", "VPATH", "KERNELRELEASE",
               "KERNELVERSION", "KCFLAGS", "KAFLAGS", "CFLAGS_MODULE",
               "CFLAGS_KERNEL", "RUSTFLAGS_KERNEL", "LDFLAGS_vmlinux"}}
    flags = ["-m64", "-mcmodel=kernel", "-mno-red-zone", "-fno-pic", "-fno-PIE",
             "-mno-sse", "-mno-sse2", "-mno-mmx", "-mno-80387",
             "-mpreferred-stack-boundary=3", "-mincoming-stack-boundary=3",
             "-fno-stack-protector", "-fno-jump-tables",
             "-fcf-protection=branch" if args.ibt else "-fcf-protection=none"]
    if args.retpoline:
        flags += ["-mindirect-branch=thunk-extern", "-mindirect-branch-register"]
    if args.rethunk:
        flags += ["-mfunction-return=thunk-extern"]
    make = ["make", "-rR", "-C", str(view), f"O={work}", "ARCH=lkl", "LKL_KABI=y",
            f"CC={args.cc}", "KCFLAGS=" + " ".join(flags)]
    seed = args.config.read_bytes()
    stamp = build / "source.config"
    if not stamp.exists() or stamp.read_bytes() != seed or not (work / ".config").exists():
        (work / ".config").write_bytes(seed)
        configure = [str(source / "scripts/config"), "--file", str(work / ".config"),
                     "--enable", "DEBUG_INFO_NONE", "--disable", "DEBUG_INFO_DWARF4",
                     "--disable", "DEBUG_INFO_DWARF5", "--disable", "LKL_FUZZING",
                     "--disable", "LKL_LINE_COV", "--disable", "KASAN",
                     "--disable", "GCOV_KERNEL", "--disable", "KCOV"]
        run(configure, env=env)
        stamp.write_bytes(seed)
    if args.clock and not args.runtime:
        raise RuntimeError("independent LKL clock requires the embedded runtime")
    run([str(source / "scripts/config"), "--file", str(work / ".config"),
         "--enable" if args.clock else "--disable", "LKL_X86_PVCLOCK"], env=env)
    run(make + ["olddefconfig"], env=env)
    config = (work / ".config").read_text()
    if 'CONFIG_OUTPUT_FORMAT="elf64-x86-64"' not in config or "CONFIG_64BIT=y" not in config:
        raise RuntimeError("embedded LKL currently requires an elf64-x86-64 configuration")
    if args.runtime and any(option + "=y" in config for option in ("CONFIG_MMU", "CONFIG_SMP", "CONFIG_KASAN")):
        raise RuntimeError("Linux host operations currently require NOMMU, UP LKL without KASAN")
    # The top-level all target builds lkl.o and its generated metadata;
    # requesting a bare .o is treated as a single-file build by kbuild.
    run(make + [f"-j{max(1, args.jobs)}", "all"], env=env)
    raw = work / "lkl.o"
    nm = shlex.split(args.nm)
    definitions = run(nm + ["-P", "--defined-only", str(raw)], capture_output=True).stdout
    symbols = {}
    for line in definitions.splitlines():
        fields = line.split()
        if len(fields) >= 2:
            symbols[fields[0]] = fields[1]
    public = {name for name, kind in symbols.items() if kind.isupper() and name.startswith("lkl_")}
    required = {"lkl_init", "lkl_start_kernel", "lkl_syscall"}
    required.update("lkl_evl_" + name for name in (
        "strcmp", "strncmp", "strncpy", "strsep", "simple_strtol", "hashlen_string",
        "rb_insert_color", "rb_erase", "rb_next", "rb_replace_node", "find_first_bit",
        "find_first_zero_bit", "int_sqrt", "ns_to_timespec64"))
    if args.clock:
        required |= {"lkl_ktime_get_mono_fast_ns", "lkl_ktime_get_raw_fast_ns",
                     "lkl_ktime_get_real_fast_ns", "lkl_clock_read_counter",
                     "lkl_clock_is_ready"}
    if required - public:
        raise RuntimeError(f"missing LKL entry points: {sorted(required - public)}")
    if len(symbols) < 1000:
        raise RuntimeError("input is not the expected complete LKL core")
    undefined = {line.split()[-1] for line in
                 run(nm + ["-u", str(raw)], capture_output=True).stdout.splitlines() if line.split()}
    unexpected = {name for name in undefined if name not in {
        "lkl_bug", "lkl_printf", "__x86_return_thunk"} and
        not name.startswith("__x86_indirect_thunk_")}
    if unexpected:
        raise RuntimeError(f"unexpected embedded LKL dependencies: {sorted(unexpected)}")

    mapping = build / "symbols.map"
    # GCC cold/cloned fragments must retain their public parent's spelling:
    # objtool resolves foo.cold to foo when analyzing the linked kernel.
    public_fragments = {name for name in symbols
                        if any(name.startswith(entry + ".") for entry in public)}
    mapping.write_text("".join(f"{name} __evl_lkl_{name}\n"
                               for name in sorted(symbols)
                               if name not in public and name not in public_fragments))
    sections = run(["readelf", "-SW", str(raw)], capture_output=True).stdout
    command = shlex.split(args.objcopy) + ["--strip-debug", "--redefine-syms=" + str(mapping)]
    renamed = {}
    for line in sections.splitlines():
        match = re.match(r"\s*\[\s*\d+\]\s+(\S+)\s+(\S+)\s+\S+\s+\S+\s+\S+\s+\S+\s+(\S+)", line)
        if not match:
            continue
        name, kind, attributes = match.groups()
        if name in renamed or kind not in {"PROGBITS", "NOBITS", "NOTE"} or name == ".comment":
            continue
        if "A" not in attributes and not name.startswith("__"):
            continue
        category = "text" if "X" in attributes else "bss" if kind == "NOBITS" else "data" if "W" in attributes else "rodata"
        # Use ordinary permanent sections understood by the native linker.
        # In particular, inner initcalls/parameters must never retain their
        # native registration section names in the outer kernel.
        destination = f".{category}"
        renamed[name] = destination
        note_flags = ",alloc,load,readonly,data" if kind == "NOTE" else ""
        command += ["--rename-section", f"{name}={destination}{note_flags}"]
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(".o.new")
    run(command + [str(raw), str(temporary)])
    if output.exists() and filecmp.cmp(output, temporary, shallow=False):
        temporary.unlink()
    else:
        temporary.replace(output)
    report = {"source": str(source), "object": str(output), "symbol_count": len(symbols),
              "public": sorted(public), "undefined": sorted(undefined), "sections": renamed,
              "compiler_flags": flags, "runtime_configured": args.runtime,
              "runtime_started": False, "independent_clock": args.clock}
    (build / "manifest.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Embedded complete LKL: {len(symbols)} symbols, {output.stat().st_size} bytes", flush=True)


if __name__ == "__main__":
    main()
