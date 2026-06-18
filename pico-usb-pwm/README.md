# Pico USB PWM Controller

Stabil PWM‑styrning via USB (CDC) från Linux/Ubuntu till Raspberry Pi Pico / Pico 2 / Pico W.

## Features
- 16-bit PWM
- USB CDC kommunikation
- Python CLI
- Justerbar frekvens
- Ramp/fade stöd

## Bygg
```bash
mkdir build
cd build
cmake .. -DPICO_BOARD=pico2_w
make -j
```

## Flash
Håll BOOTSEL och kopiera UF2:
```bash
cp pico_usb_pwm.uf2 /media/$USER/RPI-RP2/
```

## Python CLI
```bash
./pico_pwm.py --ping
./pico_pwm.py --set8 128
./pico_pwm.py --percent 50
./pico_pwm.py --freq 25000
```

## Test via terminal
```bash
picocom /dev/ttyACM0 -b 115200
```

Kommandon:
PING
GET
SET 0..65535
SET8 0..255
PERCENT 0..100
FREQ hz

## Koppling
GPIO15 -> resistor -> LED -> GND

För laster: använd MOSFET.

## Notes
- Fungerar stabilt med USB3
- Ingen V-USB behövs
- Hardware PWM i RP2040

