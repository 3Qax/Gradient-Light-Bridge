import sys
import serial
import time
import threading

port = '/dev/ttyACM0'
baud = 115200

def reader(ser):
    while True:
        try:
            line = ser.readline().decode('utf-8', errors='replace')
            if line:
                sys.stdout.write(line)
                sys.stdout.flush()
        except Exception as e:
            print('READ ERR', e, file=sys.stderr)
            break

with serial.Serial(port, baud, timeout=0.1) as ser:
    t = threading.Thread(target=reader, args=(ser,), daemon=True)
    t.start()

    print('>>> monitor started; press Enter to send "discover" and begin re-pairing', file=sys.stderr)
    input()
    ser.write(b'discover\n')
    print('>>> discover sent', file=sys.stderr)

    # Keep reading until Ctrl-C
    try:
        while True:
            time.sleep(1)
    except KeyboardInterrupt:
        pass
