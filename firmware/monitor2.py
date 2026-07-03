import sys
import serial
import time

port = '/dev/ttyACM0'
baud = 115200

with serial.Serial(port, baud, timeout=0.1) as ser:
    start = time.time()
    command_sent = False
    while time.time() - start < 40:
        try:
            line = ser.readline().decode('utf-8', errors='replace')
            if line:
                sys.stdout.write(line)
                sys.stdout.flush()
            if not command_sent and time.time() - start > 3:
                ser.write(b'discover\n')
                sys.stdout.write('>>> discover\n')
                sys.stdout.flush()
                command_sent = True
        except Exception as e:
            print('ERR', e)
            break
