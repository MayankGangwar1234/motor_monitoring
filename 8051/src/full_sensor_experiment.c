/*
 * Experiment 2 - Full sensor integration on an 8051 trainer kit
 * Target: AT89S52/AT89C52, 11.0592 MHz, Keil C51.
 *
 * Sensors: INA219, MPU6050, DS18B20, TCRT5000 and SSD1306 OLED.
 * Control: FR120N motor output, START, fail-safe STOP, RESET, LEDs and buzzer.
 *
 * This fixed-point teaching example intentionally has no Wi-Fi, MQTT or FFT.
 * Tune all trip thresholds from measured healthy values before demonstration.
 */

#include <REG51.H>
#include <INTRINS.H>

typedef unsigned char u8;
typedef signed char s8;
typedef unsigned int u16;
typedef signed int s16;
typedef unsigned long u32;
typedef signed long s32;

#define INA219_ADDRESS       0x40
#define MPU6050_ADDRESS      0x68
#define OLED_ADDRESS         0x3C

#define CURRENT_TRIP_MA      1200U
#define TEMPERATURE_TRIP_X10 600
#define VIBRATION_TRIP_RAW   2500U

sbit MOTOR_OUT   = P1^0;
sbit BUZZER      = P1^1;
sbit LED_GREEN   = P1^2;
sbit LED_YELLOW  = P1^3;
sbit LED_RED     = P1^4;
sbit DS18B20_DQ  = P1^5;

sbit I2C_SDA = P2^0;
sbit I2C_SCL = P2^1;

sbit START_BUTTON = P3^3; /* NO to GND */
sbit STOP_HEALTHY = P3^4; /* NC to GND: LOW is healthy */
sbit RESET_BUTTON = P3^5; /* NO to GND */
sbit FAULT_INPUT  = P3^6; /* NO to GND: LOW simulates fault */

volatile u16 rpm_pulses = 0;
u16 rpm = 0;
u16 bus_voltage_mv = 0;
u16 motor_current_ma = 0;
u16 vibration_raw = 0;
s16 temperature_x10 = 0;
s16 accel_x = 0;
s16 accel_y = 0;
s16 accel_z = 0;

bit motor_running = 0;
bit trip_latched = 0;
bit ina_ok = 0;
bit mpu_ok = 0;
bit ds_ok = 0;
bit oled_ok = 0;
bit temperature_valid = 0;

/* Character order: space, 0-9, A-Z, colon, hyphen, dot, slash. */
code char FONT_CHARS[] = " 0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ:-./";
code u8 FONT5X7[][5] = {
    {0x00,0x00,0x00,0x00,0x00},
    {0x3E,0x51,0x49,0x45,0x3E},{0x00,0x42,0x7F,0x40,0x00},
    {0x42,0x61,0x51,0x49,0x46},{0x21,0x41,0x45,0x4B,0x31},
    {0x18,0x14,0x12,0x7F,0x10},{0x27,0x45,0x45,0x45,0x39},
    {0x3C,0x4A,0x49,0x49,0x30},{0x01,0x71,0x09,0x05,0x03},
    {0x36,0x49,0x49,0x49,0x36},{0x06,0x49,0x49,0x29,0x1E},
    {0x7E,0x11,0x11,0x11,0x7E},{0x7F,0x49,0x49,0x49,0x36},
    {0x3E,0x41,0x41,0x41,0x22},{0x7F,0x41,0x41,0x22,0x1C},
    {0x7F,0x49,0x49,0x49,0x41},{0x7F,0x09,0x09,0x09,0x01},
    {0x3E,0x41,0x49,0x49,0x7A},{0x7F,0x08,0x08,0x08,0x7F},
    {0x00,0x41,0x7F,0x41,0x00},{0x20,0x40,0x41,0x3F,0x01},
    {0x7F,0x08,0x14,0x22,0x41},{0x7F,0x40,0x40,0x40,0x40},
    {0x7F,0x02,0x0C,0x02,0x7F},{0x7F,0x04,0x08,0x10,0x7F},
    {0x3E,0x41,0x41,0x41,0x3E},{0x7F,0x09,0x09,0x09,0x06},
    {0x3E,0x41,0x51,0x21,0x5E},{0x7F,0x09,0x19,0x29,0x46},
    {0x46,0x49,0x49,0x49,0x31},{0x01,0x01,0x7F,0x01,0x01},
    {0x3F,0x40,0x40,0x40,0x3F},{0x1F,0x20,0x40,0x20,0x1F},
    {0x3F,0x40,0x38,0x40,0x3F},{0x63,0x14,0x08,0x14,0x63},
    {0x07,0x08,0x70,0x08,0x07},{0x61,0x51,0x49,0x45,0x43},
    {0x00,0x36,0x36,0x00,0x00},{0x08,0x08,0x08,0x08,0x08},
    {0x00,0x60,0x60,0x00,0x00},{0x20,0x10,0x08,0x04,0x02}
};
code u8 OLED_INIT_SEQUENCE[] = {
    0xAE,0x20,0x00,0xB0,0xC8,0x00,0x10,0x40,0x81,0x7F,
    0xA1,0xA6,0xA8,0x3F,0xA4,0xD3,0x00,0xD5,0x80,0xD9,
    0xF1,0xDA,0x12,0xDB,0x40,0x8D,0x14,0xAF
};

void delay_10ms(void)
{
    TMOD = (TMOD & 0xF0) | 0x01;
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
    SCON = 0x50;
    TMOD = (TMOD & 0x0F) | 0x20;
    TH1 = 0xFD;
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
    while (*text != '\0') uart_putc(*text++);
}

void uart_put_uint(u16 value)
{
    char digits[5];
    u8 count = 0;
    if (value == 0) {
        uart_putc('0');
        return;
    }
    while (value > 0 && count < 5) {
        digits[count++] = (char)((value % 10U) + '0');
        value /= 10U;
    }
    while (count > 0) uart_putc(digits[--count]);
}

void i2c_delay(void)
{
    _nop_(); _nop_(); _nop_(); _nop_();
}

void i2c_start(void)
{
    I2C_SDA = 1; I2C_SCL = 1; i2c_delay();
    I2C_SDA = 0; i2c_delay(); I2C_SCL = 0;
}

void i2c_stop(void)
{
    I2C_SDA = 0; I2C_SCL = 1; i2c_delay();
    I2C_SDA = 1; i2c_delay();
}

bit i2c_write(u8 value)
{
    u8 bit_count;
    bit acknowledged;
    for (bit_count = 0; bit_count < 8; bit_count++) {
        I2C_SDA = (value & 0x80U) ? 1 : 0;
        i2c_delay(); I2C_SCL = 1; i2c_delay(); I2C_SCL = 0;
        value <<= 1;
    }
    I2C_SDA = 1;
    i2c_delay(); I2C_SCL = 1; i2c_delay();
    acknowledged = (I2C_SDA == 0);
    I2C_SCL = 0;
    return acknowledged;
}

u8 i2c_read(bit send_ack)
{
    u8 bit_count;
    u8 value = 0;
    I2C_SDA = 1;
    for (bit_count = 0; bit_count < 8; bit_count++) {
        value <<= 1;
        I2C_SCL = 1; i2c_delay();
        if (I2C_SDA) value |= 1U;
        I2C_SCL = 0; i2c_delay();
    }
    I2C_SDA = send_ack ? 0 : 1;
    I2C_SCL = 1; i2c_delay(); I2C_SCL = 0;
    I2C_SDA = 1;
    return value;
}

bit i2c_write_reg(u8 address, u8 reg, u8 value)
{
    bit ok;
    i2c_start();
    ok = i2c_write((u8)(address << 1));
    ok = ok && i2c_write(reg);
    ok = ok && i2c_write(value);
    i2c_stop();
    return ok;
}

bit i2c_write_reg16(u8 address, u8 reg, u16 value)
{
    bit ok;
    i2c_start();
    ok = i2c_write((u8)(address << 1));
    ok = ok && i2c_write(reg);
    ok = ok && i2c_write((u8)(value >> 8));
    ok = ok && i2c_write((u8)value);
    i2c_stop();
    return ok;
}

bit i2c_read_regs(u8 address, u8 reg, u8 *data, u8 length)
{
    u8 index;
    bit ok;
    i2c_start();
    ok = i2c_write((u8)(address << 1));
    ok = ok && i2c_write(reg);
    i2c_start();
    ok = ok && i2c_write((u8)((address << 1) | 1U));
    if (!ok) {
        i2c_stop();
        return 0;
    }
    for (index = 0; index < length; index++) {
        data[index] = i2c_read(index < (u8)(length - 1U));
    }
    i2c_stop();
    return 1;
}

bit i2c_read_reg16(u8 address, u8 reg, u16 *value)
{
    u8 bytes[2];
    if (!i2c_read_regs(address, reg, bytes, 2)) return 0;
    *value = ((u16)bytes[0] << 8) | bytes[1];
    return 1;
}

bit ina219_init(void)
{
    bit ok;
    ok = i2c_write_reg16(INA219_ADDRESS, 0x00, 0x399F);
    ok = ok && i2c_write_reg16(INA219_ADDRESS, 0x05, 0x1000);
    return ok;
}

bit ina219_read(void)
{
    u16 raw_bus;
    u16 raw_current_u;
    s16 raw_current;
    if (!i2c_read_reg16(INA219_ADDRESS, 0x02, &raw_bus)) return 0;
    if (!i2c_read_reg16(INA219_ADDRESS, 0x04, &raw_current_u)) return 0;
    bus_voltage_mv = (u16)((raw_bus >> 3) * 4U);
    raw_current = (s16)raw_current_u;
    if (raw_current < 0) raw_current = (s16)(-raw_current);
    motor_current_ma = (u16)raw_current / 10U; /* 0.1 mA/bit with 0x1000 calibration */
    return 1;
}

bit mpu6050_init(void)
{
    bit ok;
    ok = i2c_write_reg(MPU6050_ADDRESS, 0x6B, 0x00);
    ok = ok && i2c_write_reg(MPU6050_ADDRESS, 0x1C, 0x00);
    ok = ok && i2c_write_reg(MPU6050_ADDRESS, 0x1A, 0x03);
    return ok;
}

u16 abs16(s16 value)
{
    return (value < 0) ? (u16)(-value) : (u16)value;
}

bit mpu6050_read(void)
{
    static bit first_sample = 1;
    static s16 previous_x = 0;
    static s16 previous_y = 0;
    static s16 previous_z = 0;
    u8 bytes[6];
    u32 change;
    if (!i2c_read_regs(MPU6050_ADDRESS, 0x3B, bytes, 6)) return 0;
    accel_x = (s16)(((u16)bytes[0] << 8) | bytes[1]);
    accel_y = (s16)(((u16)bytes[2] << 8) | bytes[3]);
    accel_z = (s16)(((u16)bytes[4] << 8) | bytes[5]);
    if (first_sample) {
        vibration_raw = 0;
        first_sample = 0;
    } else {
        change = (u32)abs16((s16)(accel_x - previous_x));
        change += (u32)abs16((s16)(accel_y - previous_y));
        change += (u32)abs16((s16)(accel_z - previous_z));
        vibration_raw = (u16)(change / 3UL);
    }
    previous_x = accel_x;
    previous_y = accel_y;
    previous_z = accel_z;
    return 1;
}

/* One-Wire loop timing is for 11.0592 MHz Keil C51. Verify on the lab kit. */
void ow_delay(u16 loops)
{
    while (loops-- > 0) _nop_();
}

bit ow_reset(void)
{
    bit present;
    bit previous_ea = EA;
    EA = 0;
    DS18B20_DQ = 0; ow_delay(160);
    DS18B20_DQ = 1; ow_delay(20);
    present = (DS18B20_DQ == 0);
    ow_delay(150);
    EA = previous_ea;
    return present;
}

void ow_write_bit(bit value)
{
    bit previous_ea = EA;
    EA = 0;
    DS18B20_DQ = 0;
    if (value) {
        ow_delay(2); DS18B20_DQ = 1; ow_delay(20);
    } else {
        ow_delay(20); DS18B20_DQ = 1; ow_delay(2);
    }
    EA = previous_ea;
}

bit ow_read_bit(void)
{
    bit value;
    bit previous_ea = EA;
    EA = 0;
    DS18B20_DQ = 0; ow_delay(2);
    DS18B20_DQ = 1; ow_delay(4);
    value = DS18B20_DQ;
    ow_delay(16);
    EA = previous_ea;
    return value;
}

void ow_write_byte(u8 value)
{
    u8 bit_count;
    for (bit_count = 0; bit_count < 8; bit_count++) {
        ow_write_bit(value & 1U);
        value >>= 1;
    }
}

u8 ow_read_byte(void)
{
    u8 bit_count;
    u8 value = 0;
    for (bit_count = 0; bit_count < 8; bit_count++) {
        if (ow_read_bit()) value |= (u8)(1U << bit_count);
    }
    return value;
}

bit ds18b20_start_conversion(void)
{
    if (!ow_reset()) return 0;
    ow_write_byte(0xCC);
    ow_write_byte(0x44);
    return 1;
}

bit ds18b20_read_temperature(void)
{
    u8 low_byte;
    u8 high_byte;
    s16 raw;
    if (!ow_reset()) return 0;
    ow_write_byte(0xCC);
    ow_write_byte(0xBE);
    low_byte = ow_read_byte();
    high_byte = ow_read_byte();
    raw = (s16)(((u16)high_byte << 8) | low_byte);
    temperature_x10 = (s16)(((s32)raw * 10L) / 16L);
    temperature_valid = 1;
    return 1;
}

bit oled_command(u8 command)
{
    bit ok;
    i2c_start();
    ok = i2c_write((u8)(OLED_ADDRESS << 1));
    ok = ok && i2c_write(0x00);
    ok = ok && i2c_write(command);
    i2c_stop();
    return ok;
}

bit oled_data(u8 value)
{
    bit ok;
    i2c_start();
    ok = i2c_write((u8)(OLED_ADDRESS << 1));
    ok = ok && i2c_write(0x40);
    ok = ok && i2c_write(value);
    i2c_stop();
    return ok;
}

void oled_cursor(u8 page, u8 column)
{
    oled_command((u8)(0xB0U | page));
    oled_command((u8)(0x00U | (column & 0x0FU)));
    oled_command((u8)(0x10U | (column >> 4)));
}

u8 font_index(char character)
{
    u8 index = 0;
    while (FONT_CHARS[index] != '\0') {
        if (FONT_CHARS[index] == character) return index;
        index++;
    }
    return 0;
}

void oled_putc(char character)
{
    u8 index;
    u8 column;
    index = font_index(character);
    for (column = 0; column < 5; column++) oled_data(FONT5X7[index][column]);
    oled_data(0x00);
}

void oled_puts(char *text)
{
    while (*text != '\0') oled_putc(*text++);
}

void oled_put_uint(u16 value)
{
    char digits[5];
    u8 count = 0;
    if (value == 0) {
        oled_putc('0');
        return;
    }
    while (value > 0 && count < 5) {
        digits[count++] = (char)((value % 10U) + '0');
        value /= 10U;
    }
    while (count > 0) oled_putc(digits[--count]);
}

void oled_clear(void)
{
    u8 page;
    u8 column;
    for (page = 0; page < 8; page++) {
        oled_cursor(page, 0);
        i2c_start();
        i2c_write((u8)(OLED_ADDRESS << 1));
        i2c_write(0x40);
        for (column = 0; column < 128; column++) i2c_write(0x00);
        i2c_stop();
    }
}

bit oled_init(void)
{
    u8 index;
    bit ok = 1;
    for (index = 0; index < sizeof(OLED_INIT_SEQUENCE); index++) {
        ok = ok && oled_command(OLED_INIT_SEQUENCE[index]);
    }
    if (ok) oled_clear();
    return ok;
}

void oled_show_measurements(void)
{
    if (!oled_ok) return;
    oled_clear();
    oled_cursor(0, 0);
    oled_puts("T:");
    if (temperature_x10 < 0) oled_putc('-');
    oled_put_uint((u16)(abs16(temperature_x10) / 10U));
    oled_putc('.');
    oled_put_uint((u16)(abs16(temperature_x10) % 10U));
    oled_putc('C');

    oled_cursor(2, 0);
    oled_puts("I:"); oled_put_uint(motor_current_ma); oled_puts("MA ");
    oled_puts("U:"); oled_put_uint(bus_voltage_mv); oled_puts("MV");

    oled_cursor(4, 0);
    oled_puts("RPM:"); oled_put_uint(rpm);
    oled_puts(" VIB:"); oled_put_uint(vibration_raw);

    oled_cursor(6, 0);
    oled_puts("STATE:");
    if (trip_latched) oled_puts("TRIP");
    else if (motor_running) oled_puts("RUN");
    else oled_puts("READY");
}

void set_outputs(void)
{
    MOTOR_OUT = motor_running && !trip_latched;
    LED_GREEN = motor_running && !trip_latched;
    LED_YELLOW = !motor_running && !trip_latched;
    LED_RED = trip_latched;
    BUZZER = trip_latched;
}

void trip_motor(char *reason)
{
    if (!trip_latched) {
        uart_puts("TRIP: "); uart_puts(reason); uart_puts("\r\n");
    }
    motor_running = 0;
    trip_latched = 1;
    set_outputs();
}

bit sensors_are_safe(void)
{
    if (!ina_ok || !mpu_ok || !ds_ok || !temperature_valid) return 0;
    if (motor_current_ma >= CURRENT_TRIP_MA) return 0;
    if (temperature_x10 >= TEMPERATURE_TRIP_X10) return 0;
    if (vibration_raw >= VIBRATION_TRIP_RAW) return 0;
    return 1;
}

void check_safety(void)
{
    if (STOP_HEALTHY != 0) trip_motor("STOP OPEN");
    else if (FAULT_INPUT == 0) trip_motor("FAULT BUTTON");
    else if (!ina_ok) trip_motor("INA219");
    else if (!mpu_ok) trip_motor("MPU6050");
    else if (!ds_ok) trip_motor("DS18B20");
    else if (motor_current_ma >= CURRENT_TRIP_MA) trip_motor("OVERCURRENT");
    else if (temperature_valid && temperature_x10 >= TEMPERATURE_TRIP_X10) trip_motor("TEMPERATURE");
    else if (vibration_raw >= VIBRATION_TRIP_RAW) trip_motor("VIBRATION");
}

void rpm_interrupt(void) interrupt 0
{
    rpm_pulses++;
}

void ports_init(void)
{
    MOTOR_OUT = 0;
    BUZZER = 0;
    START_BUTTON = 1;
    STOP_HEALTHY = 1;
    RESET_BUTTON = 1;
    FAULT_INPUT = 1;
    DS18B20_DQ = 1;
    I2C_SDA = 1;
    I2C_SCL = 1;
    P3 |= 0x04;
    IT0 = 1;
    EX0 = 1;
    EA = 1;
}

void print_measurements(void)
{
    uart_puts("T10="); uart_put_uint((u16)abs16(temperature_x10));
    uart_puts(" IMA="); uart_put_uint(motor_current_ma);
    uart_puts(" UMV="); uart_put_uint(bus_voltage_mv);
    uart_puts(" RPM="); uart_put_uint(rpm);
    uart_puts(" VIB="); uart_put_uint(vibration_raw);
    uart_puts("\r\n");
}

void main(void)
{
    u16 ticks_10ms = 0;
    bit start_was_pressed = 0;
    bit reset_was_pressed = 0;

    ports_init();
    uart_init();
    motor_running = 0;
    trip_latched = 0;
    set_outputs();

    ina_ok = ina219_init();
    mpu_ok = mpu6050_init();
    ds_ok = ds18b20_start_conversion();
    oled_ok = oled_init();
    uart_puts("FULL SENSOR EXPERIMENT\r\n");

    if (!ina_ok || !mpu_ok || !ds_ok) trip_motor("SENSOR INIT");

    while (1) {
        if (START_BUTTON == 0 && !start_was_pressed) {
            debounce_20ms();
            if (START_BUTTON == 0 && !trip_latched && sensors_are_safe() && STOP_HEALTHY == 0) {
                motor_running = 1;
                uart_puts("MOTOR STARTED\r\n");
            }
        }
        start_was_pressed = (START_BUTTON == 0);

        if (RESET_BUTTON == 0 && !reset_was_pressed) {
            debounce_20ms();
            if (RESET_BUTTON == 0 && trip_latched && sensors_are_safe() && STOP_HEALTHY == 0 && FAULT_INPUT == 1) {
                trip_latched = 0;
                motor_running = 0;
                uart_puts("TRIP RESET; PRESS START\r\n");
            }
        }
        reset_was_pressed = (RESET_BUTTON == 0);

        if ((ticks_10ms % 5U) == 0U) {
            mpu_ok = mpu6050_read();
        }
        if ((ticks_10ms % 10U) == 0U) {
            ina_ok = ina219_read();
        }
        if ((ticks_10ms % 50U) == 0U) {
            oled_show_measurements();
        }
        if (ticks_10ms >= 100U) {
            EX0 = 0;
            rpm = (u16)(rpm_pulses * 60U);
            rpm_pulses = 0;
            EX0 = 1;

            ds_ok = ds18b20_read_temperature();
            if (ds_ok) ds_ok = ds18b20_start_conversion();
            print_measurements();
            ticks_10ms = 0;
        }

        check_safety();
        set_outputs();
        delay_10ms();
        ticks_10ms++;
    }
}
