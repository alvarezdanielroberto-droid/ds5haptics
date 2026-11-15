#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
# This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
# If a copy of the MPL was not distributed with this file, You can obtain one at
# https://mozilla.org/MPL/2.0/.
"""Cliente USB/IP de prueba: hace lo mismo que usbip-win2 + los drivers de Windows
(enumerar, importar, pedir descriptores, configurar, leer entradas, mandar salidas,
cancelar URBs) contra tests/usbip_harness.

Uso: python3 test_usbip_client.py <ruta al usbip_harness>
"""
import socket
import struct
import subprocess
import sys
import time

PORT = 32400
failures = []


def check(cond, msg):
    if not cond:
        failures.append(msg)
        print("FALLO:", msg)


def recv_all(s, n):
    b = b""
    while len(b) < n:
        chunk = s.recv(n - len(b))
        if not chunk:
            raise ConnectionError("cerrado")
        b += chunk
    return b


class Client:
    def __init__(self):
        self.s = socket.create_connection(("127.0.0.1", PORT))
        self.s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.seq = 1

    def import_dev(self, busid=b"1-1"):
        self.s.sendall(struct.pack(">HHI", 0x0111, 0x8003, 0) + busid.ljust(32, b"\0"))
        h = recv_all(self.s, 8)
        ver, code, status = struct.unpack(">HHI", h)
        if status != 0:
            return status, None
        body = recv_all(self.s, 0x138)
        return status, body

    def submit(self, ep, direction, length, setup=b"\0" * 8, data=b"", wait=True):
        seq = self.seq
        self.seq += 1
        hdr = struct.pack(">IIIII", 1, seq, 0x00010001, direction, ep)
        hdr += struct.pack(">IIiiI", 0, length, 0, -1, 0) + setup
        self.s.sendall(hdr + (data if direction == 0 else b""))
        if not wait:
            return seq
        return self.read_ret(direction)

    def read_ret(self, direction=None):
        """Lee una respuesta. Devuelve (cmd, seq, status, actual_length, data[, iso])."""
        h = recv_all(self.s, 48)
        cmd, seq = struct.unpack(">II", h[:8])
        status = struct.unpack(">i", h[20:24])[0]
        alen = struct.unpack(">I", h[24:28])[0]
        npk = struct.unpack(">i", h[32:36])[0]
        check(h[8:20] == b"\0" * 12, "devid/direction/ep deben ser 0 en la respuesta")
        data = b""
        self.last_iso = []
        if cmd == 3:
            if direction == 1 and alen:
                data = recv_all(self.s, alen)
            if npk > 0:
                raw = recv_all(self.s, 16 * npk)
                self.last_iso = [struct.unpack(">IIIi", raw[i * 16:(i + 1) * 16]) for i in range(npk)]
        return cmd, seq, status, alen, data

    def submit_iso(self, ep, direction, packets, data=b"", wait=True):
        """packets: lista de largos. Para OUT, data es el buffer completo."""
        seq = self.seq
        self.seq += 1
        total = sum(packets)
        hdr = struct.pack(">IIIII", 1, seq, 0x00010001, direction, ep)
        hdr += struct.pack(">IIiiI", 2, total, 0, len(packets), 1) + b"\0" * 8   # URB_ISO_ASAP
        desc = b""
        off = 0
        for ln in packets:
            desc += struct.pack(">IIIi", off, ln, 0, 0)
            off += ln
        self.s.sendall(hdr + (data if direction == 0 else b"") + desc)
        if not wait:
            return seq
        return self.read_ret(direction)

    def control_in(self, rt, req, value, index, length):
        setup = struct.pack("<BBHHH", rt, req, value, index, length)
        return self.submit(0, 1, length, setup)

    def control_out(self, rt, req, value, index, data=b""):
        setup = struct.pack("<BBHHH", rt, req, value, index, len(data))
        return self.submit(0, 0, len(data), setup, data)

    def unlink(self, target):
        seq = self.seq
        self.seq += 1
        self.s.sendall(struct.pack(">IIIII", 2, seq, 0x00010001, 0, 0) + struct.pack(">I", target) + b"\0" * 24)
        return seq


def main():
    harness = sys.argv[1]
    proc = subprocess.Popen([harness, str(PORT)], stdout=subprocess.PIPE, text=True)
    assert proc.stdout.readline().strip() == "READY"
    try:
        run()
    finally:
        proc.terminate()
        out = proc.communicate(timeout=5)[0]
    outputs = [l for l in out.splitlines() if l.startswith("OUTPUT")]
    check(len(outputs) == 2, f"se esperaban 2 salidas traducidas, hubo {len(outputs)}")
    check(all(l.startswith("OUTPUT OK") for l in outputs), "traducción USB 0x02 -> BT 0x31 incorrecta")
    check(any(l.startswith("SETFEATURE 08") for l in out.splitlines()), "SET_REPORT feature no informado")
    audio = [l for l in out.splitlines() if l.startswith("AUDIO")]
    # El audio se entrega al llegar: 5x8 + 20 + 20 paquetes = 80 ms = 3840 frames
    check(audio == ["AUDIO frames=3840 OK"], f"audio entregado a la háptica: {audio}")
    if failures:
        print(f"{len(failures)} fallos")
        sys.exit(1)
    print("OK: el DualSense virtual (HID + audio) responde como un mando USB real")


def run():
    # --- OP_REQ_DEVLIST ---
    s = socket.create_connection(("127.0.0.1", PORT))
    s.sendall(struct.pack(">HHI", 0x0111, 0x8005, 0))
    ver, code, status, ndev = struct.unpack(">HHII", recv_all(s, 12))
    check((ver, code, status, ndev) == (0x0111, 0x0005, 0, 1), "cabecera DEVLIST")
    dev = recv_all(s, 0x138)
    busid = dev[256:288].rstrip(b"\0")
    busnum, devnum, speed, vid, pid, bcd = struct.unpack(">IIIHHH", dev[0x120:0x132])
    nif = dev[0x137]
    ifs = recv_all(s, 4 * nif)
    check(busid == b"1-1", "busid")
    check((vid, pid, speed) == (0x054C, 0x0CE6, 3), f"vid/pid/speed {vid:04x} {pid:04x} {speed}")
    check(nif == 4 and [ifs[i * 4] for i in range(4)] == [1, 1, 1, 3], "interfaces: 3 de audio + HID")
    s.close()

    # --- Importar un busid inexistente: error ---
    c = Client()
    st, _ = c.import_dev(b"9-9")
    check(st != 0, "importar un busid inexistente debería fallar")
    c.s.close()

    # --- OP_REQ_IMPORT ---
    c = Client()
    st, body = c.import_dev()
    check(st == 0, "importar 1-1")
    check(struct.unpack(">HH", body[0x12C:0x130]) == (0x054C, 0x0CE6), "vid/pid en IMPORT")

    # --- Descriptores (lo que hace Windows al enumerar) ---
    cmd, _, st, n, d = c.control_in(0x80, 6, 0x0100, 0, 64)
    check(st == 0 and n == 18 and d[8:12] == bytes([0x4C, 0x05, 0xE6, 0x0C]), "descriptor de dispositivo")
    cmd, _, st, n, d = c.control_in(0x80, 6, 0x0200, 0, 9)
    check(st == 0 and n == 9 and struct.unpack("<H", d[2:4])[0] == 227 and d[4] == 4, "cabecera de configuración")
    cmd, _, st, n, d = c.control_in(0x80, 6, 0x0200, 0, 255)
    check(st == 0 and n == 227, "configuración completa")
    # Recorrer los descriptores y verificar la estructura
    pos, descs = 0, []
    while pos < len(d):
        ln = d[pos]
        check(ln >= 2 and pos + ln <= len(d), f"descriptor mal formado en {pos}")
        if ln < 2:
            break
        descs.append(d[pos:pos + ln])
        pos += ln
    check(pos == 227, "los descriptores suman 227 bytes")
    ifaces = [(x[2], x[3], x[5], x[6]) for x in descs if x[1] == 4]
    check(ifaces == [(0, 0, 1, 1), (1, 0, 1, 2), (1, 1, 1, 2), (2, 0, 1, 2), (2, 1, 1, 2), (3, 0, 3, 0)], f"interfaces {ifaces}")
    ac_hdr = [x for x in descs if x[1] == 0x24 and x[2] == 1 and len(x) == 10][0]
    check(struct.unpack("<H", ac_hdr[5:7])[0] == 73, "cabecera de control de audio: 73 bytes")
    ac_start = next(i for i, x in enumerate(descs) if x is ac_hdr)
    ac_len = sum(len(x) for x in descs[ac_start:ac_start + 7])
    check(ac_len == 73, f"la parte de control de audio mide {ac_len}, no 73")
    eps = {x[2]: x for x in descs if x[1] == 5}
    check(set(eps) == {0x01, 0x82, 0x84, 0x03}, "endpoints 0x01, 0x82, 0x84, 0x03")
    check(eps[0x01][3] == 0x09 and struct.unpack("<H", eps[0x01][4:6])[0] == 392 and eps[0x01][6] == 4, "EP 0x01 isócrono 392 bytes")
    check(eps[0x82][3] == 0x05 and struct.unpack("<H", eps[0x82][4:6])[0] == 196, "EP 0x82 isócrono 196 bytes")
    check(eps[0x84][6] == 6, "EP 0x84 cada 4 ms")
    hid = [x for x in descs if x[1] == 0x21][0]
    check(struct.unpack("<H", hid[7:9])[0] == 273, "descriptor HID (273 bytes de reporte)")
    fmts = [x for x in descs if x[1] == 0x24 and x[2] == 2 and len(x) == 11]
    check([(f[4], f[6], f[8] | f[9] << 8 | f[10] << 16) for f in fmts] == [(4, 16, 48000), (2, 16, 48000)], "formatos de audio")
    cmd, _, st, n, d = c.control_in(0x80, 6, 0x0600, 0, 10)
    check(st == 0 and n == 10, "device qualifier")
    cmd, _, st, n, d = c.control_in(0x80, 6, 0x0300, 0, 255)
    check(st == 0 and d == bytes([4, 3, 9, 4]), "idiomas")
    cmd, _, st, n, d = c.control_in(0x80, 6, 0x0302, 0x0409, 255)
    check(st == 0 and d[2:].decode("utf-16-le") == "DualSense Wireless Controller", "nombre del producto")
    cmd, _, st, n, d = c.control_in(0x80, 6, 0x03EE, 0, 255)
    check(st == -32, "string 0xEE (descriptor de Microsoft) debe dar stall")
    cmd, _, st, n, d = c.control_out(0x00, 9, 1, 0)
    check(st == 0, "SET_CONFIGURATION")
    cmd, _, st, n, d = c.control_out(0x21, 0x0A, 0, 3)
    check(st == 0, "SET_IDLE")
    cmd, _, st, n, d = c.control_in(0x81, 6, 0x2100, 3, 9)
    check(st == 0 and n == 9 and d[1] == 0x21, "descriptor HID pedido a la interfaz")
    cmd, _, st, n, d = c.control_in(0x81, 6, 0x2200, 3, 273)
    check(st == 0 and n == 273 and d[:4] == bytes([5, 1, 9, 5]) and d[-1] == 0xC0, "descriptor de reporte HID")

    # --- Feature reports (calibración) ---
    cmd, _, st, n, d = c.control_in(0xA1, 1, 0x0305, 3, 41)
    check(st == 0 and n == 41 and d[0] == 0x05, "feature 0x05")
    check(d[1:37] == bytes(0xA0 + i for i in range(1, 37)) and d[37:] == b"\0" * 4,
          "feature 0x05: datos reenviados y CRC de Bluetooth borrado")
    cmd, _, st, n, d = c.control_in(0xA1, 1, 0x0320, 3, 64)
    check(st == 0 and n == 64 and d[0] == 0x20, "feature 0x20 (firmware)")
    cmd, _, st, n, d = c.control_in(0xA1, 1, 0x0309, 3, 20)
    check(st == -32, "feature no disponible en el mando -> stall")
    cmd, _, st, n, d = c.control_out(0x21, 9, 0x0308, 3, bytes([8]) + b"\0" * 47)
    check(st == 0, "SET_REPORT feature")

    # --- Entradas por interrupción (endpoint 4 IN) ---
    for i in range(5):
        cmd, _, st, n, d = c.submit(4, 1, 64)
        check(cmd == 3 and st == 0 and n == 64 and d[0] == 0x01, "reporte de entrada USB 0x01")
        if n == 64:
            ctr = (d[1] - 2 * 7) & 0xFF   # usb[1] = bt[2] = 2*7 + contador
            exp = bytes([0x01]) + bytes(((i2 * 7 + ctr) & 0xFF) for i2 in range(2, 65))
            check(d == exp, "traducción BT 0x31 -> USB 0x01")

    # Dos URBs pendientes a la vez (como hace el driver HID de Windows)
    a = c.submit(4, 1, 64, wait=False)
    b = c.submit(4, 1, 64, wait=False)
    got = {c.read_ret(1)[1], c.read_ret(1)[1]}
    check(got == {a, b}, "dos URBs IN pendientes se completan ambas")

    # --- Salida por interrupción (endpoint 3 OUT): gatillos / luces ---
    out = bytes([0x02]) + bytes(range(1, 48))
    cmd, _, st, n, d = c.submit(3, 0, len(out), data=out)
    check(st == 0 and n == 48, "reporte de salida USB 0x02")
    cmd, _, st, n, d = c.control_out(0x21, 9, 0x0202, 3, out)
    check(st == 0, "SET_REPORT de salida")


    # --- Controles de audio (lo que hace Windows al cargar el driver de audio) ---
    for req, exp in ((0x82, -100 * 256), (0x83, 0), (0x84, 256), (0x81, 0)):
        cmd, _, st, n, d = c.control_in(0xA1, req, 0x0200, 0x0200, 2)
        check(st == 0 and n == 2 and struct.unpack("<h", d)[0] == exp, f"volumen del parlante (pedido {req:#x})")
    cmd, _, st, n, d = c.control_out(0x21, 0x01, 0x0200, 0x0200, struct.pack("<h", -20 * 256))
    check(st == 0, "SET_CUR volumen")
    cmd, _, st, n, d = c.control_in(0xA1, 0x81, 0x0200, 0x0200, 2)
    check(st == 0 and struct.unpack("<h", d)[0] == -20 * 256, "el volumen queda guardado")
    cmd, _, st, n, d = c.control_in(0xA1, 0x81, 0x0100, 0x0500, 1)
    check(st == 0 and d == b"\0", "mute del micrófono")
    cmd, _, st, n, d = c.control_in(0xA1, 0x81, 0x0200, 0x0900, 2)
    check(st == -32, "unidad de audio inexistente -> stall")

    # --- Activar el parlante y mandar audio (ritmo de 1 ms por paquete) ---
    cmd, _, st, n, d = c.control_out(0x01, 0x0B, 1, 1)
    check(st == 0, "SET_INTERFACE parlante alt 1")
    cmd, _, st, n, d = c.control_in(0x81, 0x0A, 0, 1, 1)
    check(st == 0 and d == b"\x01", "GET_INTERFACE parlante")

    def audio_buf(packets, k0):
        b = b""
        k = k0
        for _ in range(packets):
            for _ in range(48):
                b += struct.pack("<hhhh", 1000, 1000, k, -k)
                k = (k + 37) % 30000
        return b, k

    k = 0
    seqs = []
    t0 = time.monotonic()
    for _ in range(5):                       # 5 URBs x 8 paquetes = 40 ms de audio
        buf, k = audio_buf(8, k)
        seqs.append(c.submit_iso(1, 0, [384] * 8, buf, wait=False))
    times = []
    for _ in range(5):
        cmd, seq, st, n, d = c.read_ret(0)
        times.append(time.monotonic() - t0)
        check(cmd == 3 and st == 0 and n == 384 * 8, "URB isócrono de salida completado")
        check(len(c.last_iso) == 8 and all(p[2] == 384 and p[3] == 0 for p in c.last_iso),
              "descriptores isócronos devueltos")
    check(0.030 <= times[-1] <= 0.080, f"40 ms de audio se consumieron en {times[-1] * 1000:.0f} ms (debe ser a ritmo real)")
    check(times[0] >= 0.005, f"el primer URB volvió en {times[0] * 1000:.1f} ms: demasiado rápido")
    print(f"ritmo isócrono: 40 ms de audio en {times[-1] * 1000:.1f} ms")

    # --- Micrófono: devuelve silencio, 48 frames estéreo por ms ---
    cmd, _, st, n, d = c.control_out(0x01, 0x0B, 1, 2)
    cmd, seq, st, n, d = c.submit_iso(2, 1, [196] * 4)
    check(st == 0 and n == 192 * 4 and d == b"\0" * (192 * 4), "micrófono en silencio")
    check([p[2] for p in c.last_iso] == [192] * 4, "paquetes del micrófono de 192 bytes")

    # --- Cancelar un URB isócrono en cola ---
    buf, k = audio_buf(20, k)
    a = c.submit_iso(1, 0, [384] * 20, buf, wait=False)
    buf, k = audio_buf(20, k)
    b = c.submit_iso(1, 0, [384] * 20, buf, wait=False)
    u = c.unlink(b)
    rets = []
    while not (any(r[0] == 4 for r in rets) and any(r[0] == 3 and r[1] == a for r in rets)):
        rets.append(c.read_ret(0))
    un = [r for r in rets if r[0] == 4][0]
    check(un[1] == u and un[2] == -104, "URB isócrono cancelado")
    check(not any(r[0] == 3 and r[1] == b for r in rets), "el URB cancelado no se completa")
    cmd, _, st, n, d = c.control_out(0x01, 0x0B, 0, 1)
    check(st == 0, "SET_INTERFACE parlante alt 0")

    # --- Endpoint inexistente ---
    cmd, _, st, n, d = c.submit(7, 1, 64)
    check(st == -32, "endpoint inexistente -> stall")

    # --- Cancelar un URB pendiente (UNLINK) ---
    unlinked_ok = 0
    for _ in range(20):
        # El primero puede completarse con un reporte ya disponible; el segundo queda pendiente
        first = c.submit(4, 1, 64, wait=False)
        target = c.submit(4, 1, 64, wait=False)
        u = c.unlink(target)
        rets = []
        while True:
            r = c.read_ret(1)
            rets.append(r)
            if any(x[0] == 4 for x in rets) and any(x[0] == 3 and x[1] == first for x in rets):
                break
        ret_unlink = [r for r in rets if r[0] == 4][0]
        submit_done = any(r[0] == 3 and r[1] == target for r in rets)
        check(ret_unlink[1] == u, "seqnum del RET_UNLINK")
        if ret_unlink[2] == -104:
            check(not submit_done, "URB cancelado no debe completarse")
            unlinked_ok += 1
        else:
            check(ret_unlink[2] == 0 and submit_done, "si no se canceló, debe haberse completado antes")
    check(unlinked_ok > 0, "ningún UNLINK llegó a cancelar (improbable)")

    # --- Desconectar y volver a importar ---
    c.s.close()
    time.sleep(0.1)
    c = Client()
    st, _ = c.import_dev()
    check(st == 0, "volver a importar después de desconectar")
    cmd, _, st, n, d = c.submit(4, 1, 64)
    check(st == 0 and n == 64, "entradas después de reconectar")
    c.s.close()


if __name__ == "__main__":
    main()
