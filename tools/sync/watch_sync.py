#!/usr/bin/env python3
"""Vuelca los contadores sync/* de todas las capas, una linea por segundo."""
import socket, struct, sys, time, collections

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 6260


def _pad(i):
    return (i + 4) & ~3


def parse_message(msg):
    """Una sola OSC message -> (direccion, [argumentos])."""
    end = msg.index(b'\x00')
    addr = msg[:end].decode('ascii', 'replace')
    i = _pad(end)
    if i >= len(msg) or msg[i:i + 1] != b',':
        return addr, []
    end = msg.index(b'\x00', i)
    tags = msg[i + 1:end].decode('ascii', 'replace')
    i = _pad(end)
    args = []
    for t in tags:
        if t in 'ifr':
            if i + 4 > len(msg):
                break
            args.append(struct.unpack('>i' if t != 'f' else '>f', msg[i:i + 4])[0])
            i += 4
        elif t in 'hdt':
            if i + 8 > len(msg):
                break
            args.append(struct.unpack('>q' if t != 'd' else '>d', msg[i:i + 8])[0])
            i += 8
        elif t == 's' or t == 'S':
            end = msg.index(b'\x00', i)
            args.append(msg[i:end].decode('utf-8', 'replace'))
            i = _pad(end)
        elif t == 'b':
            (n,) = struct.unpack('>i', msg[i:i + 4])
            i += 4
            args.append(msg[i:i + n])
            i = _pad(i + n - 1) if n else i
        elif t in 'TF':
            args.append(t == 'T')
        elif t in 'N I':
            args.append(None)
    return addr, args


def parse_packet(data):
    """CasparCG manda #bundle, no mensajes sueltos. Devuelve una lista de (addr, args)."""
    if data[:8] == b'#bundle\x00':
        out = []
        i = 16  # 8 de "#bundle\0" + 8 del timetag
        while i + 4 <= len(data):
            (n,) = struct.unpack('>i', data[i:i + 4])
            i += 4
            if n <= 0 or i + n > len(data):
                break
            out.extend(parse_packet(data[i:i + n]))  # los bundles pueden anidarse
            i += n
        return out
    try:
        return [parse_message(data)]
    except ValueError:
        return []


def main():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind(('0.0.0.0', PORT))
    sock.settimeout(0.2)
    state = collections.defaultdict(dict)
    last = 0.0
    print("escuchando OSC en %d; Ctrl-C para salir" % PORT, file=sys.stderr)
    while True:
        try:
            data, _ = sock.recvfrom(65536)
            for addr, args in parse_packet(data):
                if '/sync/' not in addr or not args:
                    continue
                head, field = addr.split('/sync/', 1)
                parts = head.split('/')
                if 'layer' not in parts or 'channel' not in parts:
                    continue
                ch = parts[parts.index('channel') + 1]
                ly = parts[parts.index('layer') + 1]
                state['%s-%s' % (ch, ly)][field] = args[0]
        except socket.timeout:
            pass
        now = time.time()
        if now - last >= 1.0 and state:
            last = now
            stamp = time.strftime('%H:%M:%S')
            for key in sorted(state, key=lambda k: [int(x) for x in k.split('-')]):
                s = state[key]
                print("%s capa %6s  modo=%-8s ts=%-9s slip=%6s rep=%6s drop=%6s "
                      "disc=%3s reconn=%3s buf=%s/%s ppm=%s src=%s gslip=%s"
                      % (stamp, key, s.get('mode', '?'), s.get('timestamps', '?'),
                         s.get('net-slip-frames', '?'), s.get('repeats', '?'),
                         s.get('drops', '?'), s.get('discontinuities', '?'),
                         s.get('reconnects', '?'), s.get('buffer', '?'),
                         s.get('buffer-avg', '?'), s.get('offset-ppm', '?'),
                         s.get('source-time', '?'), s.get('graph-slip-frames', '?')))
            print('', flush=True)


if __name__ == '__main__':
    main()
