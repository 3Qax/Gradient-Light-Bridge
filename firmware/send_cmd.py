import serial
import sys

cmd = sys.argv[1] if len(sys.argv) > 1 else 'leave'
with serial.Serial('/dev/ttyACM0', 115200, timeout=1) as ser:
    ser.write((cmd + '\n').encode())
    print(f'Sent: {cmd}')
