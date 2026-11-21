#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# 设备档案导出 + lint（#1749 M0）：
#   pull ：adb 拉 files/devprofile/*.json → 本地目录
#   lint ：校验 s2a-device-profile-v1 M0 段（键白名单、三态、必填、digest 格式）
# 用： device_profile.py pull --package <pkg> --out <dir>
#     device_profile.py lint <dir-or-file>
import argparse
import json
import subprocess
import sys
from pathlib import Path

# M0 段键白名单（probes / bench 段属后续里程碑，出现即 lint 报超范围键）
TOP_KEYS = {"schema", "platform", "api", "identity", "model", "vulkan", "system", "factsDigest", "collectMs"}
API_KEYS = {"kind", "version"}
IDENTITY_KEYS = {"vendorId", "deviceId", "driverVersion", "driverName", "deviceName"}
MODEL_KEYS = {"manufacturer", "model", "socModel", "androidRelease"}
VK_KEYS = {"extensions", "enumFailed", "features", "properties", "memory", "queues", "timeDomains", "formats"}
SYS_KEYS = {"sdk", "pageSize", "configuredNcpu", "affinityNcpu", "cpu",
            "topologyComplete", "topologyOk", "kgsl", "adpf",
            "vsyncApi33", "vsyncApi29", "vsyncApi24", "powerRails"}
FEAT_KEYS = {"textureCompressionBC", "geometryShader", "multiDrawIndirect",
             "drawIndirectFirstInstance", "indexTypeUint8", "samplerFilterMinmax", "shaderInt16"}
SYS_TRISTATE = {"topologyComplete", "topologyOk", "kgsl", "adpf",
                "vsyncApi33", "vsyncApi29", "vsyncApi24", "powerRails"}

def lint_file(path):
    errs = []
    try:
        p = json.loads(Path(path).read_text())
    except Exception as e:
        return [f"{path}: JSON 解析失败 {e}"]
    def keys(section, allow, name):
        if not isinstance(section, dict):
            errs.append(f"{path}: {name} 缺/非对象"); return
        extra = set(section) - allow
        if extra: errs.append(f"{path}: {name} 超范围键 {sorted(extra)}")
    keys(p, TOP_KEYS, "顶层")
    if p.get("schema") != "s2a-device-profile-v1":
        errs.append(f"{path}: schema={p.get('schema')!r}")
    keys(p.get("api"), API_KEYS, "api")
    keys(p.get("identity"), IDENTITY_KEYS, "identity")
    keys(p.get("model"), MODEL_KEYS, "model")
    vk = p.get("vulkan")
    keys(vk, VK_KEYS, "vulkan")
    if isinstance(vk, dict):
        keys(vk.get("features"), FEAT_KEYS, "vulkan.features")
        fmt = vk.get("formats")
        if not isinstance(fmt, dict): errs.append(f"{path}: vulkan.formats 缺/非对象")
        elif not all(str(k).isdigit() and isinstance(v.get("optimal"), str) for k, v in fmt.items()):
            errs.append(f"{path}: vulkan.formats 项须以 NVN 格式号为键、含 optimal/buffer hex 串")
    s = p.get("system")
    keys(s, SYS_KEYS, "system")
    if isinstance(s, dict):
        for k in SYS_TRISTATE & set(s):
            if s[k] not in (-1, 0, 1): errs.append(f"{path}: system.{k}={s[k]!r} 非三态")
    d = p.get("factsDigest", "")
    if not (isinstance(d, str) and d.startswith("sha256-") and len(d) == 71):
        errs.append(f"{path}: factsDigest 格式 {d!r}（应为 sha256-<64 hex>）")
    return errs

def pull(package, out_dir, device=None):
    remote = f"/sdcard/Android/data/{package}/files/devprofile"
    adb = ["adb"] + (["-s", device] if device else [])
    r = subprocess.run(adb + ["shell", "ls", remote], capture_output=True, text=True)
    if r.returncode: raise SystemExit(f"adb ls {remote}: {r.stderr.strip()}")
    names = [n.strip() for n in r.stdout.splitlines() if n.strip().endswith(".json")]
    out = Path(out_dir); out.mkdir(parents=True, exist_ok=True)
    got = 0
    for n in names:
        r = subprocess.run(adb + ["pull", f"{remote}/{n}", str(out / n)], capture_output=True, text=True)
        if r.returncode == 0: got += 1
        else: print(f"拉 {n} 失败: {r.stderr.strip()}", file=sys.stderr)
    print(f"拉取 {got}/{len(names)} 份档案 → {out}")
    return got

def main():
    ap = argparse.ArgumentParser(description="s2a 设备档案导出/lint")
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("pull"); p.add_argument("--package", required=True)
    p.add_argument("--out", required=True); p.add_argument("--device")
    l = sub.add_parser("lint"); l.add_argument("path")
    a = ap.parse_args()
    if a.cmd == "pull":
        pull(a.package, a.out, a.device)
        sys.exit(0)
    targets = [Path(a.path)] if Path(a.path).is_file() else sorted(Path(a.path).glob("*.json"))
    if not targets: sys.exit(f"{a.path} 下没有 JSON")
    bad = 0
    for t in targets:
        e = lint_file(t)
        bad += len(e)
        for x in e: print(x)
        if not e: print(f"{t}: 通过")
    sys.exit(1 if bad else 0)

if __name__ == "__main__":
    main()
