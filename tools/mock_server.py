#!/usr/bin/env python3
"""Mock server ImgTxd IPC v1.1 (TCP loopback) — uji sisi aplikasi TANPA game.

Jalankan di Termux (Android):  python3 mock_server.py
Lalu dari ImgTxd Studio (build loader terbaru): buka arsip IMG ->
sheet More -> Modloader -> Check Connection / Substitute / Reload Model.

Meniru plugin libimgtxd_hook.so v1.1: dengarkan 127.0.0.1:38177,
header 24 byte LE, payload SUBSTITUTE di-stream setelah header.
"""
import socket
import struct
import sys
import hashlib

MAGIC = 0x44585449  # 'ITXD'
HEADER = struct.Struct("<IHHIIQ")  # magic, cmd, reserved, modelId, pad, payloadSize = 24 byte
CMD_SUBSTITUTE, CMD_RELOAD, CMD_PING = 1, 2, 3
HOST, PORT = "127.0.0.1", 38177


def recv_exact(conn, n):
    buf = b""
    while len(buf) < n:
        chunk = conn.recv(n - len(buf))
        if not chunk:
            raise ConnectionError(f"EOF di {len(buf)}/{n} byte")
        buf += chunk
    return buf


def main():
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((HOST, PORT))
    srv.listen(4)
    print(f"[mock] listening on tcp {HOST}:{PORT}", flush=True)
    print("[mock] sekarang uji dari ImgTxd Studio (Check Connection dulu)", flush=True)
    while True:
        conn, peer = srv.accept()
        with conn:
            conn.settimeout(5.0)
            try:
                data = recv_exact(conn, HEADER.size)
                magic, cmd, _res, model_id, _pad, size = HEADER.unpack(data)
                if magic != MAGIC:
                    print(f"[mock] magic salah: 0x{magic:08X} (bukan ITXD), dibuang", flush=True)
                    continue
                if cmd == CMD_PING:
                    conn.sendall(b"\x01")
                    print("[mock] PING -> pong (0x01). Sisi aplikasi HIDUP.", flush=True)
                elif cmd == CMD_RELOAD:
                    print(f"[mock] RELOAD model {model_id} diterima.", flush=True)
                elif cmd == CMD_SUBSTITUTE:
                    payload = recv_exact(conn, size) if size else b""
                    sha = hashlib.sha256(payload).hexdigest()[:16]
                    head = payload[:4].hex()
                    print(
                        f"[mock] SUBSTITUTE model {model_id}: {size} byte diterima utuh lewat stream "
                        f"(sha256[:16]={sha}, 4 byte pertama={head})",
                        flush=True,
                    )
                else:
                    print(f"[mock] cmd tak dikenal: {cmd}", flush=True)
            except Exception as e:
                print(f"[mock] koneksi dari {peer} error: {e}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(0)
