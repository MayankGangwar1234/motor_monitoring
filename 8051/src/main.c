/*
 * First-year 8051 trainer-kit experiment
 * 12 V DC motor START/STOP/RESET, simulated trip and RPM display on UART.
 * Target: AT89S52/AT89C52 compatible MCU, 11.0592 MHz, Keil C51.
 */

#include <REG51.H>

sbit MOTOR_OUT = P1^0;  /* HIGH enables FR120N module */
sbit BUZZER    = P1^1;  /* active HIGH module */
sbit LED_GREEN = P1^2;
sbit LED_YELLOW= P1^3;
sbit LED_RED   = P1^4;

sbit START_BUTTON = P3^3; /* NO to GND: LOW when pressed */
sbit STOP_HEALTHY = P3^4; /* NC to GND: LOW when healthy */
sbit RESET_BUTTON = P3^5; /* NO to GND: LOW when pressed */
sbit FAULT_INPUT  = P3^6; /* NO to GND: LOW means fault */

volatile unsigned int rpm_pulses = 0;
bit motor_running = 0;
bit trip_latched = 0;

void delay_10ms(void)
{
    /* 11.0592 MHz / 12 = 921600 timer counts/s.
       10 ms needs 9216 counts; reload = 65536 - 9216 = 0xDC00. */
    TMOD = (TMOD & 0xF0) | 0x01; /* Timer 0, mode 1; preserve Timer 1 UART */
    TH0 = 0xDC;
    TL0 = 0x00;
    TF0 = 0;
    TR0 = 1;
    while (TF0 == 0) { }
    TR0 = 0;
    TF0 = 0;
}

void debounce_20ms(void)
{
    delay_10ms();
    delay_10ms();
}

void uart_init(void)
{
    SCON = 0x50;  /* UART mode 1, receiver enabled */
    TMOD = (TMOD & 0x0F) | 0x20; /* Timer 1, mode 2 */
    TH1 = 0xFD;   /* 9600 baud at 11.0592 MHz */
    TL1 = 0xFD;
    TR1 = 1;
}

void uart_putc(char value)
{
    SBUF = value;
    while (TI == 0) { }
    TI = 0;
}

void uart_puts(char *text)
{
    while (*text != '\0') {
        uart_putc(*text++);
    }
}

void uart_put_uint(unsigned int value)
{
    char digits[5];
    unsigned char count = 0;

    if (value == 0) {
        uart_putc('0');
        return;
    }

    while (value > 0 && count < 5) {
        digits[count++] = (value % 10) + '0';
        value = value / 10;
    }
    while (count > 0) {
        uart_putc(digits[--count]);
    }
}

void set_outputs(void)
{
    MOTOR_OUT = motor_running && !trip_latched;
    LED_GREEN = motor_running && !trip_latched;
    LED_YELLOW = !motor_running && !trip_latched;
    LED_RED = trip_latched;
    BUZZER = trip_latched;
}

void trip_motor(void)
{
    motor_running = 0;
    trip_latched = 1;
    set_outputs();
}

bit inputs_are_safe(void)
{
    return (STOP_HEALTHY == 0) && (FAULT_INPUT == 1);
}

void rpm_interrupt(void) interrupt 0
{
    rpm_pulses++;
}

void inputs_init(void)
{
    /* Writing 1 makes standard 8051 port pins act as inputs. */
    START_BUTTON = 1;
    STOP_HEALTHY = 1;
    RESET_BUTTON = 1;
    FAULT_INPUT = 1;
    P3 |= 0x04; /* P3.2 / INT0 input */

    IT0 = 1; /* INT0 falling-edge triggered */
    EX0 = 1;
    EA = 1;
}

void main(void)
{
    unsigned int one_second_ticks = 0;
    unsigned int pulses;
    unsigned int rpm;
    bit start_was_pressed = 0;
    bit reset_was_pressed = 0;

    MOTOR_OUT = 0; /* fail-safe motor OFF during startup */
    BUZZER = 0;
    motor_running = 0;
    trip_latched = 0;

    uart_init();
    inputs_init();
    set_outputs();
    uart_puts("8051 motor experiment READY\r\n");

    while (1) {
        /* STOP is fail-safe: pressed or broken/open wire reads HIGH. */
        if (!inputs_are_safe()) {
            trip_motor();
        }

        if (START_BUTTON == 0 && !start_was_pressed) {
            debounce_20ms();
            if (START_BUTTON == 0 && !trip_latched && inputs_are_safe()) {
                motor_running = 1;
                uart_puts("Motor STARTED\r\n");
            }
        }
        start_was_pressed = (START_BUTTON == 0);

        if (RESET_BUTTON == 0 && !reset_was_pressed) {
            debounce_20ms();
            if (RESET_BUTTON == 0 && trip_latched && inputs_are_safe()) {
                trip_latched = 0;
                motor_running = 0; /* RESET never starts the motor */
                uart_puts("Trip RESET; press START\r\n");
            }
        }
        reset_was_pressed = (RESET_BUTTON == 0);

        set_outputs();
        delay_10ms();
        one_second_ticks++;

        if (one_second_ticks >= 100) {
            EX0 = 0;
            pulses = rpm_pulses;
            rpm_pulses = 0;
            EX0 = 1;

            rpm = pulses * 60; /* one reflective mark = one pulse/revolution */
            uart_puts("RPM = ");
            uart_put_uint(rpm);
            uart_puts("\r\n");
            one_second_ticks = 0;
        }
    }
}
