#!/usr/bin/env python3
"""pmos-debug-shell.py [--timeout SECS] CMD [CMD...]: run shell commands in postmarketOS's initramfs debug shell over
telnet (172.16.42.1:23, the pmos.debug-shell boot option) and print what each one wrote. Each command is followed by a
marker so the output is cut exactly; the exit status comes with it. The last command may be pmos_continue_boot.

docs/data-volume.md section 7 uses it for the no-keys shrink of pmOS's root (resize2fs can only shrink an unmounted
ext4, and the debug shell stops the boot before the root is mounted)."""
import socket
import sys
import time

HOST, PORT = '172.16.42.1', 23
IAC, DONT, DO, WONT, WILL = 255, 254, 253, 252, 251


def connect(wait):
    deadline = time.time() + wait
    while True:
        try:
            s = socket.create_connection((HOST, PORT), timeout=5)
            return s
        except OSError:
            if time.time() > deadline:
                sys.exit(f'pmos-debug-shell: no telnet at {HOST}:{PORT} within {wait} s')
            time.sleep(2)


def readUntil(s, marker, timeout):
    """bytes until marker (telnet negotiation answered with refusals, stripped), or what came by the timeout"""
    s.settimeout(timeout)
    buf = bytearray()
    deadline = time.time() + timeout
    while marker not in buf:
        if time.time() > deadline:
            break
        try:
            chunk = s.recv(4096)
        except socket.timeout:
            break
        if not chunk:
            break
        i = 0
        while i < len(chunk):
            b = chunk[i]
            if b == IAC and i + 2 < len(chunk) + 1:
                cmd = chunk[i + 1] if i + 1 < len(chunk) else None
                if cmd in (DO, DONT, WILL, WONT) and i + 2 < len(chunk):
                    opt = chunk[i + 2]
                    s.sendall(bytes([IAC, WONT if cmd == DO else DONT, opt]))
                    i += 3
                    continue
                i += 2
                continue
            buf.append(b)
            i += 1
    return bytes(buf)


def main():
    args = sys.argv[1:]
    timeout = 600
    wait = 180
    if args and args[0] == '--timeout':
        timeout = int(args[1])
        args = args[2:]
    if args and args[0] == '--wait':
        wait = int(args[1])
        args = args[2:]
    s = connect(wait)
    banner = readUntil(s, b'# ', 20).decode(errors='replace')
    print(banner, end='' if banner.endswith(' ') else '\n')
    for n, cmd in enumerate(args):
        marker = f'__DLISA_DONE_{n}_'
        # the shell echoes the line: the marker is split in what is sent ("" between its halves) so only the
        # command's own output carries it whole
        # (a multi-line command, such as a quoted heredoc, gets the marker on a line of its own after it)
        sep = '\n' if '\n' in cmd else '; '
        s.sendall((cmd + f'{sep}echo __DLISA_""DONE_{n}_$?\n').encode())
        out = readUntil(s, marker.encode(), timeout).decode(errors='replace')
        head, found, tail = out.partition(marker)
        status = tail.strip().split('\n')[0] if tail else '?'
        body = head.split('\n', 1)[1] if '\n' in head else ''      # the first line is the echo of the command
        print(f'$ {cmd}\n{body}[exit {status}]', flush=True)
        if not found:
            print(f'pmos-debug-shell: no end marker for {cmd!r} within {timeout} s', flush=True)
        if cmd.strip().startswith('pmos_continue_boot'):
            break
    s.close()


if __name__ == '__main__':
    main()
