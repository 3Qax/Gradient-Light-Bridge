import sys
import serial
import time

port = '/dev/ttyACM0'
baud = 115200

with serial.Serial(port, baud, timeout=0.1) as ser:
    ser.dtr = True
    ser.rts = True
    time.sleep(0.1)
    ser.rts = False
    time.sleep(0.5)
    ser.reset_input_buffer()
    start = time.time()
    while time.time() - start < 30:
        try:
            line = ser.readline().decode('utf-8', errors='replace')
            if line:
                sys.stdout.write(line)
                sys.stdout.flush()
        except Exception as e:
            print('ERR', e)
            break
