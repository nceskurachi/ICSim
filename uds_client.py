import can
import time

VCAN_IFACE = 'vcan0'
UDS_REQ_ID = 0x7DF     # UDS request (functional)
UDS_RESP_ID = 0x7E8    # ECU response ID
SECURITY_ACCESS_SID = 0x27
SUBFUNC_SEED = 0x01
SUBFUNC_KEY = 0x02
XOR_KEY = 0xAA         # ECU側と一致させること！

def request_seed(bus):
    msg = can.Message(arbitration_id=UDS_REQ_ID,
                      data=[SECURITY_ACCESS_SID, SUBFUNC_SEED],
                      is_extended_id=False)
    bus.send(msg)
    print("[INFO] Sent seed request (0x27 0x01)")

def wait_for_seed(bus, timeout=2.0):
    start = time.time()
    while time.time() - start < timeout:
        msg = bus.recv(timeout=0.1)
        if msg:
            print(f"[DEBUG] Received: ID=0x{msg.arbitration_id:X}, Data={msg.data.hex()}")
        if (msg and msg.arbitration_id == UDS_RESP_ID and
            msg.data[0] == 0x67 and msg.data[1] == SUBFUNC_SEED):
            seed = msg.data[2]
            print(f"[INFO] Received seed: 0x{seed:02X}")
            return seed
    raise TimeoutError("Seed (0x67 0x01) not received")

def calculate_key(seed):
    return seed ^ XOR_KEY

def send_key(bus, key):
    msg = can.Message(arbitration_id=UDS_REQ_ID,
                      data=[SECURITY_ACCESS_SID, SUBFUNC_KEY, key],
                      is_extended_id=False)
    bus.send(msg)
    print(f"[INFO] Sent key (0x27 0x02): 0x{key:02X}")

def wait_for_result(bus, timeout=2.0):
    start = time.time()
    while time.time() - start < timeout:
        msg = bus.recv(timeout=0.1)
        if msg and msg.arbitration_id == UDS_RESP_ID:
            if msg.data[0] == 0x67 and msg.data[1] == SUBFUNC_KEY:
                print("SecurityAccess success: ECU unlocked")
                return True
            elif msg.data[0] == 0x7F:
                print(f"SecurityAccess failed: NRC=0x{msg.data[2]:02X}")
                return False
    print("No final response received")
    return False

def main():
    bus = can.interface.Bus(channel=VCAN_IFACE, interface='socketcan')

    request_seed(bus)
    try:
        seed = wait_for_seed(bus)
    except TimeoutError as e:
        print(e)
        return

    key = calculate_key(seed)
    send_key(bus, key)
    wait_for_result(bus)
    
if __name__ == "__main__":
    main()
