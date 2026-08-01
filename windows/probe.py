"""Link probe for CatCam: connect to the tablet like the host would and
report what the network actually delivers, second by second.

    python probe.py <tablet-ip> [seconds]

Speaks the wire protocol ([1B type][4B big-endian len][payload]; 0x01 config,
0x02 H.264 AU, 0x03 PCM chunk) but decodes nothing: it only measures. Use it
to judge a transport path (WiFi vs USB forward) independently of the whole
virtual-camera chain, and to watch the tablet's adaptive bitrate converge.
Note: the tablet serves ONE client, so stop CatCamHost first.
"""
import socket
import struct
import sys
import time


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    ip = sys.argv[1]
    duration = int(sys.argv[2]) if len(sys.argv) > 2 else 60

    # The tablet serves ONE client per ServerSocket cycle: a connect that
    # lands in the backlog of an owned session gets silence, then RST when
    # that session ends. So: connect, demand a first packet quickly (audio
    # alone guarantees 10/s on a healthy session), otherwise reconnect and
    # race for the fresh ServerSocket.
    s = None
    for attempt in range(30):
        try:
            s = socket.create_connection((ip, 9000), timeout=5)
            s.settimeout(2.5)
            first = s.recv(1)
            if first:
                break
            s.close(); s = None
        except (socket.timeout, ConnectionError, OSError):
            if s: s.close()
            s = None
        time.sleep(0.3)
    if s is None:
        print("never received a first byte: another client owns the stream")
        sys.exit(2)
    s.settimeout(5)
    print(f"connected to {ip}:9000 (attempt {attempt + 1}), measuring {duration}s")
    pending_first = first

    t0 = time.monotonic()
    last = t0
    vids = auds = vbytes = abytes = 0
    total_v = total_bytes = 0
    header = bytearray()

    def read_exact(n):
        nonlocal pending_first
        buf = bytearray()
        if pending_first:
            buf.extend(pending_first)
            pending_first = b""
        while len(buf) < n:
            chunk = s.recv(n - len(buf))
            if not chunk:
                raise ConnectionError("closed")
            buf.extend(chunk)
        return bytes(buf)

    try:
        while time.monotonic() - t0 < duration:
            try:
                hdr = read_exact(5)
            except socket.timeout:
                print(f"{time.monotonic()-t0:6.1f}s  STALL: no packet in 5s")
                continue
            ptype, plen = hdr[0], struct.unpack(">I", hdr[1:5])[0]
            payload = read_exact(plen)
            if ptype == 0x01:
                w, h = struct.unpack(">II", payload[:8])
                print(f"{time.monotonic()-t0:6.1f}s  config {w}x{h} ({plen}B)")
            elif ptype == 0x02:
                vids += 1
                vbytes += plen
                total_v += 1
            elif ptype == 0x03:
                auds += 1
                abytes += plen
            total_bytes += plen + 5
            now = time.monotonic()
            if now - last >= 1.0:
                print(f"{now-t0:6.1f}s  video {vids:3d}fps {vbytes/1024:7.1f}KB/s"
                      f"  audio {auds:2d}pkt {abytes/1024:5.1f}KB/s"
                      f"  total {(vbytes+abytes)/1024/(now-last):7.1f}KB/s")
                vids = auds = vbytes = abytes = 0
                last = now
    except (ConnectionError, OSError) as e:
        print(f"connection ended: {e}")
    finally:
        s.close()
    dt = time.monotonic() - t0
    print(f"\nTOTAL: {dt:.1f}s, {total_v} video frames ({total_v/dt:.1f}fps avg), "
          f"{total_bytes/1024/dt:.1f}KB/s avg")


if __name__ == "__main__":
    main()
