"""
MimiClaw Board B (Executor) - main.py
Listens for Python scripts via ESP-NOW from Board A, executes them,
and sends back the result (stdout + exceptions).

Protocol:
  Header (7 bytes): [type(1) | seq(2 LE) | total_len(4 LE)]
  Types: 0x01=SCRIPT_START, 0x02=SCRIPT_CHUNK, 0x03=SCRIPT_END
         0x11=RESULT_START, 0x12=RESULT_CHUNK, 0x13=RESULT_END
"""

import espnow
import network
import sys
import io
import struct
import time

# --- Constants ---
MSG_SCRIPT_START = 0x01
MSG_SCRIPT_CHUNK = 0x02
MSG_SCRIPT_END   = 0x03
MSG_RESULT_START = 0x11
MSG_RESULT_CHUNK = 0x12
MSG_RESULT_END   = 0x13

HEADER_SIZE = 7
MAX_PAYLOAD = 250
CHUNK_DATA  = MAX_PAYLOAD - HEADER_SIZE

# --- ESP-NOW Setup ---
sta = network.WLAN(network.STA_IF)
sta.active(True)

e = espnow.ESPNow()
e.active(True)

# Board A peer - will be set on first received message
peer_mac = None


def build_header(msg_type, seq, total_len=0):
    return struct.pack('<BHI', msg_type, seq, total_len)


def send_result(mac, result_text):
    """Send result back to Board A in chunks."""
    data = result_text.encode('utf-8')
    total = len(data)
    offset = 0
    seq = 0

    while offset < total:
        chunk_size = min(CHUNK_DATA, total - offset)
        is_first = (seq == 0)
        is_last = (offset + chunk_size >= total)

        if is_first:
            msg_type = MSG_RESULT_START
        elif is_last:
            msg_type = MSG_RESULT_END
        else:
            msg_type = MSG_RESULT_CHUNK

        header = build_header(msg_type, seq, total if is_first else 0)
        pkt = header + data[offset:offset + chunk_size]

        try:
            e.send(mac, pkt)
        except Exception as ex:
            print('ESP-NOW send error:', ex)
            return

        offset += chunk_size
        seq += 1
        if not is_last:
            time.sleep_ms(5)

    # Handle empty result
    if total == 0:
        header = build_header(MSG_RESULT_START, 0, 0)
        e.send(mac, header)
        header = build_header(MSG_RESULT_END, 1, 0)
        e.send(mac, header)


def execute_script(code):
    """Execute Python code and capture stdout + exceptions."""
    old_stdout = sys.stdout
    captured = io.StringIO()
    sys.stdout = captured

    try:
        exec(code, {'__name__': '__main__'})
    except Exception as ex:
        print('Error:', type(ex).__name__, '-', ex)
    finally:
        sys.stdout = old_stdout

    return captured.getvalue()


# --- Main Loop ---
print('Board B executor ready. Waiting for scripts...')

script_buf = bytearray()
script_total = 0
script_seq = 0
receiving = False

while True:
    mac, msg = e.recv(timeout_ms=1000)
    if msg is None:
        continue

    if len(msg) < HEADER_SIZE:
        continue

    msg_type, seq, total_len = struct.unpack('<BHI', msg[:HEADER_SIZE])
    payload = msg[HEADER_SIZE:]

    # Remember the peer
    if peer_mac is None and mac is not None:
        peer_mac = bytes(mac)
        try:
            e.add_peer(peer_mac)
        except Exception:
            pass  # peer may already exist
        print('Paired with Board A:', ':'.join('%02x' % b for b in peer_mac))

    if msg_type == MSG_SCRIPT_START:
        script_total = total_len
        script_buf = bytearray(payload)
        script_seq = seq + 1
        receiving = True

    elif msg_type == MSG_SCRIPT_CHUNK and receiving:
        if seq == script_seq:
            script_buf.extend(payload)
            script_seq = seq + 1

    elif msg_type == MSG_SCRIPT_END:
        if payload:
            script_buf.extend(payload)

        code = bytes(script_buf).decode('utf-8')
        print('Received script ({} bytes), executing...'.format(len(code)))

        result = execute_script(code)
        print('Result ({} bytes): {}'.format(len(result), result[:100]))

        if peer_mac:
            send_result(peer_mac, result)

        # Reset state
        script_buf = bytearray()
        receiving = False
