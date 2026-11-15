#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
# If a copy of the MPL was not distributed with this file, You can obtain one at
# https://mozilla.org/MPL/2.0/.
"""Prueba los mandos de compatibilidad (PS4 y Xbox 360) y el cambio de mando en caliente,
con el mismo cliente USB/IP simulado de test_usbip_client.py.

Uso: python3 test_pads.py <ruta al usbip_harness>
"""
import os
import socket
import struct
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import test_usbip_client as base
from test_usbip_client import Client, check, recv_all, failures

PORT = 32410
base.PORT = PORT


def devlist():
    s = socket.create_connection(("127.0.0.1", PORT))
    s.sendall(struct.pack(">HHI", 0x0111, 0x8005, 0))
    recv_all(s, 12)
    dev = recv_all(s, 0x138)
    speed, vid, pid = struct.unpack(">IHH", dev[0x128:0x130])
    cls = dev[0x132]
    nif = dev[0x137]
    ifs = recv_all(s, 4 * nif)
    s.close()
    return vid, pid, speed, cls, [tuple(ifs[i * 4:i * 4 + 3]) for i in range(nif)]


def split_descs(d):
    pos, out = 0, []
    while pos < len(d):
        ln = d[pos]
        if ln < 2 or pos + ln > len(d):
            check(False, f"descriptor mal formado en {pos}")
            break
        out.append(d[pos:pos + ln])
        pos += ln
    return out, pos


def hid_sizes(desc):
    """Bytes (sin Report ID) de cada reporte: {'in': {id: n}, 'out': ..., 'feat': ...}."""
    bits = {0x80: {}, 0x90: {}, 0xB0: {}}
    rid = size = count = 0
    i = 0
    depth = 0
    while i < len(desc):
        p = desc[i]
        n = 4 if (p & 3) == 3 else (p & 3)
        v = int.from_bytes(desc[i + 1:i + 1 + n], "little")
        tag = p & 0xFC
        if tag == 0x84:
            rid = v
        elif tag == 0x74:
            size = v
        elif tag == 0x94:
            count = v
        elif tag == 0xA0:
            depth += 1
        elif tag == 0xC0:
            depth -= 1
        elif tag in bits:
            bits[tag][rid] = bits[tag].get(rid, 0) + size * count
        i += 1 + n
    check(i == len(desc) and depth == 0, "el descriptor HID no termina bien (items o colecciones)")
    for t in bits:
        for r, b in bits[t].items():
            check(b % 8 == 0, f"reporte {r:#x} no es múltiplo de 8 bits")
    return {k: {r: b // 8 for r, b in v.items()} for k, v in zip(("in", "out", "feat"), bits.values())}


def run_harness(mode, fn):
    proc = subprocess.Popen([sys.argv[1], str(PORT), mode], stdout=subprocess.PIPE, stdin=subprocess.PIPE, text=True)
    assert proc.stdout.readline().strip() == "READY"
    try:
        fn(proc)
    finally:
        proc.terminate()
        out = proc.communicate(timeout=5)[0]
    return out.splitlines()


def test_xbox(proc):
    vid, pid, speed, cls, ifs = devlist()
    check((vid, pid, speed, cls) == (0x045E, 0x028E, 2, 0xFF), f"xbox devlist {vid:04x}:{pid:04x} speed {speed}")
    check(ifs == [(0xFF, 0x5D, 0x01)], f"xbox interfaz {ifs}")
    c = Client()
    st, _ = c.import_dev()
    check(st == 0, "xbox import")
    _, _, st, n, d = c.control_in(0x80, 6, 0x0100, 0, 18)
    check(st == 0 and n == 18 and d[4:7] == b"\xff\xff\xff" and d[8:12] == bytes([0x5E, 0x04, 0x8E, 0x02]), "xbox descriptor de dispositivo")
    _, _, st, n, d = c.control_in(0x80, 6, 0x0200, 0, 9)
    check(st == 0 and struct.unpack("<H", d[2:4])[0] == 49, "xbox cabecera de configuración")
    _, _, st, n, d = c.control_in(0x80, 6, 0x0200, 0, 255)
    descs, total = split_descs(d)
    check(st == 0 and n == 49 and total == 49, "xbox configuración de 49 bytes")
    check(descs[1][5:8] == bytes([0xFF, 0x5D, 0x01]) and descs[1][4] == 2, "xbox interfaz FF/5D/01 con 2 endpoints")
    eps = {x[2]: x for x in descs if x[1] == 5}
    check(set(eps) == {0x81, 0x02} and all(e[3] == 3 and e[4] == 32 for e in eps.values()), "xbox endpoints 0x81 y 0x02 de 32 bytes")
    _, _, st, n, d = c.control_in(0x80, 6, 0x0302, 0x0409, 255)
    check(st == 0 and d[2:].decode("utf-16-le") == "Controller", "xbox nombre")
    _, _, st, n, d = c.control_in(0x80, 6, 0x0600, 0, 10)
    check(st == -32, "xbox: device qualifier da stall (full speed)")
    _, _, st, n, d = c.control_in(0xC1, 0x01, 0x0100, 0, 20)
    check(st == -32, "xbox: petición propia de XInput da stall")
    _, _, st, n, d = c.control_out(0x00, 9, 1, 0)
    check(st == 0, "xbox SET_CONFIGURATION")

    # Entrada: cruz + derecha + L1 + Options + PS, sticks y gatillos
    _, _, st, n, d = c.submit(1, 1, 32)
    exp = bytes([0x00, 0x14, 0x08 | 0x10, 0x01 | 0x04 | 0x10, 0x11, 0x22])
    exp += struct.pack("<hhhh", -32768, 32767 - 65535, 0x8080 - 32768, 32767 - 0x4040) + bytes(6)
    check(st == 0 and n == 20, f"xbox reporte de 20 bytes (llegaron {n})")
    check(d == exp, f"xbox traducción de entrada\n  {d.hex()}\n  {exp.hex()}")

    # Vibración
    for msg in (bytes([0, 8, 0, 0xC0, 0x40, 0, 0, 0]), bytes([0, 8, 0, 0xC0, 0x40, 0, 0, 0]),
                bytes([1, 3, 6]), bytes([0, 8, 0, 0, 0, 0, 0, 0])):
        _, _, st, n, d = c.submit(2, 0, len(msg), data=msg)
        check(st == 0 and n == len(msg), "xbox salida aceptada")
    c.s.close()


def test_ps4(proc):
    vid, pid, speed, cls, ifs = devlist()
    check((vid, pid, speed, cls) == (0x054C, 0x05C4, 2, 0), f"ps4 devlist {vid:04x}:{pid:04x}")
    check(ifs == [(3, 0, 0)], "ps4 interfaz HID")
    c = Client()
    st, _ = c.import_dev()
    check(st == 0, "ps4 import")
    _, _, st, n, d = c.control_in(0x80, 6, 0x0100, 0, 18)
    check(st == 0 and n == 18 and d[8:12] == bytes([0x4C, 0x05, 0xC4, 0x05]), "ps4 descriptor de dispositivo")
    _, _, st, n, d = c.control_in(0x80, 6, 0x0200, 0, 255)
    descs, total = split_descs(d)
    check(st == 0 and n == 41 and total == 41, "ps4 configuración de 41 bytes")
    hid = [x for x in descs if x[1] == 0x21][0]
    rdlen = struct.unpack("<H", hid[7:9])[0]
    eps = {x[2]: x for x in descs if x[1] == 5}
    check(set(eps) == {0x84, 0x03}, "ps4 endpoints 0x84 y 0x03")
    _, _, st, n, d = c.control_in(0x80, 6, 0x0302, 0x0409, 255)
    check(st == 0 and d[2:].decode("utf-16-le") == "Wireless Controller", "ps4 nombre")
    c.control_out(0x00, 9, 1, 0)
    c.control_out(0x21, 0x0A, 0, 0)
    _, _, st, n, d = c.control_in(0x81, 6, 0x2100, 0, 9)
    check(st == 0 and d == hid, "ps4 descriptor HID por interfaz")
    _, _, st, n, rd = c.control_in(0x81, 6, 0x2200, 0, rdlen)
    check(st == 0 and n == rdlen and rd[:4] == bytes([5, 1, 9, 5]) and rd[-1] == 0xC0, "ps4 descriptor de reporte")
    sizes = hid_sizes(rd)
    check(sizes["in"] == {1: 63}, f"ps4 entrada 0x01 de 64 bytes: {sizes['in']}")
    check(sizes["out"] == {5: 31}, f"ps4 salida 0x05 de 32 bytes: {sizes['out']}")
    f = sizes["feat"]
    check((f.get(0x02), f.get(0x12), f.get(0xA3), f.get(0x81)) == (36, 15, 48, 6), "ps4 features 0x02/0x12/0xA3/0x81")
    print(f"ps4: descriptor de reporte de {rdlen} bytes, {len(f)} features")

    # Features
    _, _, st, n, d = c.control_in(0xA1, 1, 0x0302, 0, 37)
    check(st == 0 and n == 37 and d[0] == 2 and d[1:35] == bytes((0xA0 + i) & 0xFF for i in range(1, 35)) and d[35:] == b"\0\0",
          "ps4 calibración tomada del DualSense")
    _, _, st, n, d = c.control_in(0xA1, 1, 0x0312, 0, 16)
    check(st == 0 and n == 16 and d[:7] == bytes([0x12, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66]), "ps4 MAC (0x12)")
    _, _, st, n, d = c.control_in(0xA1, 1, 0x03A3, 0, 49)
    check(st == 0 and n == 49 and d[0] == 0xA3, "ps4 versión (0xA3)")
    _, _, st, n, d = c.control_in(0xA1, 1, 0x0377, 0, 64)
    check(st == -32, "ps4 feature inexistente -> stall")

    # Entrada
    _, _, st, n, d = c.submit(4, 1, 64)
    check(st == 0 and n == 64 and d[0] == 1, "ps4 reporte de 64 bytes")
    if n == 64:
        check(d[1:5] == bytes([0x00, 0xFF, 0x80, 0x40]), "ps4 sticks")
        check(d[5] == 0x22 and d[6] == 0x21 and (d[7] & 3) == 1, "ps4 botones")
        check(d[8] == 0x11 and d[9] == 0x22, "ps4 gatillos")
        check(d[13:15] == bytes([0x34, 0x12]) and d[19:21] == bytes([0x78, 0x56]), "ps4 sensores")
        check(d[30] == 0x11, f"ps4 batería {d[30]:#x}")
        # toque: x=1000, y=540*942/1080=471
        check(d[33] == 1 and d[35:39] == bytes([0x05, 0xE8, 0x73, 0x1D]), f"ps4 panel táctil {d[35:39].hex()}")
        check(d[39] == 0x80, "ps4 segundo dedo inactivo")
    _, _, _, _, d2 = c.submit(4, 1, 64)
    check((d2[7] >> 2) != (d[7] >> 2), "ps4 contador de reportes avanza")

    # Salida: vibración + color
    out = bytes([0x05, 0x03, 0x00, 0x00, 0x40, 0xC0, 0x10, 0x20, 0x30]) + bytes(23)
    _, _, st, n, _ = c.submit(3, 0, len(out), data=out)
    check(st == 0 and n == 32, "ps4 salida por endpoint")
    out2 = bytes([0x05, 0x01, 0x00, 0x00, 0x00, 0x00]) + bytes(26)
    _, _, st, n, _ = c.control_out(0x21, 9, 0x0205, 0, out2)
    check(st == 0, "ps4 salida por SET_REPORT")
    c.s.close()


def test_switch(proc):
    check(devlist()[:2] == (0x054C, 0x0CE6), "switch: arranca como DualSense")
    c = Client()
    st, _ = c.import_dev()
    check(st == 0, "switch: import DualSense")
    _, _, st, n, d = c.submit(4, 1, 64)
    check(st == 0 and n == 64, "switch: entrada del DualSense")
    proc.stdin.write("xbox\n")
    proc.stdin.flush()
    c.s.settimeout(3)
    closed = False
    try:
        pending = c.submit(4, 1, 64, wait=False)
        while True:
            if not c.s.recv(4096):
                closed = True
                break
    except (ConnectionError, socket.timeout, OSError):
        closed = True
    check(closed, "switch: el servidor corta la conexión del mando anterior")
    time.sleep(0.2)
    check(devlist()[:2] == (0x045E, 0x028E), "switch: ahora exporta el mando de Xbox")
    c = Client()
    st, _ = c.import_dev()
    check(st == 0, "switch: import Xbox")
    _, _, st, n, d = c.submit(1, 1, 32)
    check(st == 0 and n == 20 and d[1] == 0x14, "switch: entrada del mando de Xbox")
    c.s.close()
    proc.stdin.write("ps4\n")
    proc.stdin.flush()
    time.sleep(0.3)
    check(devlist()[:2] == (0x054C, 0x05C4), "switch: ahora exporta el DualShock 4")
    c = Client()
    st, _ = c.import_dev()
    _, _, st, n, d = c.submit(4, 1, 64)
    check(st == 0 and n == 64 and d[5] == 0x22, "switch: entrada del DualShock 4")
    c.s.close()


def main():
    out = run_harness("xbox", test_xbox)
    rum = [l for l in out if l.startswith("RUMBLE")]
    check(rum == ["RUMBLE 192 64", "RUMBLE 0 0"], f"xbox vibración (sin repetir valores iguales): {rum}")

    out = run_harness("ps4", test_ps4)
    rum = [l for l in out if l.startswith("RUMBLE")]
    check(rum == ["RUMBLE 192 64", "RUMBLE 0 0"], f"ps4 vibración: {rum}")
    check([l for l in out if l.startswith("LIGHTBAR")] == ["LIGHTBAR 16 32 48"], "ps4 color de la barra de luz")

    run_harness("switch", test_switch)

    if failures:
        print(f"{len(failures)} fallos")
        sys.exit(1)
    print("OK: mandos de PS4 y Xbox 360, y cambio de mando en caliente")


if __name__ == "__main__":
    main()
