## 📦 CLI Command Reference

| Command   | Argument        | Description                     | Example            | Response |
|-----------|----------------|---------------------------------|--------------------|----------|
| `PING`    | –              | Test device connection          | `PING`             | `PONG` |
| `GET`     | –              | Read current PWM status         | `GET`              | `OK PWM gpio=15 level=32768 percent=50.00 freq=1000` |
| `HELP`    | –              | Show available commands         | `HELP`             | Command list |
| `SET`     | 0–65535        | Set PWM duty (16-bit)           | `SET 32768`        | `OK PWM ...` |
| `SET8`    | 0–255          | Set PWM duty (8-bit)            | `SET8 128`         | `OK PWM ...` |
| `PERCENT` | 0–100          | Set PWM duty in percent         | `PERCENT 25`       | `OK PWM ...` |
| `FREQ`    | 1–1000000 Hz   | Set PWM frequency               | `FREQ 25000`       | `OK PWM ...` |

---

## 🧮 PWM Value Mapping

| Method     | Resolution | Mapping |
|------------|-----------|--------|
| `SET`      | 16-bit    | Direct (0–65535) |
| `SET8`     | 8-bit     | Scaled to 16-bit |
| `PERCENT`  | float     | % → 16-bit |

---

## 📊 Usage Examples

| Use Case              | Command |
|----------------------|--------|
| Turn off PWM         | `SET 0` |
| 50% duty (precise)   | `SET 32768` |
| 50% duty (simple)    | `SET8 128` |
| 25% brightness       | `PERCENT 25` |
| Fan control          | `FREQ 25000` |
| LED dimming          | `SET8 64` |

---

## 🔁 Ramp / Fade (Python CLI)

| Parameter | Description              | Example |
|----------|--------------------------|--------|
| START    | Start value (0–255)      | `0` |
| STOP     | End value (0–255)        | `255` |
| STEP     | Step size                | `5` |
| delay    | Delay between steps (s)  | `0.01` |

Example:

```bash
./pico_pwm.py --ramp8 0 255 5 --delay 0.01
``