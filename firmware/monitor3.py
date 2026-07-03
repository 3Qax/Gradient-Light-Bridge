import sys
import serial
import time

port = '/dev/ttyACM0'
baud = 115200

with serial.Serial(port, baud, timeout=0.1) as ser:
    start = time.time()
    while time.time() - start < 20:
        try:
            line = ser.readline().decode('utf-8', errors='replace')
            if line:
                sys.stdout.write(line)
                sys.stdout.flush()
        except Exception as e:
            print('ERR', e)
            break
