import can
import time

VCAN_IFACE = 'vcan0'
UDS_REQ_ID = 0x7DF     # UDS request (functional)
UDS_RESP_ID = 0x7E8    # ECU response ID
SECURITY_ACCESS_SID = 0x27
SUBFUNC_SEED = 0x01
SUBFUNC_KEY = 0x02
XOR_KEY = 0xAA         # Example XOR key for key calculation

def request_seed(bus):
    msg = can.Message(arbitration_id=UDS_REQ_ID,
                      data=[SECURITY_ACCESS_SID, SUBFUNC_SEED] + [0x00] * 6,
                      is_extended_id=False)
    bus.send(msg)
    print("[INFO] Sent seed request: 0x27 0x01")

def wait_for_seed(bus, timeout=2.0):
    start = time.time()
    while time.time() - start < timeout:
        msg = bus.recv(timeout=0.1)
        if msg:
            print(f"[DEBUG] Received: ID=0x{msg.arbitration_id:X}, Data={msg.data.hex()}")
        if (msg.arbitration_id == UDS_RESP_ID and
            msg.data[0] == 0x67 and msg.data[1] == SUBFUNC_SEED):
            seed = msg.data[2]
            print(f"[INFO] Received seed: 0x{seed:02X}")
            return seed
    raise TimeoutError("Seed (0x67 0x01) not received")

def calculate_key(seed):
    # Calculate key bytes based on the seed
    return [
        seed ^ XOR_KEY,
        (seed + 1) ^ XOR_KEY,
        (seed + 2) ^ XOR_KEY
    ]

def send_key(bus, key_bytes):
    # key_bytes: [key1, key2, key3]
    data = [SECURITY_ACCESS_SID, SUBFUNC_KEY] + key_bytes + [0x00] * (8 - 2 - 3)
    msg = can.Message(arbitration_id=UDS_REQ_ID,
                      data=data,
                      is_extended_id=False)
    bus.send(msg)
    print(f"[INFO] Sent key (0x27 0x02): {[f'0x{b:02X}' for b in key_bytes]}")

def wait_for_result(bus, timeout=2.0):
    start = time.time()
    while time.time() - start < timeout:
        msg = bus.recv(timeout=0.1)
        if msg and msg.arbitration_id == UDS_RESP_ID:
            if msg.data[0] == 0x67 and msg.data[1] == SUBFUNC_KEY:
                print("[SUCCESS] SecurityAccess succeeded: ECU unlocked")
                return True
            elif msg.data[0] == 0x7F:
                print(f"[FAILURE] SecurityAccess failed: NRC=0x{msg.data[2]:02X}")
                return False
    print("[WARN] No final response received")
    return False

def main():
    bus = can.interface.Bus(channel=VCAN_IFACE, interface='socketcan')

    request_seed(bus)
    try:
        seed = wait_for_seed(bus)
    except TimeoutError as e:
        print("[ERROR]", e)
        return

    key_bytes = calculate_key(seed)
    send_key(bus, key_bytes)
    wait_for_result(bus)

if __name__ == "__main__":
    main()
