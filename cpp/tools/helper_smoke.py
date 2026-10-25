#!/usr/bin/env python3
"""jp-pii-helper を **別プロセスから** stdio で叩くスモーク（CI 用・docs/excel-helper.md）。

--selftest はプロセス内でフレーミングまで検査するが、本当にパイプ越しに動くかは
プロセス境界を越えて初めて分かる（バイナリモード・flush・終了コード）。ここでは
アドインがやることをそのまま順にやる: hello → init → load → sanitize → saveMapping
→ reverse → shutdown。

使い方: python3 tools/helper_smoke.py ./build/jp-pii-helper --data models
"""
import json
import os
import struct
import subprocess
import sys

# Windows のコンソール／CI ランナーは stdout が cp1252 や cp932 のことがあり、日本語の
# 検査名を print した時点で UnicodeEncodeError になる。検査結果の文字列は UTF-8 で出す。
for _stream in (sys.stdout, sys.stderr):
    if hasattr(_stream, "reconfigure"):
        _stream.reconfigure(encoding="utf-8", errors="replace")


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    exe, extra = sys.argv[1], sys.argv[2:]
    p = subprocess.Popen([exe, "--stdio", *extra], stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    fails = 0

    def check(ok: bool, what: str) -> None:
        nonlocal fails
        print(f"  [{'OK' if ok else 'NG'}] {what}")
        if not ok:
            fails += 1

    def send(obj: dict) -> None:
        data = json.dumps(obj, ensure_ascii=False).encode("utf-8")  # 生の UTF-8 を通す
        p.stdin.write(struct.pack("<I", len(data)) + data)
        p.stdin.flush()

    def recv() -> dict:
        hdr = p.stdout.read(4)
        if len(hdr) != 4:
            raise EOFError("ヘルパーが応答せずに終了しました")
        (n,) = struct.unpack("<I", hdr)
        body = p.stdout.read(n)
        if len(body) != n:
            raise EOFError("フレームが途中で切れました")
        return json.loads(body.decode("utf-8"))

    cells = [
        {"r": 1, "c": 1, "text": "氏名"}, {"r": 1, "c": 2, "text": "電話"},
        {"r": 2, "c": 1, "text": "山田太郎"}, {"r": 2, "c": 2, "text": "03-1234-5678"},
        {"r": 3, "c": 1, "text": "山田"}, {"r": 3, "c": 2, "text": "太郎"},
    ]
    # 対応表の置き場は exe から見える相対パス（WSL から Windows の exe を叩くときも通るように）
    mpath = os.path.join("build", ".helper_smoke_mapping.jsonl")

    try:
        send({"id": 1, "cmd": "hello"})
        r = recv()
        check(r.get("ok") and r.get("protocol") == 1 and r.get("id") == 1, f"hello: {r}")

        send({"id": 2, "cmd": "init"})
        r = recv()
        check(r.get("ok"), f"init: {r.get('error', 'ok')}")

        send({"id": 3, "cmd": "load", "cells": cells})
        r = recv()
        names = {(c["text"], c["entity_type"]) for c in r.get("candidates", [])}
        check(r.get("ok") and r.get("cell_count") == 6, f"load: cell_count={r.get('cell_count')}")
        check(("山田太郎", "PERSON") in names, f"候補に 山田太郎: {sorted(names)}")

        send({"id": 4, "cmd": "sanitize", "reversible": True,
              "confirmed": [{"text": "山田太郎", "type": "PERSON"}]})
        r = recv()
        changed = {(c["r"], c["c"]): c["text"] for c in r.get("cells", [])}
        check(r.get("ok") and r.get("leaks") == [], f"sanitize: leaks={r.get('leaks')}")
        check(changed.get((2, 1)) == "{{PERSON_1}}" and changed.get((2, 2)) == "{{PHONE_1}}",
              f"R2C1/R2C2 がトークン化: {changed}")
        check((3, 1) not in changed and (3, 2) not in changed, "分割セル（山田｜太郎）は変わらない")

        send({"id": 5, "cmd": "saveMapping", "path": mpath})
        r = recv()
        check(r.get("ok"), f"saveMapping: {r.get('error', 'ok')}")
        with open(mpath, encoding="utf-8") as f:
            first = f.readline().strip()
        check(first == '{"_meta":{"version":1}}', f"対応表の 1 行目: {first}")

        send({"id": 6, "cmd": "reverse", "mappingPath": mpath,
              "cells": [{"r": 2, "c": 1, "text": "{{PERSON_1}}"}, {"r": 2, "c": 2, "text": "{{PHONE_1}}"}]})
        r = recv()
        back = {(c["r"], c["c"]): c["text"] for c in r.get("cells", [])}
        check(r.get("ok") and back == {(2, 1): "山田太郎", (2, 2): "03-1234-5678"}
              and r.get("leftovers") == [], f"reverse: {back} leftovers={r.get('leftovers')}")

        send({"id": 7, "cmd": "shutdown"})
        r = recv()
        check(r.get("ok"), "shutdown")
        p.stdin.close()
        code = p.wait(timeout=30)
        check(code == 0, f"終了コード {code}")
    except Exception as e:  # noqa: BLE001 — スモークなので何が起きたかを出して失敗にする
        print(f"  [NG] 例外: {e}")
        fails += 1
    finally:
        if p.poll() is None:
            p.kill()
        try:
            os.remove(mpath)
        except OSError:
            pass

    print("HELPER SMOKE PASS" if fails == 0 else f"HELPER SMOKE FAIL ({fails})")
    return 0 if fails == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
