#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "pico/stdlib.h"
#include "hardware/pwm.h"
#include "hardware/clocks.h"

#ifndef PWM_GPIO
#define PWM_GPIO 15
#endif

#ifndef DEFAULT_PWM_FREQ_HZ
#define DEFAULT_PWM_FREQ_HZ 1000
#endif

#define PWM_WRAP 65535u

static uint pwm_gpio = PWM_GPIO;
static uint pwm_slice;
static uint pwm_channel;

static uint32_t pwm_freq_hz = DEFAULT_PWM_FREQ_HZ;
static uint16_t pwm_level = 0;

static void trim_line(char *s)
{
    size_t len = strlen(s);

    while (len > 0 && (s[len - 1] == '\n' || s[len - 1] == '\r' || isspace((unsigned char)s[len - 1]))) {
        s[len - 1] = '\0';
        len--;
    }

    while (*s && isspace((unsigned char)*s)) {
        memmove(s, s + 1, strlen(s));
    }
}

static void uppercase(char *s)
{
    while (*s) {
        *s = (char)toupper((unsigned char)*s);
        s++;
    }
}

static void pwm_apply_frequency(uint32_t freq_hz)
{
    if (freq_hz == 0) {
        freq_hz = 1;
    }

    uint32_t sys_hz = clock_get_hz(clk_sys);

    /*
     * PWM frequency approximately:
     *
     *   f_pwm = clk_sys / (divider * (wrap + 1))
     *
     * We use 16-bit wrap for high duty resolution.
     */
    float divider = (float)sys_hz / ((float)freq_hz * (float)(PWM_WRAP + 1u));

    /*
     * Hardware divider has practical limits.
     * Keep this conservative.
     */
    if (divider < 1.0f) {
        divider = 1.0f;
    }

    if (divider > 255.0f) {
        divider = 255.0f;
    }

    pwm_set_clkdiv(pwm_slice, divider);
    pwm_set_wrap(pwm_slice, PWM_WRAP);
    pwm_freq_hz = freq_hz;
}

static void pwm_set_level_u16(uint16_t level)
{
    pwm_level = level;
    pwm_set_chan_level(pwm_slice, pwm_channel, pwm_level);
}

static void pwm_init_output(void)
{
    gpio_set_function(pwm_gpio, GPIO_FUNC_PWM);

    pwm_slice = pwm_gpio_to_slice_num(pwm_gpio);
    pwm_channel = pwm_gpio_to_channel(pwm_gpio);

    pwm_config cfg = pwm_get_default_config();
    pwm_config_set_wrap(&cfg, PWM_WRAP);

    pwm_init(pwm_slice, &cfg, false);

    pwm_apply_frequency(DEFAULT_PWM_FREQ_HZ);
    pwm_set_level_u16(0);

    pwm_set_enabled(pwm_slice, true);
}

static int parse_u32(const char *s, uint32_t *out)
{
    char *end = NULL;
    unsigned long value = strtoul(s, &end, 10);

    if (s == end) {
        return 0;
    }

    while (*end) {
        if (!isspace((unsigned char)*end)) {
            return 0;
        }
        end++;
    }

    *out = (uint32_t)value;
    return 1;
}

static void print_status(void)
{
    float percent = ((float)pwm_level * 100.0f) / 65535.0f;

    printf("OK PWM gpio=%u level=%u percent=%.2f freq=%lu\n",
           pwm_gpio,
           pwm_level,
           percent,
           (unsigned long)pwm_freq_hz);
}

static void handle_command(char *line)
{
    trim_line(line);

    if (line[0] == '\0') {
        return;
    }

    char cmd[32] = {0};
    char arg[64] = {0};

    int n = sscanf(line, "%31s %63s", cmd, arg);
    if (n <= 0) {
        return;
    }

    uppercase(cmd);

    if (strcmp(cmd, "PING") == 0) {
        printf("PONG\n");
        return;
    }

    if (strcmp(cmd, "HELP") == 0) {
        printf("OK commands: PING, GET, SET 0..65535, SET8 0..255, PERCENT 0..100, FREQ hz\n");
        return;
    }

    if (strcmp(cmd, "GET") == 0) {
        print_status();
        return;
    }

    if (strcmp(cmd, "SET") == 0) {
        uint32_t value;

        if (n < 2 || !parse_u32(arg, &value) || value > 65535u) {
            printf("ERR SET requires 0..65535\n");
            return;
        }

        pwm_set_level_u16((uint16_t)value);
        print_status();
        return;
    }

    if (strcmp(cmd, "SET8") == 0) {
        uint32_t value;

        if (n < 2 || !parse_u32(arg, &value) || value > 255u) {
            printf("ERR SET8 requires 0..255\n");
            return;
        }

        uint16_t level = (uint16_t)((value * 65535u) / 255u);
        pwm_set_level_u16(level);
        print_status();
        return;
    }

    if (strcmp(cmd, "PERCENT") == 0) {
        char *end = NULL;
        float percent = strtof(arg, &end);

        if (n < 2 || arg == end || percent < 0.0f || percent > 100.0f) {
            printf("ERR PERCENT requires 0..100\n");
            return;
        }

        uint16_t level = (uint16_t)((percent / 100.0f) * 65535.0f + 0.5f);
        pwm_set_level_u16(level);
        print_status();
        return;
    }

    if (strcmp(cmd, "FREQ") == 0) {
        uint32_t value;

        if (n < 2 || !parse_u32(arg, &value) || value < 1u || value > 1000000u) {
            printf("ERR FREQ requires 1..1000000\n");
            return;
        }

        pwm_apply_frequency(value);
        pwm_set_level_u16(pwm_level);
        print_status();
        return;
    }

    printf("ERR unknown command. Try HELP\n");
}

int main(void)
{
    stdio_init_all();

    pwm_init_output();

    /*
     * Ingen blockering på USB-anslutning.
     * CLI kan ansluta när /dev/ttyACM0 finns.
     */
    printf("Pico USB PWM ready. GPIO=%u. Type HELP.\n", pwm_gpio);

    char line[128];

    while (true) {
        if (fgets(line, sizeof(line), stdin) != NULL) {
            handle_command(line);
        }

        tight_loop_contents();
    }

    return 0;
}
