#!/usr/bin/env python3
"""Dump every staged-Qwen program's I/O port layout as the macOS e5rt runtime sees it.

Run on macOS against the staged export (tools/staged-qwen/staged_qwen_manifest.py):
  python3 e5rt_port_layout.py --export /tmp/qwen38-staged-jwm1 --out port-layout.json

Per program it compiles prog_NNN/model.mil through e5rt (ANE-only mask 0x4, same flags
as ANEForge's compile_and_build_op), builds the precompiled op, and records for every
manifest port both the function-level tensor descriptor and the op-level io-port tensor
descriptor: rank, per-dimension length and stride, byte size, element size.
"""
import argparse, ctypes, json, os, shutil, tempfile

ESPRESSO = "/System/Library/PrivateFrameworks/Espresso.framework/Espresso"
E = ctypes.CDLL(ESPRESSO)
vp = ctypes.c_void_p
u64 = ctypes.c_uint64


def fn(name, *argtypes):
    f = getattr(E, name)
    f.restype = ctypes.c_int64
    f.argtypes = list(argtypes)
    return f


cfg_create = fn("e5rt_e5_compiler_config_options_create", ctypes.POINTER(vp))
cfg_cache = fn("e5rt_e5_compiler_config_options_set_cache_bundle_location", vp, ctypes.c_char_p)
comp_create = fn("e5rt_e5_compiler_create_with_config", ctypes.POINTER(vp), vp)
opt_create = fn("e5rt_e5_compiler_options_create", ctypes.POINTER(vp))
opt_mask = fn("e5rt_e5_compiler_options_set_compute_device_types_mask", vp, u64)
opt_force = fn("e5rt_e5_compiler_options_set_force_recompilation", vp, ctypes.c_int)
opt_seg = fn("e5rt_e5_compiler_options_set_segmenter", vp, ctypes.c_char_p)
compile_ = fn("e5rt_e5_compiler_compile", vp, ctypes.c_char_p, vp, ctypes.POINTER(vp))
lib_fn = fn("e5rt_program_library_retain_program_function", vp, ctypes.c_char_p, ctypes.POINTER(vp))
opo_create = fn("e5rt_precompiled_compute_op_create_options_create_with_program_function", ctypes.POINTER(vp), vp)
opo_name = fn("e5rt_precompiled_compute_op_create_options_set_operation_name", vp, ctypes.c_char_p)
opo_inter = fn("e5rt_precompiled_compute_op_create_options_set_allocate_intermediate_buffers", vp, ctypes.c_int)
op_create = fn("e5rt_execution_stream_operation_create_precompiled_compute_operation_with_options", ctypes.POINTER(vp), vp)
op_in = fn("e5rt_execution_stream_operation_retain_input_port", vp, ctypes.c_char_p, ctypes.POINTER(vp))
op_out = fn("e5rt_execution_stream_operation_retain_output_port", vp, ctypes.c_char_p, ctypes.POINTER(vp))
f_in_desc = fn("e5rt_program_function_retain_input_tensor_desc", vp, ctypes.c_char_p, ctypes.POINTER(vp))
f_out_desc = fn("e5rt_program_function_retain_output_tensor_desc", vp, ctypes.c_char_p, ctypes.POINTER(vp))
port_desc = fn("e5rt_io_port_retain_tensor_desc", vp, ctypes.POINTER(vp))
port_is_tensor = fn("e5rt_io_port_is_tensor", vp, ctypes.POINTER(ctypes.c_bool))
d_rank = fn("e5rt_tensor_desc_get_rank", vp, ctypes.POINTER(u64))
d_len = fn("e5rt_tensor_desc_get_dimension_length", vp, u64, ctypes.POINTER(u64))
d_stride = fn("e5rt_tensor_desc_get_dimension_stride", vp, u64, ctypes.POINTER(u64))
d_size = fn("e5rt_tensor_desc_get_size", vp, ctypes.POINTER(u64))
d_dtype = fn("e5rt_tensor_desc_retain_dtype", vp, ctypes.POINTER(vp))
dt_esize = fn("e5rt_tensor_desc_dtype_get_element_size", vp, ctypes.POINTER(u64))


def check(err, what):
    if err:
        raise RuntimeError(f"{what}: e5rt err={err}")


def describe(desc):
    rank = u64(0)
    check(d_rank(desc, ctypes.byref(rank)), "get_rank")
    dims, strides = [], []
    for i in range(rank.value):
        n, s = u64(0), u64(0)
        check(d_len(desc, i, ctypes.byref(n)), "get_dimension_length")
        check(d_stride(desc, i, ctypes.byref(s)), "get_dimension_stride")
        dims.append(n.value)
        strides.append(s.value)
    size = u64(0)
    check(d_size(desc, ctypes.byref(size)), "get_size")
    dt, es = vp(), u64(0)
    check(d_dtype(desc, ctypes.byref(dt)), "retain_dtype")
    check(dt_esize(dt, ctypes.byref(es)), "dtype element size")
    return {"shape": dims, "strides": strides, "size": size.value, "element_size": es.value}


def compile_program(mil, cache):
    cfg, comp, opts, lib = vp(), vp(), vp(), vp()
    check(cfg_create(ctypes.byref(cfg)), "config create")
    cfg_cache(cfg, cache.encode())
    check(comp_create(ctypes.byref(comp), cfg), "compiler create")
    check(opt_create(ctypes.byref(opts)), "options create")
    opt_mask(opts, 0x4)
    opt_force(opts, 1)
    opt_seg(opts, b"graph")
    check(compile_(comp, mil.encode(), opts, ctypes.byref(lib)), f"compile {mil}")
    f = vp()
    check(lib_fn(lib, b"main", ctypes.byref(f)), "retain function")
    oo, op = vp(), vp()
    check(opo_create(ctypes.byref(oo), f), "op options")
    opo_name(oo, b"main")
    opo_inter(oo, 1)
    check(op_create(ctypes.byref(op), oo), "op create")
    return lib, f, op


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--export", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--programs", default="", help="comma list of program indices (default all)")
    a = ap.parse_args()
    man = json.load(open(os.path.join(a.export, "manifest.json")))
    todo = [int(x) for x in a.programs.split(",")] if a.programs else range(man["n_programs"])
    out = {"export": os.path.abspath(a.export), "programs": {}}
    scratch = tempfile.mkdtemp(prefix="e5rt-port-layout-")
    try:
        for ci in todo:
            pr = man["programs"][ci]
            mil = os.path.join(a.export, f"prog_{ci:03d}", "model.mil")
            _lib, f, op = compile_program(mil, os.path.join(scratch, f"prog_{ci:03d}"))
            ports = [("in", s["port"]) for s in pr["srcs"]] + \
                    [("out", d["port"]) for d in pr["dsts"]] + \
                    [("out", s["out_port"]) for s in pr["states"]]
            rows = []
            for kind, name in ports:
                fd, port, pd = vp(), vp(), vp()
                check((f_in_desc if kind == "in" else f_out_desc)(f, name.encode(), ctypes.byref(fd)),
                      f"function {kind} desc {name}")
                check((op_in if kind == "in" else op_out)(op, name.encode(), ctypes.byref(port)),
                      f"op {kind} port {name}")
                is_t = ctypes.c_bool(False)
                check(port_is_tensor(port, ctypes.byref(is_t)), "is_tensor")
                check(port_desc(port, ctypes.byref(pd)), f"port desc {name}")
                rows.append({"dir": kind, "port": name, "is_tensor": bool(is_t.value),
                             "function_desc": describe(fd), "port_desc": describe(pd)})
            out["programs"][str(ci)] = rows
            print(f"prog_{ci:03d}: " + "; ".join(
                f"{r['port']} {r['port_desc']['shape']}/{r['port_desc']['strides']}" for r in rows), flush=True)
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    json.dump(out, open(a.out, "w"), indent=1)
    print(f"wrote {a.out}")


if __name__ == "__main__":
    main()
