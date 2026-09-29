#!/usr/bin/env python3
"""Validate an upstream arithmetic/history plan and emit signal-API construction.

The upstream partition and member order are retained verbatim. This deliberately
can emit a one-sample state-transition oracle or stateful bounded-block kernels.
Internal delays stay in LLVM instances; only cross-kernel history is host-owned.
Unsupported primitives, integer state, incomplete references and invalid order
are errors; no fallback silently swaps in a handwritten or monolithic renderer.
"""
from __future__ import annotations
import hashlib
import json
import math
from typing import Any

SCHEMA = "faust-plan-llvm-v1"
FAUST_COMMIT = "3d4baa164c0dd31b7d147617ed84a626495148dd"
ARITHMETIC = {"add": "sigAdd", "sub": "sigSub", "mul": "sigMul", "div": "sigDiv"}


def integer(x: Any, label: str, lo: int, hi: int) -> int:
    if type(x) is not int or not lo <= x <= hi:
        raise ValueError(f"{label}: expected integer in [{lo}, {hi}]")
    return x


def dependencies(expr: dict, members: int, inputs: int, depth: int = 0) -> set[tuple]:
    if depth > 1024 or not isinstance(expr, dict):
        raise ValueError("invalid expression or excessive depth")
    op = expr.get("op")
    shapes = {"int": {"op", "value"}, "real": {"op", "value"},
              "input": {"op", "index"}, "ref": {"op", "member"},
              "history": {"op", "member", "delay"}, "float": {"op", "arg"}}
    shapes.update({name: {"op", "lhs", "rhs"} for name in ARITHMETIC})
    if op not in shapes or set(expr) != shapes[op]:
        raise ValueError(f"unsupported expression or unexpected fields: {op!r}")
    if op == "int":
        integer(expr["value"], "integer literal", -(2**31), 2**31 - 1)
        return set()
    if op == "real":
        v = expr["value"]
        if type(v) not in (int, float) or not math.isfinite(v):
            raise ValueError("nonfinite real literal")
        return set()
    if op == "input":
        return {("input", integer(expr["index"], "audio input", 0, inputs - 1))}
    if op in ("ref", "history"):
        m = integer(expr["member"], "member reference", 0, members - 1)
        if op == "ref":
            return {("ref", m)}
        return {("history", m, integer(expr["delay"], "history delay", 1, 4096))}
    if op == "float":
        return dependencies(expr["arg"], members, inputs, depth + 1)
    return (dependencies(expr["lhs"], members, inputs, depth + 1) |
            dependencies(expr["rhs"], members, inputs, depth + 1))


def validate(plan: dict) -> dict:
    if plan.get("schema") != SCHEMA or plan.get("faust_commit") != FAUST_COMMIT:
        raise ValueError("schema or pinned compiler identity mismatch")
    if plan.get("stage") != "post-fusion-before-emission":
        raise ValueError("not a post-planning export")
    if type(plan.get("fusion_enabled")) is not bool:
        raise ValueError("missing explicit fusion setting")
    if plan.get("output_tail_outside_partition") is not True:
        raise ValueError("output tail contract missing")
    ni = integer(plan.get("inputs"), "input count", 1, 64)
    no = integer(plan.get("outputs"), "output count", 1, 64)
    chunk = integer(plan.get("chunk_size"), "upstream chunk size", 1, 4096)
    ms, bs = plan.get("members"), plan.get("blocks")
    if not isinstance(ms, list) or not isinstance(bs, list) or not ms or not bs:
        raise ValueError("empty or missing materialized partition")
    if len(ms) > 4096 or len(bs) > 1024:
        raise ValueError("research plan size bound exceeded")
    n, nb = len(ms), len(bs)
    ownership: dict[int, int] = {}
    ready: set[int] = set()
    dep_sets = []
    for i, m in enumerate(ms):
        if m["id"] != i:
            raise ValueError("member IDs are not dense in compiler order")
        integer(m["block"], "block ownership", 0, nb - 1)
        integer(m["max_delay"], "history capacity", 0, 4096)
        if m.get("integer") is not False:
            raise ValueError("integer materialized state is not representable by this float ABI")
        ds = dependencies(m["expression"], n, ni)
        dep_sets.append(ds)
        current = {d[1] for d in ds if d[0] == "ref"}
        constrained = current | {d[1] for d in ds if d[0] == "history" and d[2] < chunk}
        if m["refs0"] != sorted(current) or m["refs"] != sorted(constrained):
            raise ValueError(f"member {i}: exported AST disagrees with upstream dependency sets")
        for d in ds:
            if d[0] == "history" and d[2] > ms[d[1]]["max_delay"]:
                raise ValueError("delay exceeds producer-owned history")
    for b, block in enumerate(bs):
        if block["id"] != b or not block["members"]:
            raise ValueError("invalid block ID or empty block")
        members = block["members"]
        if len(set(members)) != len(members):
            raise ValueError("duplicate member in block")
        for m in members:
            integer(m, "block member", 0, n - 1)
            if m in ownership or ms[m]["block"] != b:
                raise ValueError("member has multiple or contradictory owners")
            ownership[m] = b
            current = {d[1] for d in dep_sets[m] if d[0] == "ref"}
            if not current <= ready:
                raise ValueError("compiler order is not executable for current-value dependencies")
            ready.add(m)
        refs = {r for m in members for r in ms[m]["refs"] if ms[r]["block"] != b}
        if block["inputs"] != sorted(refs):
            raise ValueError("upstream block input mismatch")
        deps = {ms[r]["block"] for r in refs}
        if block["dependencies"] != sorted(deps) or any(d >= b for d in deps):
            raise ValueError("upstream block dependency/order mismatch")
    if ready != set(range(n)):
        raise ValueError("partition omitted materialized members")
    outputs = plan.get("output_expressions")
    if not isinstance(outputs, list) or len(outputs) != no:
        raise ValueError("missing final output computation")
    for expr in outputs:
        ds = dependencies(expr, n, ni)
        for d in ds:
            if d[0] == "history" and d[2] > ms[d[1]]["max_delay"]:
                raise ValueError("output delay exceeds producer-owned history")

    # Diagnostic only: this inspects, but NEVER selects or changes, the partition.
    cross_current: set[tuple] = set()
    cross_delayed: set[tuple] = set()
    all_edges: set[tuple] = set()
    for m, ds in enumerate(dep_sets):
        for d in ds:
            if d[0] in ("ref", "history") and ms[d[1]]["block"] != ms[m]["block"]:
                edge = (ms[d[1]]["block"], ms[m]["block"])
                all_edges.add(edge)
                if d[0] == "history":
                    cross_delayed.add((*edge, d[1], m, d[2]))
                else:
                    cross_current.add((*edge, d[1], m))
    succ = {b: [] for b in range(nb)}
    for a, b in all_edges:
        succ[a].append(b)
    marks: dict[int, int] = {}
    def cycle(v: int) -> bool:
        if marks.get(v) == 1:
            return True
        if marks.get(v) == 2:
            return False
        marks[v] = 1
        if any(cycle(w) for w in succ[v]):
            return True
        marks[v] = 2
        return False
    cyclic = any(cycle(b) for b in range(nb))
    return {"blocks": nb, "members": n, "history_owners": sum(m["max_delay"] > 0 for m in ms),
            "cross_current": sorted(cross_current), "cross_delayed": sorted(cross_delayed),
            "cross_region_feedback": cyclic, "output_tail_kernels": 1,
            "upstream_chunk_size": chunk}


def compile_specs(plan: dict, stateful: bool = True) -> list[dict]:
    validate(plan)
    ms = plan["members"]
    kernels = []
    for b in plan["blocks"]:
        ids = b["members"]
        kernels.append({"members": ids, "expressions": [ms[m]["expression"] for m in ids], "tail": False})
    kernels.append({"members": [], "expressions": plan["output_expressions"], "tail": True})
    for k in kernels:
        local = set(k["members"])
        ds: set[tuple] = set()
        for expr in k["expressions"]:
            ds |= dependencies(expr, len(ms), plan["inputs"])
        k["inputs"] = sorted(d for d in ds if not
                             (d[1] in local and (d[0] == "ref" or (stateful and d[0] == "history"))))
        k["local_history"] = sorted(d for d in ds if stateful and d[0] == "history" and d[1] in local)
    return kernels


def emit_header(plan: dict, source: str, stateful: bool = True) -> str:
    if ')FAUSTSOURCE"' in source:
        raise ValueError("source contains C++ literal delimiter")
    specs = compile_specs(plan, stateful)
    capacities = [0] * len(plan["members"])
    for spec in specs:
        for d in spec["inputs"]:
            if d[0] == "history":
                capacities[d[1]] = max(capacities[d[1]], d[2])
    canonical = json.dumps(plan, sort_keys=True, separators=(",", ":"), allow_nan=False).encode()
    plan_hash = hashlib.sha256(canonical).hexdigest()
    lines = ["// Generated from the checked compiler-owned plan; do not edit.",
             f'const char* plan_identity = "{plan_hash}";',
             f'const char* execution_abi = "{("stateful-chunk-v1" if stateful else "host-transition-v1")}";',
             f"const bool plan_stateful = {str(stateful).lower()};",
             f"const int plan_chunk_size = {plan['chunk_size'] if stateful else 1};",
             f"const int plan_inputs = {plan['inputs']};", f"const int plan_outputs = {plan['outputs']};",
             "const std::vector<int> history_capacity = {" + ",".join(str(capacity) for capacity in capacities) + "};",
             "const std::vector<RegionSpec> region_specs = {"]
    kind = {"input": 0, "ref": 1, "history": 2}
    for spec in specs:
        ins = ",".join("{" + f"{kind[d[0]]},{d[1]},{d[2] if len(d) == 3 else 0}" + "}" for d in spec["inputs"])
        outs = ",".join(map(str, spec["members"]))
        lines.append("    {{" + ins + "},{" + outs + "}," + str(len(spec["expressions"])) + "," + str(spec["tail"]).lower() + "},")
    lines += ["};", "#ifndef PLAN_READER", 'const char* whole_source = R"FAUSTSOURCE(' + source + ')FAUSTSOURCE";',
              "tvec construct_region(int index) {", "    switch(index) {"]
    for idx, spec in enumerate(specs):
        lines.append(f"    case {idx}: {{")
        input_map = {d: i for i, d in enumerate(spec["inputs"])}
        local: dict[int, str] = {}
        local_index = {m: i for i, m in enumerate(spec["members"])}
        def emit(e: dict) -> str:
            op = e["op"]
            if op == "int":
                return f"sigInt({e['value']})"
            if op == "real":
                return f"sigReal({float(e['value']).hex()})"
            if op == "float":
                return f"sigFloatCast({emit(e['arg'])})"
            if op == "ref" and e["member"] in local:
                return local[e["member"]]
            if op == "history" and stateful and e["member"] in local_index:
                delayed = f"sigSelfN({local_index[e['member']]})"
                return delayed if e["delay"] == 1 else f"sigDelay({delayed},sigInt({e['delay']-1}))"
            if op in ("input", "ref", "history"):
                d = (op, e["index"] if op == "input" else e["member"])
                if op == "history":
                    d += (e["delay"],)
                return f"sigInput({input_map[d]})"
            if op in ARITHMETIC:
                return f"{ARITHMETIC[op]}({emit(e['lhs'])},{emit(e['rhs'])})"
            raise ValueError("validated expression could not be lowered")
        result = []
        for j, e in enumerate(spec["expressions"]):
            name = f"s{j}"
            expression = emit(e)
            lines.append(f"        auto {name} = {expression};")
            result.append(name)
            if not spec["tail"]:
                local[spec["members"][j]] = name
        constructor = "sigRecursionN" if spec["local_history"] else ""
        value = "{" + ",".join(result) + "}"
        lines.append("        return " + (constructor + "(" + value + ")" if constructor else value) + "; }")
    lines += ['    default: throw std::runtime_error("invalid region index");', "    }", "}", "#endif", ""]
    return "\n".join(lines)
