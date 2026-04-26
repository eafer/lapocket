#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdarg.h>

static void eeprom_monitor_dump_all(void);
struct console_monitor;
static void console_monitor_dump(struct console_monitor *mon);
static void dump_all_monitors(void);

static void dump_cpu(void);
static void print_backtrace(void);

static bool panicked = false;
static bool kbinterrupted = false;

#ifdef __unix__
#include <signal.h>

static void kbinterrupt_handler(int signum)
{
	kbinterrupted = true;
}

static void set_signal_handlers(void)
{
	struct sigaction act = { .sa_handler = kbinterrupt_handler };

	sigaction(SIGINT, &act, NULL);
}

#else	/* __unix__ */

static void set_signal_handlers(void)
{
}

#endif	/* __unix__ */

#ifdef __GNUC__
__attribute__((format(printf, 1, 2)))
#endif
void notice(const char *format, ...)
{
	va_list args;

	dump_all_monitors();
	va_start(args, format);
	vprintf(format, args);
	va_end(args);
}

/*
 * TODO: panic() should check if code is actually executing, so that it can be
 * called during disassembly, memory inspection, etc.
 */
#ifdef __GNUC__
__attribute__((format(printf, 1, 2)))
#endif
void panic(const char *format, ...)
{
	va_list args;

	if (panicked)
		return;

	dump_all_monitors();

	puts("");
	puts("");

	va_start(args, format);
	printf("Panic! ");
	vprintf(format, args);
	va_end(args);

	dump_cpu();
	puts("Backtrace:");
	print_backtrace();

	puts("");
	puts("");

	panicked = true;
}

/*
 * This file represents the CompactFlash memory card. Note that ftell() returns
 * a long, but cards at the time were small (mine was 256 MiB) and 32-bit issues
 * are rare today, so I won't worry about this.
 */
FILE *card_file = NULL;
long card_size;

/*
 * The motherboard for the Jornada 545 reads "Hewlett Packard F1796-80004
 * 0007EU206". I haven't found any documentation for it, but it seems to have
 * a number of memory-mapped registers starting at address 0xb2000000 (which is
 * actually in the P2 area mapped to physical address space so the top 3 bits
 * are ignored). I've gleaned the following from looking at the firmware code:
 */
#define MBOARD_REGS_OFF			0x12000000
#define MBOARD_COMMANDS_OFF		0x12000004
#define MBOARD_STATUS_OFF		0x12000008	/* Status flags */
#define MBOARD_BLINKCNT_OFF		0x1200001C

/* Flags for the motherboard's status register */
#define MBOARD_CARD_SLOT_EMPTY	(1U << 0)
#define MBOARD_NOT_BLINKING		(1U << 5)
#define MBOARD_BATTERY_CHARGING	(1U << 8)
/* TODO: confirm what these two actually mean */
#define MBOARD_BATTERY_OUT		(1U << 9)
#define MBOARD_AC_OUT			(1U << 10)

/* Flags for the motherboard commands register */
#define MBOARD_START_BLINKING	(1U << 4)
#define MBOARD_STOP_BLINKING	(1U << 5)

struct motherboard {
	uint16_t status;
	uint16_t blinkcnt;		/* Blinks remaining for the top light */
} motherboard = {
	/*
	 * I'm not sure about the initial blinking status but this is more
	 * convenient for testing.
	 *
	 * TODO: add an input command for battery and charger to the debugger.
	 */
	.status = MBOARD_BATTERY_OUT | MBOARD_BATTERY_CHARGING | MBOARD_NOT_BLINKING,
};

static bool is_motherboard_word_address(uint32_t addr)
{
	switch (addr) {
	case MBOARD_COMMANDS_OFF:
	case MBOARD_STATUS_OFF:
	case MBOARD_BLINKCNT_OFF:
	case 0x12000000:
	case 0x12000014:
	case 0x12000024:
	case 0x12000030:
	case 0x12000034:
	case 0x12000044:
	case 0x12000068:
	case 0x1200006c:
	case 0x12000098:
		return true;
	default:
		return false;
	}
}

/*
 * The Jornada 545 that I opened comes with a battery labeled "1JP015057676". I
 * couldn't find any info for that number, but the smaller front board reads
 * "F1798-80003", and that does appear to identify a common battery for these
 * devices. A seller has it listed as "PDA-10LI - HP F1798-80003 3.7v 1800 mAh
 * LION".
 *
 * TODO: confirm that the values look similar on my device.
 */
struct battery {
	/* This voltage times 4.17 gets reported as mV by <0x8003935C> */
	unsigned int voltage;
	/* This charge times 3300/1024 gets reported as % by <0x8003935C> */
	unsigned int charge;
} battery = {
	.voltage = 887,		/* ~3.7v */
	.charge = 31,		/* ~100% */
};

/* The 545 has a resistive touchscreen */
struct touchscreen {
	/*
	 * Coordinates of the pen, or (-1,-1) if there is no contact. These will be
	 * used directly as voltage values and fed to the A/D converter, which is
	 * not accurate, but I expect it to work after calibration. TODO: extract
	 * the calibration parameters from my device and use real voltage values.
	 */
	int x;
	int y;

	uint8_t state;	/* SCP0DT:PE0DT:SCP1DT:PE1DT */
} touchscreen = {0};

#define TOUCH_STATE_PE1DT	0x01
#define TOUCH_STATE_SCP1DT	0x02
#define TOUCH_STATE_PE0DT	0x04
#define TOUCH_STATE_SCP0DT	0x08
/*
 * I won't prentend to understand this at this point (TODO?), but these values
 * are written to SCP0DT:PE0DT:SCP1DT:PE1DT before reading from each channel of
 * the A/D converter.
 */
#define TOUCH_STATE_READ_X	(TOUCH_STATE_PE0DT | TOUCH_STATE_SCP1DT)
#define TOUCH_STATE_READ_Y	(TOUCH_STATE_SCP0DT | TOUCH_STATE_PE1DT)
#define TOUCH_STATE_NOREAD	(TOUCH_STATE_SCP0DT)

/* The analog/digital converter */
struct adconv {
	uint16_t ADDRAH;	/* A/D data register A (high) */
	uint8_t ADDRAL;		/* A/D data register A (low) */
	uint16_t ADDRBH;	/* A/D data register B (high) */
	uint8_t ADDRBL;		/* A/D data register B (low) */
	uint16_t ADDRCH;	/* A/D data register C (high) */
	uint8_t ADDRCL;		/* A/D data register C (low) */
	uint16_t ADDRDH;	/* A/D data register D (high) */
	uint8_t ADDRDL;		/* A/D data register D (low) */
	uint8_t ADCSR;		/* A/D control/status register */
	uint8_t ADCR;		/* A/D control register */
} adconv = {
	/* TODO: init all register structs like this */
	.ADCR = 0x3F, /* The manual is actually inconsistent about this (TODO) */
};

#define ADCONV_ADDRAH_OFF		0x04000080
#define ADCONV_ADDRAL_OFF		0x04000082
#define ADCONV_ADDRBH_OFF		0x04000084
#define ADCONV_ADDRBL_OFF		0x04000086
#define ADCONV_ADDRCH_OFF		0x04000088
#define ADCONV_ADDRCL_OFF		0x0400008A
#define ADCONV_ADDRDH_OFF		0x0400008C
#define ADCONV_ADDRDL_OFF		0x0400008E
#define ADCONV_ADCSR_OFF		0x04000090
#define ADCONV_ADCR_OFF			0x04000092

/* Flags of the ADCSR register */
#define ADCSR_ADF	(1U << 7)	/* A/D End Flag */
#define ADCSR_ADIE	(1U << 6)	/* A/D Interrupt Enable */
#define ADCSR_ADST	(1U << 5)	/* A/D Start */
#define ADCSR_MULTI	(1U << 4)	/* Multi Mode */
#define ADCSR_CKS	(1U << 3)	/* Clock Select */
#define ADCSR_CH2	(1U << 2)	/* Channel Select 2 */
#define ADCSR_CH1	(1U << 1)	/* Channel Select 1 */
#define ADCSR_CH0	(1U << 0)	/* Channel Select 0 */
#define ADCSR_CH_MASK	(ADCSR_CH2 | ADCSR_CH1 | ADCSR_CH0)
/* These bits can be reset to zero on a write, but not set to 1 */
#define ADCSR_UNSETTABLE_MASK	(ADCSR_ADF)

static bool is_adconv_byte_address(uint32_t addr)
{
	/* TODO: apparently each ADDR register can also be read as a word */
	switch (addr) {
	case ADCONV_ADDRAH_OFF:
	case ADCONV_ADDRAL_OFF:
	case ADCONV_ADDRBH_OFF:
	case ADCONV_ADDRBL_OFF:
	case ADCONV_ADDRCH_OFF:
	case ADCONV_ADDRCL_OFF:
	case ADCONV_ADDRDH_OFF:
	case ADCONV_ADDRDL_OFF:
	case ADCONV_ADCSR_OFF:
	case ADCONV_ADCR_OFF:
		return true;
	default:
		return false;
	}
}

static void adconv_write_byte_reg(uint32_t addr, uint8_t val)
{
	uint8_t preserved_bits;

	switch (addr) {
	case ADCONV_ADCSR_OFF:
		/* TODO: only clear ADF if it has been read */
		preserved_bits = val & ADCSR_UNSETTABLE_MASK;
		val = (val & ~preserved_bits) | (adconv.ADCSR & preserved_bits);
		if (val & ADCSR_ADIE)
			return panic("A/D interrupts not supported (0x%.2x)\n", val);
		if (val & ADCSR_ADST) {
			/* In the emulator, the digital data is available immediately */
			val |= ADCSR_ADF;
		}
		if (val & ADCSR_MULTI)
			return panic("A/D multi mode not supported (0x%.2x)\n", val);
		if ((adconv.ADCSR ^ val) & ADCSR_CKS) {
			if (adconv.ADCSR & ADCSR_ADST)
				return panic("A/D clock switch during conversion\n");
			notice("A/D conversion time set to %u states\n", val & ADCSR_CKS ? 134 : 266);
		}
		adconv.ADCSR = val;
		return;
	case ADCONV_ADCR_OFF:
		if (val)
			return panic("Unsupported ADCR configuration 0x%.4x\n", val);
		adconv.ADCR = val;
		return;
	default:
		panic("Attempted write to unsupported A/D converter register at 0x%.8x\n", addr);
		return;
	}
}

/*
 * There seem to be two channels that report the charge level, with different
 * calibration. I'm not at all confident about this... (TODO)
 */
static int charge_ch5_to_ch4(int ch5)
{
	return ch5 * 33;
}

static uint8_t adconv_read_byte_reg(uint32_t addr)
{
	unsigned int channel;

	switch (addr) {
	case ADCONV_ADCSR_OFF:
		return adconv.ADCSR;
	/*
	 * The result of an A/D conversion is a 10-bit number. 'H' registers hold
	 * the top 8 bits; 'L' registers hold the bottom 2, in the top 2 positions.
	 */
	case ADCONV_ADDRAH_OFF:
		if (!(adconv.ADCSR & ADCSR_ADST)) {
			panic("A/D data read attempt outside of conversion\n");
			return 0;
		}
		channel = adconv.ADCSR & ADCSR_CH_MASK;
		if (channel == 4)
			return charge_ch5_to_ch4(battery.charge) >> 2;
		panic("A/D ADDRA read attempt for wrong channel (%u)\n", channel);
		return 0;
	case ADCONV_ADDRAL_OFF:
		if (!(adconv.ADCSR & ADCSR_ADST)) {
			panic("A/D data read attempt outside of conversion\n");
			return 0;
		}
		channel = adconv.ADCSR & ADCSR_CH_MASK;
		if (channel == 4)
			return charge_ch5_to_ch4(battery.charge) << 6;
		panic("A/D ADDRA read attempt for wrong channel (%u)\n", channel);
		return 0;
	case ADCONV_ADDRBH_OFF:
		if (!(adconv.ADCSR & ADCSR_ADST)) {
			panic("A/D data read attempt outside of conversion\n");
			return 0;
		}
		channel = adconv.ADCSR & ADCSR_CH_MASK;
		if (channel == 1) {
			/* This is probably not accurate but it shouldn't matter... */
			if (touchscreen.y < 0 || touchscreen.state != TOUCH_STATE_READ_Y)
				return 0;
			return touchscreen.y >> 2;
		}
		if (channel == 5)
			return battery.charge >> 2;
		panic("A/D ADDRB read attempt for wrong channel (%u)\n", channel);
		return 0;
	case ADCONV_ADDRBL_OFF:
		if (!(adconv.ADCSR & ADCSR_ADST)) {
			panic("A/D data read attempt outside of conversion\n");
			return 0;
		}
		channel = adconv.ADCSR & ADCSR_CH_MASK;
		if (channel == 1) {
			if (touchscreen.y < 0 || touchscreen.state != TOUCH_STATE_READ_Y)
				return 0;
			return touchscreen.y << 6;
		}
		if (channel == 5)
			return battery.charge << 6;
		panic("A/D data read attempt for wrong channel (%u != 1)\n", channel);
		return 0;
	case ADCONV_ADDRCH_OFF:
		if (!(adconv.ADCSR & ADCSR_ADST)) {
			panic("A/D data read attempt outside of conversion\n");
			return 0;
		}
		channel = adconv.ADCSR & ADCSR_CH_MASK;
		if (channel != 2) {
			panic("A/D data read attempt for wrong channel (%u != 2)\n", channel);
			return 0;
		}
		if (touchscreen.x < 0 || touchscreen.state != TOUCH_STATE_READ_X)
			return 0;
		return touchscreen.x >> 2;
	case ADCONV_ADDRCL_OFF:
		if (!(adconv.ADCSR & ADCSR_ADST)) {
			panic("A/D data read attempt outside of conversion\n");
			return 0;
		}
		channel = adconv.ADCSR & ADCSR_CH_MASK;
		if (channel != 2) {
			panic("A/D data read attempt for wrong channel (%u != 2)\n", channel);
			return 0;
		}
		if (touchscreen.x < 0 || touchscreen.state != TOUCH_STATE_READ_X)
			return 0;
		return touchscreen.x << 6;
	case ADCONV_ADDRDH_OFF:
		if (!(adconv.ADCSR & ADCSR_ADST)) {
			panic("A/D data read attempt outside of conversion\n");
			return 0;
		}
		channel = adconv.ADCSR & ADCSR_CH_MASK;
		if (channel == 3)
			return battery.voltage >> 2;
		panic("A/D ADDRD read attempt for wrong channel (%u)\n", channel);
		return 0;
	case ADCONV_ADDRDL_OFF:
		if (!(adconv.ADCSR & ADCSR_ADST)) {
			panic("A/D data read attempt outside of conversion\n");
			return 0;
		}
		channel = adconv.ADCSR & ADCSR_CH_MASK;
		if (channel == 3)
			return battery.voltage << 6;
		panic("A/D ADDRD read attempt for wrong channel (%u)\n", channel);
		return 0;
	default:
		panic("Attempted read of unsupported A/D converter register at 0x%.8x\n", addr);
		return 0;
	}
}

/* The digital/analog converter */
struct daconv {
	uint8_t DADR0;	/* D/A data register 0 */
	uint8_t DADR1;	/* D/A data register 1 */
	uint8_t DACR;	/* D/A control register */
} daconv = {
	.DACR = 0x1F,
};

#define DACONV_DADR0_OFF		0x040000A0
#define DACONV_DADR1_OFF		0x040000A2
#define DACONV_DACR_OFF			0x040000A4

/* Flags of the DACR register */
#define DACR_DAOE1	(1U << 7)	/* D/A Output Enable 1 */
#define DACR_DAOE0	(1U << 6)	/* D/A Output Enable 0 */
#define DACR_DAE	(1U << 5)	/* D/A Enable */
#define DACR_RSVD	0x1FU		/* Reserved - always read as 1 */

static bool is_daconv_byte_address(uint32_t addr)
{
	switch (addr) {
	case DACONV_DADR0_OFF:
	case DACONV_DADR1_OFF:
	case DACONV_DACR_OFF:
		return true;
	default:
		return false;
	}
}

static void daconv_write_byte_reg(uint32_t addr, uint8_t val)
{
	/*
	 * Contrast and brightness are set as a voltage between 0 and 256 on analog
	 * output pins DA0 and DA1 respectively. So the firmware converts the
	 * percentage into this range and writes it to these registers, then we
	 * convert it back to a percentage to report it. Integer division is
	 * involved in both directions so the result won't always be exactly the
	 * same.
	 *
	 * TODO: actually apply some transformation to the display contents.
	 */
	switch (addr) {
	case DACONV_DADR0_OFF:
		daconv.DADR0 = val;
		if (daconv.DACR & DACR_DAE || daconv.DACR & DACR_DAOE0)
			notice("Contrast set to %u%%\n", daconv.DADR0 * 100 >> 8);
		return;
	case DACONV_DADR1_OFF:
		daconv.DADR1 = val;
		if (daconv.DACR & DACR_DAE || daconv.DACR & DACR_DAOE1)
			notice("Brightness set to %u%%\n", daconv.DADR1 * 100 >> 8);
		return;
	case DACONV_DACR_OFF:
		daconv.DACR = val | DACR_RSVD;
		return;
	default:
		return panic("BUG: nonexistent register for the D/A converter\n");
	}
}

static uint8_t daconv_read_byte_reg(uint32_t addr)
{
	switch (addr) {
	case DACONV_DADR0_OFF:
		return daconv.DADR0;
	case DACONV_DADR1_OFF:
		return daconv.DADR1;
	case DACONV_DACR_OFF:
		return daconv.DACR;
	default:
		panic("BUG: nonexistent register for the D/A converter\n");
		return 0;
	}
}

enum button {
	BUTTON_ONOFF	= 0,
	BUTTON_QL1		= 1,
	BUTTON_QL2		= 2,
	BUTTON_QL3		= 3,
	BUTTON_QL4		= 4,
	BUTTON_EXIT		= 5,
	BUTTON_RECORD	= 6,
	BUTTON_ENTER	= 7,
	BUTTON_DOWN		= 8,
	BUTTON_UP		= 9,
	/* Not a button... TODO: rethink this */
	BUTTON_PEN		= 10,
};

/* Set or unset a flag in a 32-bit register. TODO: reuse this function */
static void write_flag_to_long(uint32_t *reg, uint32_t flag, bool value)
{
	if (value == true)
		*reg |= flag;
	else
		*reg &= ~flag;
}

/* Set or unset a flag in an 8-bit register. TODO: reuse this function */
static void write_flag_to_byte(uint8_t *reg, uint8_t flag, bool value)
{
	if (value == true)
		*reg |= flag;
	else
		*reg &= ~flag;
}

#define BUTTON_ONOFF_PUSHED		(1U << BUTTON_ONOFF)
#define BUTTON_QL1_PUSHED		(1U << BUTTON_QL1)
#define BUTTON_QL2_PUSHED		(1U << BUTTON_QL2)
#define BUTTON_QL3_PUSHED		(1U << BUTTON_QL3)
#define BUTTON_QL4_PUSHED		(1U << BUTTON_QL4)
#define BUTTON_QLX_MASK			(BUTTON_QL1_PUSHED | BUTTON_QL2_PUSHED | BUTTON_QL3_PUSHED | BUTTON_QL4_PUSHED)
#define BUTTON_EXIT_PUSHED		(1U << BUTTON_EXIT)
#define BUTTON_RECORD_PUSHED	(1U << BUTTON_RECORD)
#define BUTTON_ENTER_PUSHED		(1U << BUTTON_ENTER)
#define BUTTON_DOWN_PUSHED		(1U << BUTTON_DOWN)
#define BUTTON_UP_PUSHED		(1U << BUTTON_UP)
/*
 * None of the buttons is pushed at first. Note that pins go low when the
 * matching button gets pushed.
 */
static uint16_t button_state = 0;
/* TODO: the front buttons share a board and should have their own struct */
static bool front4_requested = false;

enum pin_sense_mode {
	falling	= 0x0000,
	rising	= 0x0001,
	low		= 0x0002,
	high	= 0x0003,
};

enum monitor {
	MONITOR_NONE,
	MONITOR_SERIAL,
	MONITOR_XB3A,
	MONITOR_EEPROM,
};

/* For monitoring xB3A and serial only for now. TODO: infrared. */
#define CONSOLE_MON_LINE_LIMIT		72
#define CONSOLE_MON_BUF_SIZE		80

struct console_monitor {
	const char *cm_tag;
	char cm_buf[CONSOLE_MON_BUF_SIZE];
	int cm_len;
};

static struct console_monitor serial_monitor = { .cm_tag = "SERIAL" };
static struct console_monitor xB3A_monitor = { .cm_tag = "XB3A" };

/*
 * We don't want multiple monitors to buffer at the same time: that would make
 * it hard to tell what happened first. Before saving to any monitor buffer,
 * call this function to dump all the others.
 */
static void dump_monitors_except(enum monitor which)
{
	if (which != MONITOR_SERIAL)
		console_monitor_dump(&serial_monitor);
	if (which != MONITOR_XB3A)
		console_monitor_dump(&xB3A_monitor);
	if (which != MONITOR_EEPROM)
		eeprom_monitor_dump_all();
}

static void dump_all_monitors(void)
{
	return dump_monitors_except(MONITOR_NONE);
}

/*
 * We have 16 MiB - 32 KiB of physical memory at address 0x8C008000, which is
 * actually in the P2 area mapped to physical address space so the top 3 bits
 * are ignored.
 */
#define MEMORY_SIZE	(16 * 1024 * 1024)
uint8_t memory[MEMORY_SIZE] = {0};
#define MEMORY_OFF	0x0C000000
#define MEMORY_MASK	(MEMORY_SIZE - 1)
/*
 * To measure the size of the RAM, <0x8003023C> looks for a point when writes
 * either wrap around or start getting ignored. I'll implement the first for
 * now, but I haven't checked how the device actually works (TODO).
 */
#define MEMORY_SHADOW	0x0D000000

/*
 * According to the display RAM self-tests, there are 512 KiB of memory at
 * address 0xb4200000; the first 75 KiB are for the framebuffer. This is
 * all in in the P2 area so the top 3 bits are ignored.
 */
#define DISPLAY_OFF			0x14000000
#define	DISPLAY_FB_OFF		0x14200000
#define DISPLAY_FB_SIZE		(240 * 320)
#define DISPLAY_RAM_SIZE	0x00080000
struct display {
	/* Framebuffer and display ram. TODO: what is the ram for? Rename this? */
	uint8_t fb[DISPLAY_RAM_SIZE];
} display = {0};

enum i2c_state {
	I2C_STATE_STARTING,
	I2C_STATE_RECEIVING_ADDRESS_ACK,
	I2C_STATE_RECEIVING_DATA_ACK,
	I2C_STATE_RECEIVING_DATA,
	I2C_STATE_SENDING_DATA_ACK,
	I2C_STATE_SENDING_ADDRESS,
	I2C_STATE_SENDING_DATA,
	I2C_STATE_STOPPED,
};

/* State of the i2c bus used to communicate with the EEPROM */
struct i2c {
	uint8_t sda;	/* Serial data line */
	uint8_t scl;	/* Serial clock line */

	uint8_t bitcnt;	/* Bits in frame so far */
	uint8_t buffer;	/* Byte being sent/received */
	enum i2c_state state;
} i2c = {0};

static void eeprom_stop(void);
static void eeprom_start(void);
static uint8_t eeprom_receive_frame(void);
static void eeprom_deliver_frame(uint8_t frame);

static void i2c_release_sda(void)
{
	if (i2c.sda == 1)
		return;

	if (i2c.scl == 1) {
		eeprom_stop();
		i2c.state = I2C_STATE_STOPPED;
		i2c.buffer = i2c.bitcnt = 0;
	}
	/* The "pullup resistor" does this */
	i2c.sda = 1;
}

static void i2c_pull_down_sda(void)
{
	if (i2c.sda == 0)
		return;

	if (i2c.scl == 1) {
		i2c.state = I2C_STATE_STARTING;
		i2c.buffer = i2c.bitcnt = 0;
	}
	i2c.sda = 0;
}

static void i2c_pull_up_scl(void)
{
	if (i2c.scl == 1)
		return;

	/*
	 * We update the incoming sda values when the clock goes high, though in
	 * physical hardware it would have happened before... I think.
	 *
	 * TODO: what if the processor's port is not ready for reading? At least
	 * throw an error?
	 */
	switch (i2c.state) {
	case I2C_STATE_RECEIVING_ADDRESS_ACK:
	case I2C_STATE_RECEIVING_DATA_ACK:
		i2c.sda = 0; /* The ACK bit - communication always succeeds here */
		break;
	case I2C_STATE_RECEIVING_DATA:
		i2c.sda = i2c.buffer & 0x80;
		break;
	default:
	}
	i2c.scl = 1;
}

static void i2c_pull_down_scl(void)
{
	bool readwrite;

	if (i2c.scl == 0)
		return;

	switch (i2c.state) {
	case I2C_STATE_STARTING:
		eeprom_start();
		i2c.state = I2C_STATE_SENDING_ADDRESS;
		i2c.buffer = i2c.bitcnt = 0;
		break;
	case I2C_STATE_RECEIVING_ADDRESS_ACK:
		/* Address is now acked, move on to the actual data */
		readwrite = i2c.buffer & 1;
		if (readwrite == 1) {
			i2c.state = I2C_STATE_RECEIVING_DATA;
			i2c.buffer = eeprom_receive_frame();
			i2c.bitcnt = 0;
		} else {
			i2c.state = I2C_STATE_SENDING_DATA;
			i2c.buffer = i2c.bitcnt = 0;
		}
		break;
	case I2C_STATE_RECEIVING_DATA_ACK:
		i2c.state = I2C_STATE_SENDING_DATA;
		i2c.buffer = i2c.bitcnt = 0;
		break;
	case I2C_STATE_RECEIVING_DATA:
		/* Cpu should have read this bit by now */
		i2c.buffer <<= 1;
		if (++i2c.bitcnt == 8) {
			i2c.buffer = i2c.bitcnt = 0;
			i2c.state = I2C_STATE_SENDING_DATA_ACK;
		}
		break;
	case I2C_STATE_SENDING_DATA_ACK:
		if (i2c.sda != 0)
			notice("I2C NACK from cpu\n");
		/* I don't really know if more than one byte can be read at once */
		i2c.state = I2C_STATE_RECEIVING_DATA;
		break;
	case I2C_STATE_SENDING_ADDRESS:
	case I2C_STATE_SENDING_DATA:
		i2c.buffer <<= 1;
		i2c.buffer |= i2c.sda;
		if (++i2c.bitcnt == 8) {
			eeprom_deliver_frame(i2c.buffer);
			if (i2c.state == I2C_STATE_SENDING_ADDRESS)
				i2c.state = I2C_STATE_RECEIVING_ADDRESS_ACK;
			else
				i2c.state = I2C_STATE_RECEIVING_DATA_ACK;
		}
		break;
	default:
		panic("Invalid I2C sequence (state: %d)\n", i2c.state);
	}
	i2c.scl = 0;
}

/*
 * Exception registers are accessed at address range 0xFFFFFFD0-0xFFFFFFDB,
 * except for INTEVT2.
 */
#define EXCEPT_REGS_SIZE	12
#define EXCEPT_REGS_OFF		0xFFFFFFD0

#define EXCEPT_TRA_OFF		0xFFFFFFD0
#define EXCEPT_EXPEVT_OFF	0xFFFFFFD4
#define EXCEPT_INTEVT_OFF	0xFFFFFFD8
#define EXCEPT_INTEVT2_OFF	0x04000000

struct cpu {
	/* General purpose registers */
	uint32_t R[16];			/* R0-R7 are for BANK0 */
	uint32_t R_BANK1[8];	/* These are R0-R7 for BANK1 (privileged-only) */

	/* Control registers */
	uint32_t GBR;			/* Global base register */
	uint32_t SR;			/* Status register (some bits privileged-only) */
	uint32_t SSR;			/* Saved status register (privileged-only) */
	uint32_t SPC;			/* Saved program counter (privileged-only) */
	uint32_t VBR;			/* Vector base register (privileged-only) */

	/* System registers */
	uint32_t MACH, MACL;	/* Multiply and accumulate high/low */
	uint32_t PR;			/* Procedure register */
	uint32_t PC;			/* Program counter */

	/* Exception registers (TODO: do these belong here?) */
	uint32_t TRA;			/* TRAPA exception register */
	uint32_t EXPEVT;		/* Exception event register */
	uint32_t INTEVT;		/* Interrupt event register */
	uint32_t INTEVT2;		/* Interrupt event register 2 */

	uint32_t extra_state;	/* Extra state needed for the emulation */
} cpu = {0};

/* Fields of the status register */
#define SR_T_BIT	(1U << 0)	/* Carry/borrow/overflow/underflow/true */
#define SR_S_BIT	(1U << 1)	/* Used by the MAC instruction */
#define SR_I_BITS	(15U << 4)	/* Interrupt mask bits */
#define SR_Q_BIT	(1U << 8)	/* Used by division instructions */
#define SR_M_BIT	(1U << 9)	/* Used by division instructions */
#define SR_DSP_BIT	(1U << 12)	/* DSP bit - meaningless here, but can be set */
#define SR_BL_BIT	(1U << 28)	/* Block bit */
#define SR_RB_BIT	(1U << 29)	/* Register bank bit */
#define SR_MD_BIT	(1U << 30)	/* Operation mode bit */
#define SR_BIT_MASK	(SR_T_BIT | SR_S_BIT | SR_I_BITS | SR_Q_BIT | SR_M_BIT | SR_DSP_BIT | SR_BL_BIT | SR_RB_BIT | SR_MD_BIT)

#define SR_I_SHIFT	4			/* Interrupt mask shift */

/* Extra state flags */
#define EXTRA_IN_DELAYED	1U	/* Executing the instruction after a branch */
#define EXTRA_POWER_DOWN	2U	/* In power-down mode */

/*
 * The Card Information Structure (CIS) for a CompactFlash memory card is
 * accessed at the even addresses of range 0xB8001800-0xB8001968, which is
 * actually in the P2 area so the top 3 bits are ignored.
 *
 * The values given here were copied from a manual identified as "CompactFlash
 * Memory Card Product Manual, Rev. 10.0 (C) 2002 SANDISK CORPORATION". TODO:
 * try to extract the values from my actual Transcend-branded card... maybe
 * allow the user to pick the brand and other parameters?
 */
#define CF_CIS_SIZE		0x16A
#define CF_CIS_OFF		0x18001800
const uint8_t cf_cis[] = {
	/* Offsets 000h to 02Ah (page 6-1) */
	0x01, 0x04, 0xDF, 0x72, 0x01, 0xFF, 0x1C, 0x04, 0x03, 0xD9, 0x01,
	0xFF, 0x18, 0x02, 0xDF, 0x01, 0x20, 0x04, 0x45, 0x00, 0x01, 0x04,
	/* Offsets 02Ch to 060h (page 6-2) */
	0x15, 0x17, 0x04, 0x01, 0x53, 0x75, 0x6E, 0x44, 0x69, 0x73, 0x6B,
	0x00, 0x53, 0x44, 0x50, 0x00, 0x35, 0x2F, 0x33, 0x20, 0x30, 0x2E,
	0x36, 0x00, 0xFF, 0x80, 0x03,
	/* Offsets 062h to 07eh (page 6-3) */
	0x14, 0x08, 0x00, 0x21, 0x02, 0x04, 0x01, 0x22, 0x02, 0x01, 0x01,
	0x22, 0x03, 0x02, 0x0C,
	/* Offsets 080h to 092h (page 6-4) */
	0x0F, 0x1A, 0x05, 0x01, 0x07, 0x00, 0x02, 0x0F, 0x1B, 0x0B,
	/* Offsets 094h to 0A6h (page 6-5) */
	0xC0, 0xC0, 0xA1, 0x27, 0x55, 0x4D, 0x5D, 0x75, 0x08, 0x00,
	/* Offsets 0A8h to 0C0h (page 6-6) */
	0x21, 0x1B, 0x06, 0x00, 0x01, 0x21, 0xB5, 0x1E, 0x4D, 0x1B, 0x0D,
	0xC1, 0x41,
	/* Offsets 0C2h to 0D0h (page 6-7) */
	0x99, 0x27, 0x55, 0x4D, 0x5D, 0x75, 0x64, 0xF0,
	/* Offsets 0D2h to 0ECh (page 6-8) */
	0xFF, 0xFF, 0x21, 0x1B, 0x06, 0x01, 0x01, 0x21, 0xB5, 0x1E, 0x4D,
	0x1B, 0x12, 0xC2,
	/* Offsets 0EEh to 0FCh (page 6-9) */
	0x41, 0x99, 0x27, 0x55, 0x4D, 0x5D, 0x75, 0xEA,
	/* Offsets 0FEh to 114h (page 6-10) */
	0x61, 0xF0, 0x01, 0x07, 0xF6, 0x03, 0x01, 0xEE, 0x21, 0x1B, 0x06,
	0x02,
	/* Offsets 116h to 12Eh (page 6-11) */
	0x01, 0x21, 0xB5, 0x1E, 0x4D, 0x1B, 0x12, 0xC3, 0x41, 0x99, 0x27,
	0x55, 0x4D,
	/* Offsets 130h to 144h (page 6-12) */
	0x5D, 0x75, 0xEA, 0x61, 0x70, 0x01, 0x07, 0x76, 0x03, 0x01, 0xEE,
	/* Offsets 146h to 168h (page 6-13) */
	0x21, 0x1B, 0x06, 0x03, 0x01, 0x21, 0xB5, 0x1E, 0x4D, 0x1B, 0x04,
	0x07, 0x00, 0x28, 0xD3, 0x14, 0x00, 0xFF,
};
/*
 * The CIS is part of a so-called "Attribute memory" space. It goes on here
 * with the card's configuration registers.
 */
#define CF_CONFIG_OPTION_OFF	0x18001A00
#define CF_CONFIG_STATUS_OFF	0x18001A02
#define CF_PIN_REPLACEMENT_OFF	0x18001A04
#define CF_SOCKET_COPY_OFF		0x18001A06

/* Flags for the CompactFlash Configuration Option Register */
#define CF_CONFOPT_SRESET		(1U << 7)	/* Soft Reset */
#define CF_CONFOPT_LEVLREQ		(1U << 6)	/* Level/Pulse Mode Interrupt */
#define CF_CONFOPT_CONF_BITS	0x3F		/* Configuration Index */

/* Possible values for the Configuration Index */
#define CF_CONFOPT_CONF_MMAP		0x00	/* Memory Mapped */
#define CF_CONFOPT_CONF_IOMAP_ANY	0x01	/* I/O Mapped, any 16-byte system decoded boundary */
#define CF_CONFOPT_CONF_IOMAP_PRIM	0x02	/* I/O Mapped, 1F0-1F7/3F6-3F7 */
#define CF_CONFOPT_CONF_IOMAP_SECN	0x03	/* I/O Mapped, 170-177/376-377 */

static bool is_compactflash_cis_byte_address(uint32_t addr)
{
	if (addr < CF_CIS_OFF || addr >= CF_CIS_OFF + CF_CIS_SIZE)
		return false;
	if (addr & 1)
		return false;
	return true;
}

static uint8_t compactflash_cis_read_byte_reg(uint32_t addr)
{
	if (addr < CF_CIS_OFF || addr >= CF_CIS_OFF + CF_CIS_SIZE) {
		panic("BUG: Out of bounds CIS offset\n");
		return 0;
	}
	addr -= CF_CIS_OFF;
	return cf_cis[addr >> 1];
}

/*
 * CompactFlash ATA registers can be accessed in several modes, but the firmware
 * self-tests at <0x8003DA78> only cover "Memory Mode" (mapped to address range
 * 0xb8000000-0xb8000007) and "I/O mode" (mapped to address range
 * 0xba000170-0xba000177). So far I'm not aware of any difference between the
 * modes that would matter to the emulator.
 */
#define CF_MEMMODE_ATA_REGS_OFF		0x18000000
#define CF_IOMODE_ATA_REGS_OFF		0x1a000170
#define CF_ATA_REGS_SIZE			8
/*
 * The Data Register is the only 16-bit register here. According to the SanDisk
 * manual, there are two more registers that overlap with it on the second byte.
 * I haven't encountered them so far though (TODO).
 */
#define CF_MEMMODE_DATA_OFF			0x18000000
#define CF_MEMMODE_SECCNT_OFF		0x18000002
#define CF_MEMMODE_SECNUM_OFF		0x18000003
#define CF_MEMMODE_CYLLOW_OFF		0x18000004
#define CF_MEMMODE_CYLHIGH_OFF		0x18000005
#define CF_MEMMODE_CDH_OFF			0x18000006
/* This offset is the status register on read, the command register on write */
#define CF_MEMMODE_STATCOMM_OFF		0x18000007
#define CF_MEMMODE_DEVCON_OFF		0x1800000E
#define CF_IOMODE_DATA_OFF			0x1A000170
#define CF_IOMODE_SECCNT_OFF		0x1A000172
#define CF_IOMODE_SECNUM_OFF		0x1A000173
#define CF_IOMODE_CYLLOW_OFF		0x1A000174
#define CF_IOMODE_CYLHIGH_OFF		0x1A000175
#define CF_IOMODE_CDH_OFF			0x1A000176
/* This offset is the status register on read, the command register on write */
#define CF_IOMODE_STATCOMM_OFF		0x1A000177
#define CF_IOMODE_DEVCON_OFF		0x1A00017E

#define CF_SECTOR_SZ		512
#define CF_SECTOR_SZ_SHIFT	9
struct cfcard {
	/* ATA registers */
	uint8_t	feature;	/* Feature Register */
	uint8_t sec_cnt;	/* Sector Count Register */
	uint8_t sec_num;	/* Sector Number Register (or LBA 7-0) */
	uint8_t cyl_low;	/* Cylinder Low Register (or LBA 15-8) */
	uint8_t cyl_high;	/* Cylinder High Register (or LBA 23-16) */
	uint8_t cdh;		/* Drive/Head Register (or LBA 27-24) */
	uint8_t command;	/* Command register */
	uint8_t status;		/* Status & Alternate Status Registers */
	uint8_t dev_con;	/* Device Control Register */

	/* Configuration registers */
	uint8_t conf_opt;	/* Configuration Option Register */
	uint8_t conf_stat;	/* Configuration and Status Register */
	uint8_t pin_rep;	/* Pin Replacement Register */
	uint8_t sock_cp;	/* Socket and Copy Register */

	/*
	 * The sector buffer is read one byte or word at a time through the Data
	 * Register.
	 */
	int secbuf_off;
	uint8_t secbuf[CF_SECTOR_SZ];
} cfcard = {0};

/* Flags for the CompactFlash status register */
#define CF_STATUS_BUSY	(1U << 7)
#define CF_STATUS_RDY	(1U << 6)
#define CF_STATUS_DWF	(1U << 5)
#define CF_STATUS_DSC	(1U << 4)
#define CF_STATUS_DRQ	(1U << 3)
#define CF_STATUS_CORR	(1U << 2)
#define CF_STATUS_IDX	(1U << 1)
#define CF_STATUS_ERR	(1U << 0)

/* Flags for the CompactFlash device control register */
#define CF_DEVCON_SWRST		(1U << 2)
#define CF_DEVCON_IEN		(1U << 1)
#define CF_DEVCON_BIT_MASK	(CF_DEVCON_SWRST | CF_DEVCON_IEN)

/* Flags for the CompactFlash Drive/Head Register */
#define CF_CDH_CHS_OR_LBA	(1U << 6)	/* Cylinder/Head/Sector or LBA mode */
#define CF_CDH_DRV			(1U << 4)	/* Drive number */
#define CF_CDH_HS_BITS		(0x0F)		/* Head number or bits 24-27 of LBA */
#define CF_CDH_ALWAYS_ONE	(0xA0)		/* Always set to 1 */

static bool is_cfcard_ata_byte_address(uint32_t addr)
{
	switch (addr) {
	case CF_MEMMODE_DATA_OFF:
	case CF_MEMMODE_SECCNT_OFF:
	case CF_MEMMODE_SECNUM_OFF:
	case CF_MEMMODE_CYLLOW_OFF:
	case CF_MEMMODE_CYLHIGH_OFF:
	case CF_MEMMODE_CDH_OFF:
	case CF_MEMMODE_STATCOMM_OFF:
	case CF_MEMMODE_DEVCON_OFF:
	case CF_IOMODE_DATA_OFF:
	case CF_IOMODE_SECCNT_OFF:
	case CF_IOMODE_SECNUM_OFF:
	case CF_IOMODE_CYLLOW_OFF:
	case CF_IOMODE_CYLHIGH_OFF:
	case CF_IOMODE_CDH_OFF:
	case CF_IOMODE_STATCOMM_OFF:
	case CF_IOMODE_DEVCON_OFF:
		return true;
	default:
		return false;
	}
}

static bool is_cfcard_ata_word_address(uint32_t addr)
{
	switch (addr) {
	case CF_MEMMODE_DATA_OFF:
	case CF_IOMODE_DATA_OFF:
		return true;
	default:
		return false;
	}
}

static void cfcard_ata_check_address_mode(uint32_t addr)
{
	bool memmode, memmode_addr;

	memmode = (cfcard.conf_opt & CF_CONFOPT_CONF_BITS) == CF_CONFOPT_CONF_MMAP;
	memmode_addr = (addr & 0xFF000000) == 0x18000000;
	if (memmode != memmode_addr)
		panic("Incorrect CompactFlash configuration index to access address 0x%.8x\n", addr);
}

static uint8_t cfcard_ata_read_byte_reg(uint32_t addr)
{
	uint8_t val;

	cfcard_ata_check_address_mode(addr);

	switch (addr) {
	case CF_MEMMODE_DATA_OFF:
	case CF_IOMODE_DATA_OFF:
		/* Apparently the sector buffer can also be read one byte at a time */
		if (!(cfcard.status & CF_STATUS_DRQ))
			panic("Reading from CompactFlash buffer with no data request\n");
		if (cfcard.secbuf_off == CF_SECTOR_SZ)
			panic("Attempted read from empty CompactFlash sector buffer\n");
		val = cfcard.secbuf[cfcard.secbuf_off++];
		if (cfcard.secbuf_off == CF_SECTOR_SZ)
			cfcard.status &= ~CF_STATUS_DRQ; /* Not certain here (TODO) */
		return val;
	case CF_MEMMODE_STATCOMM_OFF:
	case CF_IOMODE_STATCOMM_OFF:
		return cfcard.status;
	default:
		panic("Attempted read from unsupported CompactFlash register at 0x%.8x\n", addr);
		return 0;
	}
}

static uint16_t cfcard_ata_read_word_reg(uint32_t addr)
{
	uint16_t val;

	cfcard_ata_check_address_mode(addr);

	switch (addr) {
	case CF_MEMMODE_DATA_OFF:
	case CF_IOMODE_DATA_OFF:
		if (!(cfcard.status & CF_STATUS_DRQ))
			panic("Reading from CompactFlash buffer with no data request\n");
		if (cfcard.secbuf_off >= CF_SECTOR_SZ - 1)
			panic("Attempted read from empty CompactFlash sector buffer\n");
		val = cfcard.secbuf[cfcard.secbuf_off++];
		val |= cfcard.secbuf[cfcard.secbuf_off++] << 8;
		if (cfcard.secbuf_off == CF_SECTOR_SZ)
			cfcard.status &= ~CF_STATUS_DRQ; /* Not certain here (TODO) */
		return val;
	default:
		panic("Attempted read from unsupported CompactFlash register at 0x%.8x\n", addr);
		return 0;
	}
}

static void cfcard_write_end(void)
{
	uint32_t secnum;
	size_t ret;

	secnum = cfcard.sec_num;			/* LBA 7-0 */
	secnum += cfcard.cyl_low << 8;		/* LBA 15-8 */
	secnum += cfcard.cyl_high << 16;	/* LBA 23-16 */
	secnum += (cfcard.cdh & 0x0F) << 8;	/* LBA 27-24 */

	if (!card_file)
		return panic("BUG: ATA write command accepted without a card\n");

	/* TODO: report system errors through ATA registers and keep going? */
	if (fseek(card_file, secnum << CF_SECTOR_SZ_SHIFT, SEEK_SET))
		return panic("Failed fseek() on card file (%s)\n", strerror(errno));

	ret = fwrite(cfcard.secbuf, 1, sizeof(cfcard.secbuf), card_file);
	if (ret != sizeof(cfcard.secbuf))
		return panic("Failed write to card file\n");
	/*
	 * I think it's better to flush so that I can see the changes to the card
	 * file right away with hexdump or whatever. This should never be a
	 * bottleneck anyway.
	 */
	if (fflush(card_file))
		return panic("Failed fflush() for card file\n");
}

static void cfcard_ata_write_word_reg(uint32_t addr, uint16_t val)
{
	cfcard_ata_check_address_mode(addr);

	switch (addr) {
	case CF_MEMMODE_DATA_OFF:
	case CF_IOMODE_DATA_OFF:
		if (!(cfcard.status & CF_STATUS_DRQ))
			panic("Writing to CompactFlash buffer with no data request\n");
		if (cfcard.secbuf_off >= CF_SECTOR_SZ - 1)
			panic("Attempted write to full CompactFlash sector buffer\n");
		cfcard.secbuf[cfcard.secbuf_off++] = val & 0xFF;
		cfcard.secbuf[cfcard.secbuf_off++] = val >> 8;
		if (cfcard.secbuf_off == CF_SECTOR_SZ) {
			cfcard_write_end();
			cfcard.status &= ~CF_STATUS_DRQ;
		}
		return;
	default:
		return panic("Attempted write to unsupported CompactFlash register at 0x%.8x\n", addr);
	}
}

/* Supported ATA command codes (TODO: support them all) */
#define ATA_READ_SECTOR		0x20
#define ATA_WRITE_SECTOR	0x30
#define ATA_IDENTIFY_DRIVE	0xEC

static void cfcard_read_sector(void)
{
	uint32_t secnum;
	size_t ret;

	if (!(cfcard.cdh & CF_CDH_CHS_OR_LBA))
		return panic("CompactFlash Cylinder/Head/Sector mode not supported\n");
	if (cfcard.sec_cnt != 1)
		return panic("Unsupported read of multiple CompactFlash sectors at once\n");

	secnum = cfcard.sec_num;			/* LBA 7-0 */
	secnum += cfcard.cyl_low << 8;		/* LBA 15-8 */
	secnum += cfcard.cyl_high << 16;	/* LBA 23-16 */
	secnum += (cfcard.cdh & 0x0F) << 8;	/* LBA 27-24 */

	if (!card_file)
		return panic("BUG: ATA read command accepted without a card\n");

	/* TODO: report system errors through ATA registers and keep going? */
	if (fseek(card_file, secnum << CF_SECTOR_SZ_SHIFT, SEEK_SET))
		return panic("Failed fseek() on card file (%s)\n", strerror(errno));

	ret = fread(cfcard.secbuf, 1, sizeof(cfcard.secbuf), card_file);
	if (ret != sizeof(cfcard.secbuf)) {
		if (ferror(card_file))
			return panic("Failed to read from card file\n");
		else
			return panic("Out-of-bounds CompactFlash read (sector: %u)\n", secnum);
	}
	cfcard.secbuf_off = 0;
	cfcard.status |= CF_STATUS_DRQ;
}

static void cfcard_write_begin(void)
{
	if (!(cfcard.cdh & CF_CDH_CHS_OR_LBA))
		return panic("CompactFlash Cylinder/Head/Sector mode not supported\n");
	if (cfcard.sec_cnt != 1)
		return panic("Unsupported read of multiple CompactFlash sectors at once\n");

	cfcard.secbuf_off = 0;
	cfcard.status |= CF_STATUS_DRQ;
}

/* Structure returned by the Identify Drive command. Must be kept packed. */
struct identify_drive_info {
	uint16_t gencon;	/* General configuration */
	uint16_t dcylnum;	/* Default number of cylinders */
	uint16_t rsvd_04;	/* Reserved */
	uint16_t dheadnum;	/* Default number of heads */
	uint16_t unformtr;	/* Number of unformatted bytes per track */
	uint16_t unformsec;	/* Number of unformatted bytes per sector */
	uint16_t dsptr;		/* Default number of sectors per track */
	uint16_t spcard_ms; /* Number of sectors per card (MSW) */
	uint16_t spcard_ls; /* Number of sectors per card (LSW) */
	uint16_t rsvd_12;	/* Reserved */
	char serial[20];	/* Serial number in ASCII (Right Justified) */
	uint16_t buftype;	/* Buffer type (dual ported) */
	uint16_t bufsize;	/* Buffer size in 512 byte increments */
	uint16_t eccsize;	/* ECC bytes passed on Read/Write Long Commands */
	char firmrev[8];	/* Firmware revision in ASCII (Rev M.ms) */
	char model[40];		/* Model number in ASCII (Left Justified) */
	uint16_t mulseccnt;	/* Read/Write Multiple Sector Count */
	uint16_t doublew;	/* Double Word Support */
	uint16_t caps;		/* Capabilities */
	uint16_t rsvd_64;	/* Reserved */
	uint16_t piomode;	/* PIO Data Transfer Cycle Timing Mode */
	uint16_t dmamode;	/* Single Word DMA Data Transfer Cycle Timing Mode */
	uint16_t tranvalid;	/* Translation Parameters Valid */
	uint16_t cylnum;	/* Current numbers of cylinders */
	uint16_t headnum;	/* Current numbers of heads */
	uint16_t sptr;		/* Current sectors per track */
	uint16_t capac_ls;	/* Current capacity in sectors (LSW) */
	uint16_t capac_ms;	/* Current capacity in sectors (MSW) */
	uint16_t mulsec;	/* Multiple Sector Setting */
	uint16_t secnum_ls;	/* Total Sectors Addressable in LBA Mode (LSW) */
	uint16_t secnum_ms;	/* Total Sectors Addressable in LBA Mode (MSW) */
	uint16_t rsvd_7c[2];/* Reserved */
	uint16_t advpio;	/* Advanced PIO Transfer Modes Supported */
	uint16_t rsvd_82[2];/* Reserved */
	uint16_t piotm_nof;	/* Minimum PIO Transfer Cycle Time Without Flow Control */
	uint16_t piotm_f;	/* Minimum PIO Transfer Cycle Time With Flow Control */
	/*
	 * The rest is just reserved, though some words in the middle are documented
	 * as "Reserved vendor unique bytes".
	 */
	uint16_t rsvd_8a[187];
};

static void cfcard_identify_drive(void)
{
	struct identify_drive_info *idinfo = NULL;
	uint32_t sector_count;

	if (sizeof(*idinfo) != sizeof(cfcard.secbuf))
		return panic("BUG: wrong size of Identify Drive struct\n");
	idinfo = (struct identify_drive_info *)cfcard.secbuf;
	cfcard.secbuf_off = 0;

	/*
	 * Like the CIS values, the returned Identify Drive info was taken from a
	 * SanDisk CompactFlash manual. Fields that seem to have fixed values are
	 * easy to handle, the rest will be implemented whenever I start to need
	 * them (TODO). Maybe check this on my physical card?
	 */
	memset(idinfo, 0, sizeof(*idinfo));

	/*
	 * "This field informs the host that this is a non-magnetic, hard sectored,
	 * removable storage device with a transfer rate greater than 10 mb/sec and
	 * is not MFM encoded."
	 */
	idinfo->gencon = 0x848A;
	/*
	 * From the manual: "dual ported multi-sector buffer capable of simultaneous
	 * data transfers to or from the host and the CompactFlash Memory Card".
	 */
	idinfo->buftype = 0x0002;
	/*
	 * "Bit 0 of this field is set, indicating that words 54 to 58 are valid and
	 * reflect the current number of cylinders, heads and sectors. Bit 1 is also
	 * set, indicating values in words 64 through 70 are valid."
	 */
	idinfo->tranvalid = 0x0003;
	/*
	 * The sector count needs to be reported correctly right away because the
	 * self-tests will pick a sector at random to work with.
	 */
	sector_count = card_size >> CF_SECTOR_SZ_SHIFT;
	idinfo->secnum_ls = sector_count & 0x0000FFFF;
	idinfo->secnum_ms = sector_count >> 16;

	cfcard.status |= CF_STATUS_DRQ;
}

static void cfcard_execute_command(uint8_t code)
{
	if (!card_file)
		return panic("ATA command without a card present\n");

	switch (code) {
	case ATA_READ_SECTOR:
		return cfcard_read_sector();
	case ATA_WRITE_SECTOR:
		return cfcard_write_begin();
	case ATA_IDENTIFY_DRIVE:
		return cfcard_identify_drive();
	default:
		return panic("Unsupported ATA command 0x%.2x\n", code);
	}
}

static void cfcard_ata_write_byte_reg(uint32_t addr, uint8_t val)
{
	cfcard_ata_check_address_mode(addr);

	switch (addr) {
	case CF_MEMMODE_DEVCON_OFF:
	case CF_IOMODE_DEVCON_OFF:
		if (val & CF_DEVCON_SWRST)
			return panic("Soft reset for CompactFlash not supported\n");
		if (!(val & CF_DEVCON_IEN))
			return panic("Interrupts for CompactFlash not supported\n");
		/* The remaining bits are documented as "ignored" or "do not care" */
		cfcard.dev_con = val;
		return;
	case CF_MEMMODE_SECCNT_OFF:
	case CF_IOMODE_SECCNT_OFF:
		cfcard.sec_cnt = val;
		return;
	case CF_MEMMODE_SECNUM_OFF:
	case CF_IOMODE_SECNUM_OFF:
		cfcard.sec_num = val;
		return;
	case CF_MEMMODE_CYLLOW_OFF:
	case CF_IOMODE_CYLLOW_OFF:
		cfcard.cyl_low = val;
		return;
	case CF_MEMMODE_CYLHIGH_OFF:
	case CF_IOMODE_CYLHIGH_OFF:
		cfcard.cyl_high = val;
		return;
	case CF_MEMMODE_CDH_OFF:
	case CF_IOMODE_CDH_OFF:
		val |= CF_CDH_ALWAYS_ONE;
		if (val & CF_CDH_DRV)
			return panic("CompactFlash drive 1 not supported\n");
		if (!(val & CF_CDH_CHS_OR_LBA))
			return panic("CompactFlash Cylinder/Head/Sector mode not supported\n");
		cfcard.cdh = val;
		return;
	case CF_MEMMODE_STATCOMM_OFF:
	case CF_IOMODE_STATCOMM_OFF:
		return cfcard_execute_command(val);
	default:
		return panic("Attempted write of 0x%.2x to unsupported CompactFlash register at 0x%.8x\n", val, addr);
	}
}

static bool is_cfcard_config_byte_address(uint32_t addr)
{
	switch (addr) {
	case CF_CONFIG_OPTION_OFF:
	case CF_CONFIG_STATUS_OFF:
	case CF_PIN_REPLACEMENT_OFF:
	case CF_SOCKET_COPY_OFF:
		return true;
	default:
		return false;
	}
}

static void cfcard_config_write_byte_reg(uint32_t addr, uint8_t val)
{
	uint8_t conf;

	switch (addr) {
	case CF_CONFIG_OPTION_OFF:
		if (val & CF_CONFOPT_SRESET)
			return panic("Soft reset for CompactFlash not supported\n");
		if (val & CF_CONFOPT_LEVLREQ)
			return panic("Level mode interrupts for CompactFlash not supported\n");
		conf = val & CF_CONFOPT_CONF_BITS;
		if (conf != CF_CONFOPT_CONF_MMAP && conf != CF_CONFOPT_CONF_IOMAP_SECN)
			return panic("Unsupported CompactFlash configuration index %.2x\n", conf);
		cfcard.conf_opt = val;
		return;
	default:
		return panic("Attempted write of 0x%.2x to unsupported CompactFlash register at 0x%.8x\n", val, addr);
	}
}

static void cfcard_reset(void)
{
	/* TODO: not sure if this makes sense for all fields */
	memset(&cfcard, 0, sizeof(cfcard));
	cfcard.cdh |= CF_CDH_ALWAYS_ONE;

	/* This wouldn't be instant on real hardware, of course */
	cfcard.status |= CF_STATUS_RDY;
}

static uint16_t motherboard_read_word_reg(uint32_t addr)
{
	switch (addr) {
	case MBOARD_STATUS_OFF:
		return motherboard.status;
	case MBOARD_BLINKCNT_OFF:
		return motherboard.blinkcnt;
	case 0x12000000:
	case 0x12000014:
	case 0x12000044:
	case 0x12000098:
		/* No idea about these but keep going for now (TODO) */
		notice("Reading from unknown motherboard register 0x%.8x (PC: 0x%.8x)\n", addr, cpu.PC);
		return 0;
	default:
		panic("Attempted read of unsupported motherboard register at 0x%.8x\n", addr);
		return 0;
	}
}

static void motherboard_write_word_reg(uint32_t addr, uint16_t val)
{
	switch (addr) {
	case MBOARD_COMMANDS_OFF:
		switch (val) {
		case MBOARD_START_BLINKING:
			motherboard.status &= ~MBOARD_NOT_BLINKING;
			break;
		case MBOARD_STOP_BLINKING:
			motherboard.status |= MBOARD_NOT_BLINKING;
			break;
		case 0x8000:
		case 0x2000:
		case 0x0800:
		case 0x0200:
		case 0x0080:
		case 0x0008:
		case 0x0002:
			return notice("Ignoring unknown motherboard command 0x%.4x (PC: 0x%.8x)\n", val, cpu.PC);
		default:
			return panic("Unsupported motherboard command 0x%.4x\n", val);
		}
		return;
	case MBOARD_BLINKCNT_OFF:
		motherboard.blinkcnt = val;
		return;
	case 0x1200006c:
	case 0x12000034:
	case 0x12000024:
	case 0x12000068:
	case 0x12000014:
	case 0x12000044:
	case 0x12000098:
		/* No idea about these but keep going for now (TODO) */
	case 0x12000000:
	case 0x12000030:
		/*
		 * The CompactFlash self-tests at <0x8003DA78> write 1<<8 to 0xb2000030
		 * before doing anything, and write 1<<8 to 0xb2000000 before returning.
		 * Maybe this enables/disables CompactFlash? No idea (TODO).
		 */
		notice("Writing 0x%.4x to unknown motherboard register 0x%.8x (PC: 0x%.8x)\n", val, addr, cpu.PC);
		return;
	default:
		panic("Attempted write of 0x%.4x to unsupported motherboard register at 0x%.8x\n", val, addr);
		return;
	}
}

enum eeprom_state {
	EEPROM_STOPPED,
	EEPROM_WAITING_FOR_COMMAND,
	EEPROM_UPDATING_POINTER,
	EEPROM_WRITING_TO_POINTER,
	EEPROM_READING_FROM_POINTER,
	EEPROM_DONE,
};

/*
 * So far I've only encountered pages 0 and 3 of EEPROM. I don't know how many
 * exist (TODO)...
 */
#define EEPROM_SIZE					0xC0
#define EEPROM_PAGE_SIZE			0x40
#define EEPROM_PAGE_SHIFT			6
#define EEPROM_FIRMWARE_CONFIG_OFF	(0 * EEPROM_PAGE_SIZE)
#define EEPROM_OS_CONFIG_OFF		(2 * EEPROM_PAGE_SIZE)
#define EEPROM_PAGE(addr)			(addr >> EEPROM_PAGE_SHIFT)
#define EEPROM_MON_LEN_LIMIT		68
#define EEPROM_MON_BUF_SIZE			80
struct eeprom {
	uint8_t ptr;				/* Internal address pointer */

	/* Actual memory */
	uint8_t mem[EEPROM_SIZE];
	uint8_t	fc;
	uint8_t fd;

	enum eeprom_state state;

	/* To monitor I/O to the EEPROM */
	char mon_buf[EEPROM_MON_BUF_SIZE];
	int mon_len;
} eeprom = {0};

/* TODO: other commands? What about reserved i2c addresses? */
#define EEPROM_COMMAND_WRITE	0xA0
#define EEPROM_COMMAND_READ		0xA1

static void eeprom_monitor_dump_all(void)
{
	if (eeprom.mon_len == 0)
		return;
	puts(eeprom.mon_buf);
	eeprom.mon_len = 0;
}

/* The separator must always be after "[I2C]" */
static void eeprom_monitor_dump_until(char *end)
{
	char i2c_str[] = "[I2C]";
	char *tail = NULL;
	int tail_len;

	/* Print until the separator */
	if (!end || *end == '\0')
		return eeprom_monitor_dump_all();
	*end = '\0';
	puts(eeprom.mon_buf);

	/* Now move the leftovers to the front of the buffer */
	tail = end + 1;
	tail_len = (eeprom.mon_buf + eeprom.mon_len) - tail;
	memmove(eeprom.mon_buf + sizeof(i2c_str), tail, tail_len + 1);
	eeprom.mon_len = tail_len + sizeof(i2c_str);
}

/* The monitor output is more readable if we try to align it to stops */
static void eeprom_monitor_dump_aligned(void)
{
	char stop[] = "|||";
	char *stop2_p = NULL, *stop3_p = NULL;
	int i;

	if (eeprom.mon_len == 0)
		return;

	stop2_p = eeprom.mon_buf;
	for (i = 0; i < 2; ++i) {
		stop2_p = strstr(stop2_p, stop);
		if (!stop2_p)
			return eeprom_monitor_dump_all();
		stop2_p += sizeof(stop) - 1;
	}

	stop3_p = strstr(stop2_p, stop);
	if (!stop3_p)
		return eeprom_monitor_dump_until(stop2_p);
	stop3_p += sizeof(stop) - 1;
	return eeprom_monitor_dump_until(stop3_p);
}

/* Not for arbitrary strings: the length is expected to be 3 */
static void eeprom_monitor_save_str(const char *str)
{
	char *buf = NULL;
	int len, left, ret;

	dump_monitors_except(MONITOR_EEPROM);

	len = eeprom.mon_len;
	buf = eeprom.mon_buf + len;
	left = EEPROM_MON_BUF_SIZE - len;

	if (len == 0) {
		ret = snprintf(buf, left, "[I2C]");
		len += ret;
		buf += ret;
		left -= ret;
	}
	len += snprintf(buf, left, " %s", str);
	eeprom.mon_len = len;
	if (len >= EEPROM_MON_LEN_LIMIT)
		eeprom_monitor_dump_aligned();
}

/* TODO: add a "mon eeprom" command to turn monitor on/off */
static void eeprom_monitor_save_frame(uint8_t frame, bool to_eeprom)
{
	char buf[4];

	snprintf(buf, sizeof(buf), "%c%.2x", to_eeprom ? '>' : '<', frame);
	eeprom_monitor_save_str(buf);
}

static void eeprom_monitor_save_condition(bool is_start)
{
	/* Just some made-up notation */
	eeprom_monitor_save_str(is_start ? ">>>" : "|||");
}

static bool eeprom_is_valid_address(uint8_t addr)
{
	if (EEPROM_PAGE(addr) == 0)
		return true;
	if (EEPROM_PAGE(addr) == 2)
		return true;
	if (addr == 0xfc || addr == 0xfd)
		return true;
	return false;
}

static void eeprom_start(void)
{
	eeprom_monitor_save_condition(true /* is_start */);

	/*
	 * Restarting i2c after a write command means that the write was just a
	 * pointer update, and now we expect some other command (a read?) to that
	 * new pointer.
	 */
	switch (eeprom.state) {
	case EEPROM_WRITING_TO_POINTER:
		eeprom.state = EEPROM_WAITING_FOR_COMMAND;
		return;
	case EEPROM_STOPPED:
		eeprom.state = EEPROM_WAITING_FOR_COMMAND;
		return;
	default:
		panic("EEPROM restarted at wrong time\n");
	}
}

static void eeprom_stop(void)
{
	eeprom.state = EEPROM_STOPPED;
	eeprom_monitor_save_condition(false /* is_start */);
}

static void eeprom_write_to_pointer(uint8_t val)
{
	uint8_t ptr;

	ptr = eeprom.ptr;
	if (!eeprom_is_valid_address(ptr)) {
		panic("BUG: Out of bounds EEPROM pointer\n");
		return;
	}
	switch (ptr) {
	case 0xfc:
		eeprom.fc = val;
		break;
	case 0xfd:
		eeprom.fd = val;
		break;
	default:
		eeprom.mem[ptr] = val;
	}
	/* Only one write at a time, it seems */
	eeprom.state = EEPROM_DONE;
}

/* Here the EEPROM processes the frame delivered through the i2c bus */
static void eeprom_deliver_frame(uint8_t frame)
{
	eeprom_monitor_save_frame(frame, true /* to_eeprom */);

	switch (eeprom.state) {
	case EEPROM_STOPPED:
		panic("BUG: EEPROM received frame while stopped\n");
		return;
	case EEPROM_UPDATING_POINTER:
		if (!eeprom_is_valid_address(frame)) {
			panic("Setting EEPROM pointer out of bounds (ptr: 0x%.2x)\n", frame);
			return;
		}
		eeprom.ptr = frame;
		eeprom.state = EEPROM_WRITING_TO_POINTER;
		return;
	case EEPROM_WRITING_TO_POINTER:
		eeprom_write_to_pointer(frame);
		return;
	case EEPROM_WAITING_FOR_COMMAND:
		if (frame == EEPROM_COMMAND_WRITE) {
			eeprom.state = EEPROM_UPDATING_POINTER;
		} else if (frame == EEPROM_COMMAND_READ) {
			eeprom.state = EEPROM_READING_FROM_POINTER;
		} else {
			panic("Unknown EEPROM command frame 0x%.2x\n", frame);
			return;
		}
		return;
	case EEPROM_READING_FROM_POINTER:
		panic("EEPROM received frame while reading\n");
		return;
	case EEPROM_DONE:
		panic("EEPROM received frame while waiting for stop\n");
		return;
	}
}

static uint8_t eeprom_read_from_pointer(void)
{
	uint8_t ptr, val;

	ptr = eeprom.ptr;
	if (!eeprom_is_valid_address(ptr)) {
		panic("BUG: Out of bounds EEPROM pointer\n");
		return 0;
	}
	switch (ptr) {
	case 0xfc:
		val = eeprom.fc;
		break;
	case 0xfd:
		val = eeprom.fd;
		break;
	default:
		val = eeprom.mem[ptr];
	}

	/* Only one write at a time, it seems */
	eeprom.state = EEPROM_DONE;
	return val;
}

static uint8_t eeprom_receive_frame(void)
{
	uint8_t frame;

	if (eeprom.state != EEPROM_READING_FROM_POINTER) {
		panic("Invalid EEPROM read attempt\n");
		return 0;
	}

	frame = eeprom_read_from_pointer();
	eeprom_monitor_save_frame(frame, false /* to_eeprom */);
	return frame;
}

/*
 * The firmware is 16 MiB at address 0x80000000, which is actually in the P1
 * area mapped to physical address space so the top bit is ignored.
 */
#define FIRMWARE_SIZE	(16 * 1024 * 1024)
uint8_t firmware[FIRMWARE_SIZE] = {0};
#define FIRMWARE_OFF	0x00000000
#define BOOTLOADER_OFF	0xA0000000
#define FIRMWARE_MASK	(FIRMWARE_SIZE - 1)

/*
 * The registers for the Bus State Controller are accessed at address range
 * 0xFFFFFF50-0xFFFFFF7F, except for the synchronous DRAM mode register, which
 * is written via address bus at 0xFFFFD000-0xFFFFEFFF.
 */
#define BSC_REGS_SIZE	48
#define BSC_REGS_OFF	0xFFFFFF50
#define BSC_SDMR_SIZE	0x2000
#define BSC_SDMR_OFF	0xFFFFD000

#define BSC_BCR1_OFF	0xFFFFFF60
#define BSC_BCR2_OFF	0xFFFFFF62
#define BSC_WCR1_OFF	0xFFFFFF64
#define BSC_WCR2_OFF	0xFFFFFF66
#define BSC_MCR_OFF		0xFFFFFF68
#define BSC_DCR_OFF		0xFFFFFF6A
#define BSC_PCR_OFF		0xFFFFFF6C
#define BSC_RTCSR_OFF	0xFFFFFF6E
#define BSC_RTCNT_OFF	0xFFFFFF70
#define BSC_RTCOR_OFF	0xFFFFFF72
#define BSC_RFCR_OFF	0xFFFFFF74
#define BSC_BCR3_OFF	0xFFFFFF7E
#define BSC_MCSCR0_OFF	0xFFFFFF50
#define BSC_MCSCR1_OFF	0xFFFFFF52
#define BSC_MCSCR2_OFF	0xFFFFFF54
#define BSC_MCSCR3_OFF	0xFFFFFF56
#define BSC_MCSCR4_OFF	0xFFFFFF58
#define BSC_MCSCR5_OFF	0xFFFFFF5A
#define BSC_MCSCR6_OFF	0xFFFFFF5C
#define BSC_MCSCR7_OFF	0xFFFFFF5E

struct bsc {
	uint16_t BCR1;		/* Bus control register 1 */
	uint16_t BCR2;		/* Bus control register 2 */
	uint16_t WCR1;		/* Wait state control register 1 */
	uint16_t WCR2;		/* Wait state control register 2 */
	uint16_t MCR;		/* Individual memory control register */
	uint16_t DCR;		/* DRAM control register */
	uint16_t PCR;		/* PCMCIA control register */
	uint16_t RTCSR;		/* Refresh timer control/status register */
	uint16_t RTCNT;		/* Refresh timer counter */
	uint16_t RTCOR;		/* Refresh time constant register */
	uint16_t RFCR;		/* Refresh count register */
	uint16_t BCR3;		/* Bus control register 3 */
	uint16_t MCSCR[8];	/* MCSx control registers */

	/* TODO: SDMR */

	int ckio_tics;		/* Count of CKIO tics since last RTCNT update */
} bsc = {0};

/* Flags of the RTCSR register */
#define RTCSR_CMF	(1U << 7)	/* Compare match flag */
#define RTCSR_CMIE	(1U << 6)	/* Compare match interrupt enable */
#define RTCSR_CKS	(7U << 3)	/* Clock select bits */
#define RTCSR_OVF	(1U << 2)	/* Refresh count overflow flag */
#define RTCSR_OVIE	(1U << 1)	/* Refresh count overflow interrupt enable */
#define RTCSR_LMTS	(1U << 0)	/* Refresh count overflow limit select */
#define RTCSR_BIT_MASK	(RTCSR_CMF | RTCSR_CMIE | RTCSR_CKS | RTCSR_OVF | RTCSR_OVIE | RTCSR_LMTS)
/* These bits can be reset to zero on a write, but not set to 1 */
#define RTCSR_UNSETTABLE_MASK	(RTCSR_CMF | RTCSR_OVF)

/* Flags of the MCSx control registers */
#define MCSCR_CS20	(1U << 6)	/* CS2/CS0 Select */
#define MCSCR_CAP1	(1U << 5)	/* Connected Memory Size Specification */
#define MCSCR_CAP0	(1U << 4)
#define MCSCR_A25	(1U << 3)	/* Start Address Specification */
#define MCSCR_A24	(1U << 2)
#define MCSCR_A23	(1U << 1)
#define MCSCR_A22	(1U << 0)

/*
 * The registers for the Pin Function Controller are accessed at address range
 * 0xA4000100-0xA4000117, which is actually in the P2 area so the top 3 bits are
 * ignored.
 */
#define PFC_REGS_SIZE	24
uint8_t pfc_regs[PFC_REGS_SIZE] = {0};
#define PFC_REGS_OFF	0x04000100

#define PFC_PACR_OFF	0x04000100
#define PFC_PBCR_OFF	0x04000102
#define PFC_PCCR_OFF	0x04000104
#define PFC_PDCR_OFF	0x04000106
#define PFC_PECR_OFF	0x04000108
#define PFC_PFCR_OFF	0x0400010A
#define PFC_PGCR_OFF	0x0400010C
#define PFC_PHCR_OFF	0x0400010E
#define PFC_PJCR_OFF	0x04000110
#define PFC_PKCR_OFF	0x04000112
#define PFC_PLCR_OFF	0x04000114
#define PFC_SCPCR_OFF	0x04000116

/* Currently supported pins (TODO) */
/* TODO: name all the supported pin configurations */
#define PFC_PC0MD0		0x0001
#define PFC_PC0MD1		0x0002
#define PFC_PC0_MASK	(PFC_PC0MD0 | PFC_PC0MD1)
#define PFC_PC1MD0		0x0004
#define PFC_PC1MD1		0x0008
#define PFC_PC1_MASK	(PFC_PC1MD0 | PFC_PC1MD1)
#define PFC_PC4MD0		0x0100
#define PFC_PC4MD1		0x0200
#define PFC_PC4_MASK	(PFC_PC4MD0 | PFC_PC4MD1)
#define PFC_PC6MD0		0x1000
#define PFC_PC6MD1		0x2000
#define PFC_PC6_MASK	(PFC_PC6MD0 | PFC_PC6MD1)
#define PFC_PD1MD0		0x0004
#define PFC_PD1MD1		0x0008
#define PFC_PD1_OUT		(PFC_PD1MD0)
#define PFC_PD1_IN		(PFC_PD1MD0 | PFC_PD1MD1)
#define PFC_PD1_MASK	(PFC_PD1MD0 | PFC_PD1MD1)
#define PFC_PD3MD0		0x0040
#define PFC_PD3MD1		0x0080
#define PFC_PD3_MASK	(PFC_PD3MD0 | PFC_PD3MD1)
#define PFC_PD5MD0		0x0400
#define PFC_PD5MD1		0x0800
#define PFC_PD5_MASK	(PFC_PD5MD0 | PFC_PD5MD1)
#define PFC_PD6MD0		0x1000
#define PFC_PD6MD1		0x2000
#define PFC_PD6_MASK	(PFC_PD6MD0 | PFC_PD6MD1)
#define PFC_PD7MD0		0x4000
#define PFC_PD7MD1		0x8000
#define PFC_PD7_MASK	(PFC_PD7MD0 | PFC_PD7MD1)
#define PFC_PE7MD0		0x4000
#define PFC_PE7MD1		0x8000
#define PFC_PE7_MASK	(PFC_PE7MD0 | PFC_PE7MD1)
#define PFC_PE5MD0		0x0400
#define PFC_PE5MD1		0x0800
#define PFC_PE5_MASK	(PFC_PE5MD0 | PFC_PE5MD1)
#define PFC_PE4MD0		0x0100
#define PFC_PE4MD1		0x0200
#define PFC_PE4_MASK	(PFC_PE4MD0 | PFC_PE4MD1)
#define PFC_PE3MD0		0x0040
#define PFC_PE3MD1		0x0080
#define PFC_PE3_MASK	(PFC_PE3MD0 | PFC_PE3MD1)
#define PFC_PE2MD0		0x0010
#define PFC_PE2MD1		0x0020
#define PFC_PE2_MASK	(PFC_PE2MD0 | PFC_PE2MD1)
#define PFC_PE0MD0		0x0001
#define PFC_PE0MD1		0x0002
#define PFC_PE0_MASK	(PFC_PE0MD0 | PFC_PE0MD1)
#define PFC_PE1MD0		0x0004
#define PFC_PE1MD1		0x0008
#define PFC_PE1_MASK	(PFC_PE1MD0 | PFC_PE1MD1)
#define PFC_PE6MD0		(1U << (6 * 2 + 0))
#define PFC_PE6MD1		(1U << (6 * 2 + 1))
#define PFC_PE6_MASK	(PFC_PE6MD0 | PFC_PE6MD1)
#define PFC_PF3MD0		0x0040
#define PFC_PF3MD1		0x0080
#define PFC_PF3_MASK	(PFC_PF3MD0 | PFC_PF3MD1)
#define PFC_PF4MD0		0x0100
#define PFC_PF4MD1		0x0200
#define PFC_PF4_MASK	(PFC_PF4MD0 | PFC_PF4MD1)
#define PFC_PF7MD0		0x4000
#define PFC_PF7MD1		0x8000
#define PFC_PF7_MASK	(PFC_PF7MD0 | PFC_PF7MD1)
#define PFC_PG0MD0		0x0001
#define PFC_PG0MD1		0x0002
#define PFC_PG0_MASK	(PFC_PG0MD0 | PFC_PG0MD1)
#define PFC_PG1MD0		(1U << (1 * 2 + 0))
#define PFC_PG1MD1		(1U << (1 * 2 + 1))
#define PFC_PG1_MASK	(PFC_PG1MD0 | PFC_PG1MD1)
#define PFC_PG2MD0		(1U << (2 * 2 + 0))
#define PFC_PG2MD1		(1U << (2 * 2 + 1))
#define PFC_PG2_MASK	(PFC_PG2MD0 | PFC_PG2MD1)
#define PFC_PG4MD0		0x0100
#define PFC_PG4MD1		0x0200
#define PFC_PG4_MASK	(PFC_PG4MD0 | PFC_PG4MD1)
#define PFC_PG5MD0		0x0400
#define PFC_PG5MD1		0x0800
#define PFC_PG5_MASK	(PFC_PG5MD0 | PFC_PG5MD1)
#define PFC_PG7MD0		0x4000
#define PFC_PG7MD1		0x8000
#define PFC_PG7_MASK	(PFC_PG7MD0 | PFC_PG7MD1)
#define PFC_PH0MD0		0x0001
#define PFC_PH0MD1		0x0002
#define PFC_PH0_MASK	(PFC_PH0MD0 | PFC_PH0MD1)
#define PFC_PH1MD0		0x0004
#define PFC_PH1MD1		0x0008
#define PFC_PH1_MASK	(PFC_PH1MD0 | PFC_PH1MD1)
#define PFC_PH2MD0		0x0010
#define PFC_PH2MD1		0x0020
#define PFC_PH2_MASK	(PFC_PH2MD0 | PFC_PH2MD1)
#define PFC_PH3MD0		0x0040
#define PFC_PH3MD1		0x0080
#define PFC_PH3_MASK	(PFC_PH3MD0 | PFC_PH3MD1)
#define PFC_PH4MD0		0x0100
#define PFC_PH4MD1		0x0200
#define PFC_PH4_MASK	(PFC_PH4MD0 | PFC_PH4MD1)
#define PFC_PH5MD0		0x0400
#define PFC_PH5MD1		0x0800
#define PFC_PH5_MASK	(PFC_PH5MD0 | PFC_PH5MD1)
#define PFC_PH6MD0		0x1000
#define PFC_PH6MD1		0x2000
#define PFC_PH6_MASK	(PFC_PH6MD0 | PFC_PH6MD1)
#define PFC_PH7MD0		0x4000
#define PFC_PH7MD1		0x8000
#define PFC_PH7_MASK	(PFC_PH7MD0 | PFC_PH7MD1)
#define PFC_PJ1MD0		0x0004
#define PFC_PJ1MD1		0x0008
#define PFC_PJ1_MASK	(PFC_PJ1MD0 | PFC_PJ1MD1)
#define PFC_PJ3MD0		0x0040
#define PFC_PJ3MD1		0x0080
#define PFC_PJ3_MASK	(PFC_PJ3MD0 | PFC_PJ3MD1)
#define PFC_PJ4MD0		0x0100
#define PFC_PJ4MD1		0x0200
#define PFC_PJ4_MASK	(PFC_PJ4MD0 | PFC_PJ4MD1)
#define PFC_PJ5MD0		0x0400
#define PFC_PJ5MD1		0x0800
#define PFC_PJ5_MASK	(PFC_PJ5MD0 | PFC_PJ5MD1)
#define PFC_PL1MD0		0x0004
#define PFC_PL1MD1		0x0008
#define PFC_PL1_MASK	(PFC_PL1MD0 | PFC_PL1MD1)
#define PFC_PL2MD0		0x0010
#define PFC_PL2MD1		0x0020
#define PFC_PL2_MASK	(PFC_PL2MD0 | PFC_PL2MD1)
#define PFC_PL4MD0		0x0100
#define PFC_PL4MD1		0x0200
#define PFC_PL4_MASK	(PFC_PL4MD0 | PFC_PL4MD1)
#define PFC_PL5MD0		0x0400
#define PFC_PL5MD1		0x0800
#define PFC_PL5_MASK	(PFC_PL5MD0 | PFC_PL5MD1)
#define PFC_SCP0MD0		0x0001
#define PFC_SCP0MD1		0x0002
#define PFC_SCP0_MASK	(PFC_SCP0MD0 | PFC_SCP0MD1)
#define PFC_SCP1MD0		0x0004
#define PFC_SCP1MD1		0x0008
#define PFC_SCP1_MASK	(PFC_SCP1MD0 | PFC_SCP1MD1)
#define PFC_SCP3MD0		0x0040
#define PFC_SCP3MD1		0x0080
#define PFC_SCP3_MASK	(PFC_SCP3MD0 | PFC_SCP3MD1)
#define PFC_SCP6MD0		0x1000
#define PFC_SCP6MD1		0x2000
#define PFC_SCP6_MASK	(PFC_SCP6MD0 | PFC_SCP6MD1)
#define PFC_SCP7MD0		0x4000
#define PFC_SCP7MD1		0x8000
#define PFC_SCP7_MASK	(PFC_SCP7MD0 | PFC_SCP7MD1)

/*
 * The registers for the I/O ports are accessed at address range
 * 0xA4000120-0xA4000138, which is actually in the P2 area so the top 3
 * bits are ignored.
 */
struct ioports {
	/* TODO: the rest of the ports */
	uint8_t	PCDR;
	uint8_t PDDR;
	uint8_t	PEDR;
	uint8_t	PFDR;
	uint8_t	PGDR;
	uint8_t	PHDR;
	uint8_t PJDR;
	uint8_t SCPDR;
} ioports = {0};

#define IOPORTS_PADR_OFF	0x04000120
#define IOPORTS_PBDR_OFF	0x04000122
#define IOPORTS_PCDR_OFF	0x04000124
#define IOPORTS_PDDR_OFF	0x04000126
#define IOPORTS_PEDR_OFF	0x04000128
#define IOPORTS_PFDR_OFF	0x0400012A
#define IOPORTS_PGDR_OFF	0x0400012C
#define IOPORTS_PHDR_OFF	0x0400012E
#define IOPORTS_PJDR_OFF	0x04000130
#define IOPORTS_PKDR_OFF	0x04000132
#define IOPORTS_PLDR_OFF	0x04000134
#define IOPORTS_SCPDR_OFF	0x04000136

static void pfc_write_word_reg(uint32_t addr, uint16_t val)
{
	uint16_t *reg = NULL;

	reg = (uint16_t *)(pfc_regs + (addr - PFC_REGS_OFF));

	switch (addr) {
	case PFC_PCCR_OFF:
		if ((*reg ^ val) & ~(PFC_PC6_MASK | PFC_PC4_MASK | PFC_PC1_MASK | PFC_PC0_MASK)) {
			panic("Attempted PFC operation for unsupported pins (C: 0x%.4x -> 0x%.4x)\n", *reg, val);
			return;
		}
		if ((*reg ^ val) & PFC_PC6_MASK) {
			if ((val & PFC_PC6_MASK) != 0)
				return panic("Unsupported PC6 configuration 0x%.4x\n", val);
			notice("MCS6 pin function enabled\n");
		}
		if ((*reg ^ val) & PFC_PC1_MASK) {
			if ((val & PFC_PC1_MASK) != (PFC_PC1MD0 | PFC_PC1MD1))
				return panic("Unsupported PC1 configuration 0x%.4x\n", val);
			notice("USB interrupt pin function enabled\n");
		}
		if ((*reg ^ val) & PFC_PC0_MASK) {
			if ((val & PFC_PC0_MASK) != (PFC_PC0MD0 | PFC_PC0MD1))
				return panic("Unsupported PC0 configuration 0x%.4x\n", val);
			notice("Unknown pin PC0 set to input with pullup off\n");
		}
		if ((*reg ^ val) & PFC_PC4_MASK) {
			if ((val & PFC_PC4_MASK) != 0)
				return panic("Unsupported PC4 configuration 0x%.4x\n", val);
			notice("MCS4 pin function enabled\n");
		}
		*reg = val;
		return;
	case PFC_PDCR_OFF:
		if ((*reg ^ val) & ~(PFC_PD7_MASK | PFC_PD6_MASK | PFC_PD5_MASK | PFC_PD3_MASK | PFC_PD1_MASK)) {
			panic("Attempted PFC operation for unsupported pins (D: 0x%.4x -> 0x%.4x)\n", *reg, val);
			return;
		}
		if ((*reg ^ val) & PFC_PD7_MASK) {
			if ((val & PFC_PD7_MASK) != PFC_PD7MD0)
				return panic("Unsupported PD7 configuration 0x%.4x\n", val);
			notice("Unknown pin PD7 set to output\n");
		}
		if ((*reg ^ val) & PFC_PD6_MASK) {
			if ((val & PFC_PD6_MASK) != (PFC_PD6MD0 | PFC_PD6MD1))
				return panic("Unsupported PD6 configuration 0x%.4x\n", val);
			notice("Unknown pin PD6 set to input with pullup off\n");
		}
		if ((*reg ^ val) & PFC_PD5_MASK) {
			if ((val & PFC_PD5_MASK) != PFC_PD5MD0)
				return panic("Unsupported PD5 configuration 0x%.4x\n", val);
			notice("Unknown pin PD5 set to output\n");
		}
		if ((*reg ^ val) & PFC_PD3_MASK) {
			if ((val & PFC_PD3_MASK) != 0)
				return panic("Unsupported PD3 configuration 0x%.4x\n", val);
			notice("Pin PD3 set to \"WAKEUP output (WTC)\"\n");
		}
		if ((val & PFC_PD1_MASK) == PFC_PD1_IN) {
			/*
			 * I don't know much about hardware, but I'm guessing there is a
			 * pullup somewhere else, so this is like an output of 1.
			 */
			front4_requested = false;
		} else if ((val & PFC_PD1_MASK) == PFC_PD1_OUT) {
			front4_requested = !(ioports.PDDR & 0x02);
		} else {
			panic("Unsupported PD1 configuration 0x%.4x\n", val);
			return;
		}
		*reg = val;
		return;
	case PFC_PECR_OFF:
		if ((*reg ^ val) & ~(PFC_PE7_MASK | PFC_PE5_MASK | PFC_PE3_MASK | PFC_PE2_MASK | PFC_PE0_MASK | PFC_PE1_MASK)) {
			panic("Attempted PFC operation for unsupported pins (E: 0x%.4x -> 0x%.4x)\n", *reg, val);
			return;
		}
		if ((*reg ^ val) & PFC_PE7_MASK) {
			if ((val & PFC_PE7_MASK) != PFC_PE7MD0)
				return panic("Unsupported PE7 configuration 0x%.4x\n", val);
			notice("Unknown pin PE7 set to output\n");
		}
		if ((*reg ^ val) & PFC_PE5_MASK) {
			if ((val & PFC_PE5_MASK) != 0)
				return panic("Unsupported PE5 configuration 0x%.4x\n", val);
			notice("Pin PE5 set to \"CE2B output (PCMCIA)\"\n");
		}
		if ((*reg ^ val) & PFC_PE3_MASK) {
			if ((val & PFC_PE3_MASK) != PFC_PE3MD0)
				return panic("Unsupported PE3 configuration 0x%.4x\n", val);
			notice("Unknown pin PE3 set to output\n");
		}
		/* TODO: is releasing the pin different from pulling it up for us? */
		if ((val & PFC_PE2_MASK) == PFC_PE2MD0) {
			ioports.PEDR & 0x04 ? i2c_release_sda() : i2c_pull_down_sda();
		} else if ((val & PFC_PE2_MASK) == PFC_PE2MD1) {
			i2c_release_sda();
		} else {
			panic("Unsupported PE2 configuration 0x%.4x\n", val);
			return;
		}
		if ((*reg ^ val) & PFC_PE0_MASK) {
			if ((val & PFC_PE0_MASK) == PFC_PE0MD0)
				write_flag_to_byte(&touchscreen.state, TOUCH_STATE_PE0DT, ioports.PEDR & 0x01);
			else if ((val & PFC_PE0_MASK) != PFC_PE0MD1)
				panic("Unsupported PE0 configuration 0x%.4x\n", val);
		}
		if ((*reg ^ val) & PFC_PE1_MASK) {
			if ((val & PFC_PE1_MASK) == PFC_PE1MD0)
				write_flag_to_byte(&touchscreen.state, TOUCH_STATE_PE1DT, ioports.PEDR & 0x02);
			else if ((val & PFC_PE1_MASK) != PFC_PE1MD1)
				return panic("Unsupported PE1 configuration 0x%.4x\n", val);
		}
		*reg = val;
		return;
	case PFC_PFCR_OFF:
		if ((*reg ^ val) & ~(PFC_PF7_MASK | PFC_PF4_MASK | PFC_PF3_MASK)) {
			panic("Attempted PFC operation for unsupported pins (F: 0x%.4x -> 0x%.4x)\n", *reg, val);
			return;
		}
		if ((*reg ^ val) & PFC_PF7_MASK) {
			if ((val & PFC_PF7_MASK) != (PFC_PF7MD0 | PFC_PF7MD1))
				return panic("Unsupported PF7 configuration 0x%.4x\n", val);
			notice("Unknown pin PF7 set to input with pullup off\n");
		}
		if ((*reg ^ val) & PFC_PF4_MASK) {
			if ((val & PFC_PF4_MASK) != (PFC_PF4MD0 | PFC_PF4MD1))
				return panic("Unsupported PF4 configuration 0x%.4x\n", val);
			notice("Unknown pin PF4 set to input with pullup off\n");
		}
		if ((*reg ^ val) & PFC_PF3_MASK) {
			if ((val & PFC_PF3_MASK) != (PFC_PF3MD0 | PFC_PF3MD1))
				return panic("Unsupported PF3 configuration 0x%.4x\n", val);
			notice("Unknown pin PF3 set to input with pullup off\n");
		}
		*reg = val;
		return;
	case PFC_PGCR_OFF:
		if ((*reg ^ val) & ~(PFC_PG0_MASK | PFC_PG5_MASK | PFC_PG7_MASK)) {
			panic("Attempted PFC operation for unsupported pins (G: 0x%.4x -> 0x%.4x)\n", *reg, val);
			return;
		}
		if ((*reg ^ val) & PFC_PG5_MASK) {
			if ((val & PFC_PG5_MASK) == PFC_PG5MD0)
				notice("Unknown pin PG5 set to reserved\n");
			else if ((val & PFC_PG5_MASK) == PFC_PG5MD1)
				notice("Unknown pin PG5 set to input with pullup on\n");
			else
				return panic("Unsupported PG5 configuration 0x%.4x\n", val);
		}
		if ((*reg ^ val) & PFC_PG7_MASK) {
			if ((val & PFC_PG7_MASK) != (PFC_PG7MD0 | PFC_PG7MD1))
				return panic("Unsupported PG7 configuration 0x%.4x\n", val);
			notice("Unknown pin PG7 set to input with pullup off\n");
		}
		if ((val & PFC_PG0_MASK) != PFC_PG0MD1) {
			panic("Unsupported PG0 configuration 0x%.4x\n", val);
			return;
		}
		*reg = val; /* Pullup MOS on... does that matter here? TODO */
		return;
	case PFC_PHCR_OFF:
		if ((*reg ^ val) & ~(PFC_PH7_MASK | PFC_PH6_MASK | PFC_PH5_MASK | PFC_PH4_MASK | PFC_PH3_MASK | PFC_PH2_MASK | PFC_PH1_MASK | PFC_PH0_MASK)) {
			panic("Attempted PFC operation for unsupported pins (H: 0x%.4x -> 0x%.4x)\n", *reg, val);
			return;
		}
		if ((*reg ^ val) & PFC_PH7_MASK) {
			if ((val & PFC_PH7_MASK) != PFC_PH7MD0)
				return panic("Unsupported PH7 configuration 0x%.4x\n", val);
			notice("Unknown pin PH7 set to output\n");
		}
		if ((*reg ^ val) & PFC_PH6_MASK) {
			if ((val & PFC_PH6_MASK) != (PFC_PH6MD0 | PFC_PH6MD1))
				return panic("Unsupported PH6 configuration 0x%.4x\n", val);
			notice("Unknown pin PH6 set to input with pullup off\n");
		}
		if ((*reg ^ val) & PFC_PH5_MASK) {
			if ((val & PFC_PH5_MASK) != 0)
				return panic("Unsupported PH5 configuration 0x%.4x\n", val);
			notice("Pin PH5 set to \"ADTRG input (ADC)\"\n");
		}
		if ((*reg ^ val) & PFC_PH4_MASK) {
			if ((val & PFC_PH4_MASK) != (PFC_PH4MD0 | PFC_PH4MD1))
				return panic("Unsupported PH4 configuration 0x%.4x\n", val);
		}
		if ((*reg ^ val) & PFC_PH3_MASK) {
			if ((val & PFC_PH3_MASK) != (PFC_PH3MD0 | PFC_PH3MD1))
				return panic("Unsupported PH3 configuration 0x%.4x\n", val);
		}
		if ((*reg ^ val) & PFC_PH2_MASK) {
			if ((val & PFC_PH2_MASK) != (PFC_PH2MD0 | PFC_PH2MD1))
				return panic("Unsupported PH2 configuration 0x%.4x\n", val);
			notice("Unknown pin PH2 set to input with pullup off\n");
		}
		if ((*reg ^ val) & PFC_PH1_MASK) {
			if ((val & PFC_PH1_MASK) != 0)
				return panic("Unsupported PH1 configuration 0x%.4x\n", val);
			notice("Pin PH1 set to \"IRQ1 input (INTC)\"\n");
		}
		if ((*reg ^ val) & PFC_PH0_MASK) {
			if ((val & PFC_PH0_MASK) != (PFC_PH0MD0 | PFC_PH0MD1))
				return panic("Unsupported PH0 configuration 0x%.4x\n", val);
			notice("Unknown pin PH0 set to input with pullup off\n");
		}
		*reg = val;
		return;
	case PFC_PJCR_OFF:
		if ((*reg ^ val) & ~(PFC_PJ1_MASK | PFC_PJ3_MASK | PFC_PJ4_MASK | PFC_PJ5_MASK)) {
			panic("Attempted PFC operation for unsupported pins (J: 0x%.4x -> 0x%.4x)\n", *reg, val);
			return;
		}
		if ((*reg ^ val) & PFC_PJ1_MASK) {
			if ((val & PFC_PJ1_MASK) != PFC_PJ1MD0)
				return panic("Unsupported PJ1 configuration 0x%.4x\n", val);
			notice("Unknown pin PJ1 set to output\n");
		}
		if ((*reg ^ val) & PFC_PJ3_MASK) {
			if ((val & PFC_PJ3_MASK) == PFC_PJ3MD0) {
				ioports.PJDR & 0x08 ? i2c_pull_up_scl() : i2c_pull_down_scl();
			} else {
				panic("Unsupported PJ3 configuration 0x%.4x\n", val);
				return;
			}
		}
		if ((*reg ^ val) & PFC_PJ4_MASK) {
			if ((val & PFC_PJ4_MASK) != PFC_PJ4MD0)
				return panic("Unsupported PJ4 configuration 0x%.4x\n", val);
			notice("Unknown pin PJ4 set to output\n");
		}
		if ((*reg ^ val) & PFC_PJ5_MASK) {
			if ((val & PFC_PJ5_MASK) == PFC_PJ5MD0) {
				if (ioports.PJDR & 0x20)
					cfcard_reset();
			} else {
				return panic("Unsupported PJ5 configuration 0x%.4x\n", val);
			}
		}
		*reg = val;
		return;
	case PFC_PLCR_OFF:
		if ((*reg ^ val) & ~(PFC_PL1_MASK | PFC_PL2_MASK | PFC_PL4_MASK | PFC_PL5_MASK)) {
			panic("Attempted PFC operation for unsupported pins (L: 0x%.4x -> 0x%.4x)\n", *reg, val);
			return;
		}
		/*
		 * PL1 and PL2 are used as analog input pins for the A/D converter, so
		 * I would have expected their configuration to be "Other function".
		 * They get set to "Reserved" instead. I guess that works the same? Odd.
		 */
		if ((*reg ^ val) & PFC_PL1_MASK) {
			if ((val & PFC_PL1_MASK) != PFC_PL1MD0) {
				panic("Unsupported PL1 configuration 0x%.4x\n", val);
				return;
			}
		}
		if ((*reg ^ val) & PFC_PL2_MASK) {
			if ((val & PFC_PL2_MASK) != PFC_PL2MD0) {
				panic("Unsupported PL2 configuration 0x%.4x\n", val);
				return;
			}
		}
		if ((*reg ^ val) & PFC_PL4_MASK) {
			if ((val & PFC_PL4_MASK) != PFC_PL4MD1)
				return panic("Unsupported PL4 configuration 0x%.4x\n", val);
			notice("Unknown pin PL4 set to input\n");
		}
		if ((*reg ^ val) & PFC_PL5_MASK) {
			if ((val & PFC_PL5_MASK) != PFC_PL5MD1)
				return panic("Unsupported PL5 configuration 0x%.4x\n", val);
			notice("Unknown pin PL5 set to input\n");
		}
		*reg = val;
		return;
	case PFC_SCPCR_OFF:
		if ((*reg ^ val) & ~(PFC_SCP0_MASK | PFC_SCP1_MASK | PFC_SCP3_MASK | PFC_SCP6_MASK | PFC_SCP7_MASK)) {
			panic("Attempted PFC operation for unsupported pins (SC: 0x%.4x -> 0x%.4x)\n", *reg, val);
			return;
		}
		if ((*reg ^ val) & PFC_SCP0_MASK) {
			if ((val & PFC_SCP0_MASK) == PFC_SCP0MD0) {
				write_flag_to_byte(&touchscreen.state, TOUCH_STATE_SCP0DT, ioports.SCPDR & 0x01);
			} else {
				panic("Unsupported SCP0 configuration 0x%.4x\n", val);
				return;
			}
		}
		if ((*reg ^ val) & PFC_SCP1_MASK) {
			if ((val & PFC_SCP1_MASK) == PFC_SCP1MD0) {
				write_flag_to_byte(&touchscreen.state, TOUCH_STATE_SCP1DT, ioports.SCPDR & 0x02);
			} else {
				panic("Unsupported SCP1 configuration 0x%.4x\n", val);
				return;
			}
		}
		if ((*reg ^ val) & PFC_SCP3_MASK) {
			if ((val & PFC_SCP3_MASK) != PFC_SCP3MD0)
				return panic("Unsupported SCP3 configuration 0x%.4x\n", val);
			notice("Unknown pin SCP3 set to output\n");
		}
		if ((*reg ^ val) & PFC_SCP6_MASK) {
			if ((val & PFC_SCP6_MASK) != PFC_SCP6MD0)
				return panic("Unsupported SCP6 configuration 0x%.4x\n", val);
			notice("Unknown pin SCP6 set to output\n");
		}
		if ((*reg ^ val) & PFC_SCP7_MASK) {
			if ((val & PFC_SCP7_MASK) != (PFC_SCP7MD0 | PFC_SCP7MD1))
				return panic("Unsupported SCP7 configuration 0x%.4x\n", val);
			notice("Unknown pin SCP7 set to input with pullup off\n");
		}
		*reg = val;
		return;
	default:
		/*
		 * These are all "other function", save for a few that are "reserved".
		 * I don't think any emulation is necessary.
		 */
		if (val == 0)
			return;
		panic("Attempted write of 0x%.4x to unsupported PFC register at 0x%.8x\n", val, addr);
		return;
	}
}

/* TODO: use struct arrays to match reg addresses to functions */
static bool is_ioports_byte_address(uint32_t addr)
{
	switch (addr) {
	case IOPORTS_PADR_OFF:
	case IOPORTS_PBDR_OFF:
	case IOPORTS_PCDR_OFF:
	case IOPORTS_PDDR_OFF:
	case IOPORTS_PEDR_OFF:
	case IOPORTS_PFDR_OFF:
	case IOPORTS_PGDR_OFF:
	case IOPORTS_PHDR_OFF:
	case IOPORTS_PJDR_OFF:
	case IOPORTS_PKDR_OFF:
	case IOPORTS_PLDR_OFF:
	case IOPORTS_SCPDR_OFF:
		return true;
	default:
		return false;
	}
}

/* Prints notices about unknown pin outputs only when they change */
#define NOTICE_PIN(pin, val)										\
	do {															\
		static unsigned int last_val_##pin = 2;						\
																	\
		if (last_val_##pin == (val))								\
			break;													\
		last_val_##pin = (val);										\
		notice("Output of %u through unknown pin %s\n", val, #pin);	\
	} while (false)

static void ioports_write_byte_reg(uint32_t addr, uint8_t val)
{
	uint16_t control;

	switch (addr) {
	case IOPORTS_PDDR_OFF:
		control = *(uint16_t *)(pfc_regs + (PFC_PDCR_OFF - PFC_REGS_OFF));
		if (val & ~0x82) {
			panic("Attempted write to unsupported pin (D:0x%.2x)\n", val);
			return;
		}
		/*
		 * The interrupt handler for the front row of buttons at <0x80036D6C>
		 * writes zero to PD1DT before reading the pins for each button. So I'm
		 * guessing the pins won't be set correctly if you don't do that? TODO
		 */
		if ((control & PFC_PD1_MASK) == PFC_PD1_OUT)
			front4_requested = !(val & 0x02);
		if ((control & PFC_PD7_MASK) == PFC_PD7MD0)
			NOTICE_PIN(PD7, val >> 7);
		ioports.PDDR = val & 0xAF;
		return;
	case IOPORTS_PEDR_OFF:
		control = *(uint16_t *)(pfc_regs + (PFC_PECR_OFF - PFC_REGS_OFF));
		if (val & ~0xdf) {
			panic("Attempted write to unsupported pin (E:0x%.2x)\n", val);
			return;
		}
		if ((val & 0x40) && ((control & PFC_PE6_MASK) == PFC_PE6MD0)) {
			panic("Attempted output through input pin PTE6\n");
			return;
		}
		if ((control & PFC_PE7_MASK) == PFC_PE7MD0)
			NOTICE_PIN(PE7, val >> 7);
		if ((control & PFC_PE4_MASK) == PFC_PE4MD0)
			NOTICE_PIN(PE4, (val & 0x10) >> 4);
		if ((control & PFC_PE3_MASK) == PFC_PE3MD0)
			NOTICE_PIN(PE3, (val & 0x08) >> 3);
		if ((control & PFC_PE2_MASK) == PFC_PE2MD0)
			val & 0x04 ? i2c_release_sda() : i2c_pull_down_sda();
		if ((control & PFC_PE0_MASK) == PFC_PE0MD0)
			write_flag_to_byte(&touchscreen.state, TOUCH_STATE_PE0DT, val & 0x01);
		if ((control & PFC_PE1_MASK) == PFC_PE1MD0)
			write_flag_to_byte(&touchscreen.state, TOUCH_STATE_PE1DT, val & 0x02);
		ioports.PEDR = val;
		return;
	case IOPORTS_PGDR_OFF:
		/*
		 * According to the manual, writes to this register are just ignored.
		 * Oddly, this acutally seems to happen during boot.
		 */
		return;
	case IOPORTS_PHDR_OFF:
		control = *(uint16_t *)(pfc_regs + (PFC_PHCR_OFF - PFC_REGS_OFF));
		if (val & ~0x80) {
			panic("Attempted write to unsupported pin (H:0x%.2x)\n", val);
			return;
		}
		if ((control & PFC_PH7_MASK) == PFC_PH7MD0)
			NOTICE_PIN(PH7, val >> 7);
		ioports.PHDR = val & 0x80;
		return;
	case IOPORTS_PJDR_OFF:
		/* TODO: usually the same for all pins, so reuse code somehow */
		control = *(uint16_t *)(pfc_regs + (PFC_PJCR_OFF - PFC_REGS_OFF));
		if (val & ~0x38) {
			panic("Attempted write to unsupported pin (J:0x%.2x)\n", val);
			return;
		}
		if ((control & PFC_PJ3_MASK) == PFC_PJ3MD0)
			val & 0x08 ? i2c_pull_up_scl() : i2c_pull_down_scl();
		if ((control & PFC_PJ4_MASK) == PFC_PJ4MD0)
			NOTICE_PIN(PJ4, (val & 0x10) >> 4);
		if ((control & PFC_PJ5_MASK) == PFC_PJ5MD0) {
			/* If the line is held the card is already reset for us */
			if (val & 0x20 && !(ioports.PJDR & 0x20))
				cfcard_reset();
		}
		ioports.PJDR = val;
		return;
	case IOPORTS_SCPDR_OFF:
		control = *(uint16_t *)(pfc_regs + (PFC_SCPCR_OFF - PFC_REGS_OFF));
		if (val & ~0x03)
			return panic("Attempted write to unsupported pin (SC:0x%.2x)\n", val);
		if ((control & PFC_SCP0_MASK) == PFC_SCP0MD0)
			write_flag_to_byte(&touchscreen.state, TOUCH_STATE_SCP0DT, val & 0x01);
		else
			return panic("Unsupported configuration for Port SC (0x%.4x)\n", control);
		if ((control & PFC_SCP1_MASK) == PFC_SCP1MD0)
			write_flag_to_byte(&touchscreen.state, TOUCH_STATE_SCP1DT, val & 0x02);
		else
			return panic("Unsupported configuration for Port SC (0x%.4x)\n", control);
		ioports.PJDR = val;
		return;
	default:
		panic("Attempted write to unsupported IO register at 0x%.8x\n", addr);
		return;
	}
}

static uint8_t ioports_read_byte_reg(uint32_t addr)
{
	uint16_t control;
	uint8_t val;

	val = 0;

	switch (addr) {
	case IOPORTS_PCDR_OFF:
		control = *(uint16_t *)(pfc_regs + (PFC_PCCR_OFF - PFC_REGS_OFF));
		/* TODO: this looks backwards? We should be reading from the pins */
		if (control != 0xAAAA) {
			panic("Unsupported configuration for Port C (0x%.4x)\n", control);
			return 0;
		}
		return ioports.PCDR;
	case IOPORTS_PDDR_OFF:
		control = *(uint16_t *)(pfc_regs + (PFC_PDCR_OFF - PFC_REGS_OFF));
		if ((control & PFC_PD1_MASK) == PFC_PD1_IN) {
			/*
			 * This is an output pin to the front row buttons. I don't expect
			 * any input here; and since 0 is the only output ever set by the
			 * firmware, there must be a pullup elsewhere. TODO: confirm this.
			 */
			write_flag_to_byte(&val, 1U << 1, 1);
		} else if ((control & PFC_PD1_MASK) == PFC_PD1_OUT) {
			write_flag_to_byte(&val, 1U << 1, ioports.PDDR & (1U << 1));
		} else {
			panic("Unsupported configuration for Port D (0x%.4x)\n", control);
			return 0;
		}
		/* For now we just read zero for all unsupported pins (TODO) */
		return val;
	case IOPORTS_PEDR_OFF:
		control = *(uint16_t *)(pfc_regs + (PFC_PECR_OFF - PFC_REGS_OFF));
		if (control & PFC_PE2MD1)
			write_flag_to_byte(&val, 1U << 2, i2c.sda);
		else
			write_flag_to_byte(&val, 1U << 2, ioports.PEDR & (1U << 2));
		if (control & PFC_PE6MD1)
			write_flag_to_byte(&val, 1U << 6, !(button_state & BUTTON_QL1_PUSHED));
		else
			write_flag_to_byte(&val, 1U << 6, ioports.PEDR & (1U << 6));
		/*
		 * So far I have no reason to expect any input through pins PTE0 and
		 * PTE1: I think they are just output pins to drive the touchscreen.
		 * But the EEPROM selftests do set them to input for some reason, so
		 * I guess we just read the pullup MOS.
		 */
		if (!(control & PFC_PE0MD1))
			write_flag_to_byte(&val, 1U << 0, ioports.PEDR & (1U << 0));
		else if (!(control & PFC_PE0MD0))
			write_flag_to_byte(&val, 1U << 0, 1);
		else
			panic("Unsupported configuration for Port E (0x%.4x)\n", control);
		if (!(control & PFC_PE1MD1))
			write_flag_to_byte(&val, 1U << 1, ioports.PEDR & (1U << 1));
		else if (!(control & PFC_PE1MD0))
			write_flag_to_byte(&val, 1U << 1, 0);
		else
			panic("Unsupported configuration for Port E (0x%.4x)\n", control);
		/* For now we just read zero for all unsupported pins (TODO) */
		return val;
	case IOPORTS_PFDR_OFF:
		control = *(uint16_t *)(pfc_regs + (PFC_PFCR_OFF - PFC_REGS_OFF));
		if (control != 0xAAAA) {
			panic("Unsupported configuration for Port F (0x%.4x)\n", control);
			return 0;
		}
		/* These input pins are for the PINT8-15 interrupts */
		write_flag_to_byte(&val, 1U << (12 - 8), !(button_state & BUTTON_EXIT_PUSHED));
		/* Keep these unpushed for now... (TODO) */
		write_flag_to_byte(&val, 1U << (11 - 8), !(button_state & BUTTON_RECORD_PUSHED));
		write_flag_to_byte(&val, 1U << (10 - 8), !(button_state & BUTTON_ENTER_PUSHED));
		write_flag_to_byte(&val, 1U << (9 - 8), !(button_state & BUTTON_DOWN_PUSHED));
		write_flag_to_byte(&val, 1U << (8 - 8), !(button_state & BUTTON_UP_PUSHED));
		/* For now we just read zero for all unsupported pins (TODO) */
		return val;
	case IOPORTS_PGDR_OFF:
		control = *(uint16_t *)(pfc_regs + (PFC_PGCR_OFF - PFC_REGS_OFF));
		if (control & PFC_PG2MD1)
			write_flag_to_byte(&val, 1U << 2, !(button_state & BUTTON_QL2_PUSHED));
		if (control & PFC_PG1MD1)
			write_flag_to_byte(&val, 1U << 1, !(button_state & BUTTON_QL3_PUSHED));
		if (control & PFC_PG0MD1)
			write_flag_to_byte(&val, 1U << 0, !(button_state & BUTTON_QL4_PUSHED));
		/* For now we just read zero for all unsupported pins (TODO) */
		return val;
	case IOPORTS_PHDR_OFF:
		control = *(uint16_t *)(pfc_regs + (PFC_PHCR_OFF - PFC_REGS_OFF));
		if ((control & PFC_PH3_MASK) == (PFC_PH3MD0 | PFC_PH3MD1)) {
			/* This reads the value of the IRQ3 pin */
			write_flag_to_byte(&val, 1U << 3, touchscreen.x >= 0);
		} else {
			panic("Unsupported configuration for Port H (0x%.4x)\n", control);
			return 0;
		}
		/* For now we just read zero for all unsupported pins (TODO) */
		return val;
	case IOPORTS_PJDR_OFF:
		control = *(uint16_t *)(pfc_regs + (PFC_PJCR_OFF - PFC_REGS_OFF));
		/* For now we just read zero for all unsupported pins (TODO) */
		if (control & PFC_PJ3MD1)
			return i2c.scl ? 0x08 : 0;
		return ioports.PJDR & 0x08;
	case IOPORTS_SCPDR_OFF:
		control = *(uint16_t *)(pfc_regs + (PFC_SCPCR_OFF - PFC_REGS_OFF));
		if ((control & PFC_SCP0_MASK) == PFC_SCP0MD0) {
			write_flag_to_byte(&val, 1U << 0, ioports.SCPDR & (1U << 0));
		} else {
			panic("Unsupported configuration for Port SC (0x%.4x)\n", control);
			return 0;
		}
		if ((control & PFC_SCP1_MASK) == PFC_SCP1MD0) {
			write_flag_to_byte(&val, 1U << 1, ioports.SCPDR & (1U << 1));
		} else {
			panic("Unsupported configuration for Port SC (0x%.4x)\n", control);
			return 0;
		}
		/* For now we just read zero for all unsupported pins (TODO) */
		return val;
	default:
		panic("Attempted read from unsupported IO register at 0x%.8x\n", addr);
		return 0;
	}
}

/*
 * I have no idea what's going on at 0xB3A*****, but it's clearly some way to
 * print text. I can just do that and move on for now (TODO).
 */
#define XB3A_REGS_OFF	0x13A00000

/* 0xB3A***** registers encountered so far */
#define XB3A_014_OFF	0x13A00014
#define XB3A_018_OFF	0x13A00018
#define XB3A_020_OFF	0x13A00020
#define XB3A_024_OFF	0x13A00024
#define XB3A_028_OFF	0x13A00028
#define XB3A_078_OFF	0x13A00078
#define XB3A_07C_OFF	0x13A0007C
#define XB3A_080_OFF	0x13A00080
#define XB3A_084_OFF	0x13A00084
#define XB3A_0AC_OFF	0x13A000AC
#define XB3A_13C_OFF	0x13A0013C
#define XB3A_18C_OFF	0x13A0018C
#define XB3A_19C_OFF	0x13A0019C
#define XB3A_1A0_OFF	0x13A001A0
#define XB3A_1A4_OFF	0x13A001A4
#define XB3A_1A8_OFF	0x13A001A8
#define XB3A_1B0_OFF	0x13A001B0
#define XB3A_1C8_OFF	0x13A001C8
#define XB3A_1D8_OFF	0x13A001D8
#define XB3A_1E0_OFF	0x13A001E0
#define XB3A_1F0_OFF	0x13A001F0
#define XB3A_1F8_OFF	0x13A001F8

static void console_monitor_dump(struct console_monitor *mon)
{
	if (mon->cm_len == 0)
		return;
	puts(mon->cm_buf);
	mon->cm_len = 0;
}

static void console_monitor_save_bytes(struct console_monitor *mon, const char *bytes, int inlen)
{
	char *buf = NULL;
	int outlen, left, ret;

	/* TODO: be more clever here */
	if (mon == &xB3A_monitor)
		dump_monitors_except(MONITOR_XB3A);
	else
		dump_monitors_except(MONITOR_SERIAL);

	outlen = mon->cm_len;
	buf = mon->cm_buf + outlen;
	left = CONSOLE_MON_BUF_SIZE - outlen;

	if (outlen == 0) {
		ret = snprintf(buf, left, "[%s] ", mon->cm_tag);
		outlen += ret;
		buf += ret;
		left -= ret;
	}

	/* We will always have enough space, but just in case... */
	if (left < inlen + 1) {
		panic("BUG: Overflow while monitoring a console\n");
		return;
	}
	memcpy(buf, bytes, inlen);
	outlen += inlen;
	buf += inlen;
	*buf = '\0';

	mon->cm_len = outlen;
	if (outlen >= CONSOLE_MON_LINE_LIMIT)
		console_monitor_dump(mon);
}

static void console_monitor_save_byte(struct console_monitor *mon, uint8_t byte)
{
	char buf[5];

	if (byte == '\\')
		return console_monitor_save_bytes(mon, "\\", 1);
	if (isprint(byte))
		return console_monitor_save_bytes(mon, (char *)&byte, 1);
	if (byte == '\r')
		return console_monitor_save_bytes(mon, "\\d", 2);
	if (byte == '\n') {
		console_monitor_save_bytes(mon, "\\n", 2);
		return console_monitor_dump(mon);
	}

	snprintf(buf, sizeof(buf), "\\x%.2x", byte);
	return console_monitor_save_bytes(mon, buf, sizeof(buf) - 1);
}

static void xB3A_write_byte_reg(uint32_t addr, uint8_t val)
{
	switch (addr) {
	case XB3A_014_OFF:
	case XB3A_018_OFF:
	case XB3A_020_OFF:
	case XB3A_024_OFF:
	case XB3A_028_OFF:
	case XB3A_078_OFF:
	case XB3A_07C_OFF:
	case XB3A_080_OFF:
	case XB3A_084_OFF:
	case XB3A_0AC_OFF:
	case XB3A_1A0_OFF:
	case XB3A_1A4_OFF:
	case XB3A_1A8_OFF:
	case XB3A_1B0_OFF:
	case XB3A_1C8_OFF:
	case XB3A_1D8_OFF:
	case XB3A_1E0_OFF:
	case XB3A_1F0_OFF:
	case XB3A_1F8_OFF:
		/* Do nothing, for now */
		return;
	case XB3A_18C_OFF:
		return console_monitor_save_byte(&xB3A_monitor, val);
	default:
		panic("Attempted write to unsupported xB3A register at 0x%.8x\n", addr);
		return;
	}
}

static uint8_t xB3A_read_byte_reg(uint32_t addr)
{
	static unsigned int readcnt = 0;

	/* Just return something that matches whatever the firmware expects */
	switch (addr) {
	case XB3A_014_OFF:
		return 0;
	case XB3A_13C_OFF:
		/*
		 * The function at <0x8003557C> (which seems to initialize this
		 * interface), loops around this register until the value becomes 0x00,
		 * and then loops again until it becomes 0x25. No idea what this means,
		 * but for now alternating the two values is enough to keep going.
		 */
		if (++readcnt == 200)
			readcnt = 0;
		return readcnt < 100 ? 0x00 : 0x25;
	case XB3A_19C_OFF:
		return 0x02;
	case XB3A_1A4_OFF:
		return 0x08;
	case XB3A_1A8_OFF:
		return 0x12;
	case XB3A_1A0_OFF:
		return 0x00;
	default:
		panic("Attempted read from unsupported xB3A register at 0x%.8x\n", addr);
		return 0;
	}
}

/*
 * The registers for the Direct Memory Access Controller. These are in the P2
 * area so the top 3 bits are ignored.
 */
#define DMAC_SAR0_OFF			0x04000020
#define DMAC_DAR0_OFF			0x04000024
#define DMAC_DMATCR0_OFF		0x04000028
#define DMAC_CHCR0_OFF			0x0400002C
#define DMAC_SAR1_OFF			0x04000030
#define DMAC_DAR1_OFF			0x04000034
#define DMAC_DMATCR1_OFF		0x04000038
#define DMAC_CHCR1_OFF			0x0400003C
#define DMAC_SAR2_OFF			0x04000040
#define DMAC_DAR2_OFF			0x04000044
#define DMAC_DMATCR2_OFF		0x04000048
#define DMAC_CHCR2_OFF			0x0400004C
#define DMAC_SAR3_OFF			0x04000050
#define DMAC_DAR3_OFF			0x04000054
#define DMAC_DMATCR3_OFF		0x04000058
#define DMAC_CHCR3_OFF			0x0400005C
#define DMAC_DMAOR_OFF			0x04000060
#define DMAC_CMSTR_OFF			0x04000070
#define DMAC_CMCSR0_OFF			0x04000072
#define DMAC_CMCNT0_OFF			0x04000074
#define DMAC_CMCNT1_OFF			0x04000076

/* TODO: the manual says that these fields can be accessed with other sizes */
static bool is_dmac_word_address(uint32_t addr)
{
	switch (addr) {
	case DMAC_DMAOR_OFF:
	case DMAC_CMSTR_OFF:
	case DMAC_CMCSR0_OFF:
	case DMAC_CMCNT0_OFF:
	case DMAC_CMCNT1_OFF:
		return true;
	default:
		return false;
	}
}

/* TODO: the manual says that these fields can be accessed with other sizes */
static bool is_dmac_longword_address(uint32_t addr)
{
	switch (addr) {
	case DMAC_SAR0_OFF:
	case DMAC_DAR0_OFF:
	case DMAC_DMATCR0_OFF:
	case DMAC_CHCR0_OFF:
	case DMAC_SAR1_OFF:
	case DMAC_DAR1_OFF:
	case DMAC_DMATCR1_OFF:
	case DMAC_CHCR1_OFF:
	case DMAC_SAR2_OFF:
	case DMAC_DAR2_OFF:
	case DMAC_DMATCR2_OFF:
	case DMAC_CHCR2_OFF:
	case DMAC_SAR3_OFF:
	case DMAC_DAR3_OFF:
	case DMAC_DMATCR3_OFF:
	case DMAC_CHCR3_OFF:
	case DMAC_DMAOR_OFF:
		return true;
	default:
		return false;
	}
}

static void dmac_write_word_reg(uint32_t addr, uint16_t val)
{
	switch (addr) {
	case DMAC_CMSTR_OFF:
		if (val)
			return panic("DMAC compare match timer not supported (0x%.2x)\n", val);
		return;
	default:
		return panic("Attempted write to unsupported DMAC register at 0x%.8x\n", addr);
	}
}

static uint32_t dmac_read_longword_reg(uint32_t addr)
{
	switch (addr) {
	case DMAC_CHCR0_OFF:
	case DMAC_CHCR2_OFF:
		return 0;
	default:
		panic("Attempted read of unsupported DMAC register at 0x%.8x\n", addr);
		return 0;
	}
}

static void dmac_write_longword_reg(uint32_t addr, uint32_t val)
{
	switch (addr) {
	case DMAC_CHCR0_OFF:
	case DMAC_CHCR2_OFF:
		if (val)
			return panic("DMAC not supported (0x%.2x)\n", val);
		return;
	default:
		return panic("Attempted write to unsupported DMAC register at 0x%.8x\n", addr);
	}
}

/*
 * The registers for the Serial Communication Interface are accessed at address
 * range 0xA4000150-0xA4000160, which is actually in the P2 area so the top 3
 * bits are ignored.
 */
#define SCIF_REGS_SIZE	16
#define SCIF_REGS_OFF	0x04000150

#define SCIF_SCSMR2_OFF		0x04000150
#define SCIF_SCBRR2_OFF		0x04000152
#define SCIF_SCSCR2_OFF		0x04000154
#define SCIF_SCFTDR2_OFF	0x04000156
#define SCIF_SCSSR2_OFF		0x04000158
#define SCIF_SCFRDR2_OFF	0x0400015A
#define SCIF_SCFCR2_OFF		0x0400015C
#define SCIF_SCFDR2_OFF		0x0400015E

#define SCIF_RX_QUEUE_SIZE	255

struct scif {
	uint8_t SCSMR2;			/* Serial mode register 2 */
	uint8_t SCBRR2;			/* Bit rate register 2 */
	uint8_t SCSCR2;			/* Serial control register 2 */
	uint8_t SCFTDR2[16];	/* Transmit FIFO data register 2 */
	uint16_t SCSSR2;		/* Serial status register 2 */
	uint8_t SCFRDR2[16];	/* Receive data FIFO register 2 */
	uint8_t SCFCR2;			/* FIFO control register 2 */

	/* These fields don't correspond to actual registers */
	uint8_t SCFTDR2_count;	/* Number of entries in SCFTDR2 FIFO */
	uint8_t SCFRDR2_count;	/* Number of entries in SCFRDR2 FIFO */
	uint8_t SCSSR2_unread;	/* Set SCSSR2 flags that haven't been read yet */

	/*
	 * Functions such as <0x80032090> only read one character at a time, so it
	 * would be very annoying to input serial from a script if we only relied
	 * in the SCFRDR2 buffer.
	 */
	uint8_t	rx_queue_start, rx_queue_end, rx_queue_cnt;
	uint8_t	rx_queue[SCIF_RX_QUEUE_SIZE];
} scif = {0};

/* Flags of the SCSCR2 register */
#define SCSCR2_TIE	(1U << 7)	/* Transmit interrupt enable */
#define SCSCR2_RIE	(1U << 6)	/* Receive interrupt enable */
#define SCSCR2_TE	(1U << 5)	/* Transmit enable */
#define SCSCR2_RE	(1U << 4)	/* Receive enable */
#define SCSCR2_CKE1	(1U << 1)	/* Clock enable 1 */
#define SCSCR2_CKE0	(1U << 0)	/* Clock enable 0 */
#define SCSCR2_BIT_MASK	(SCSCR2_TIE | SCSCR2_RIE | SCSCR2_TE | SCSCR2_RE | SCSCR2_CKE1 | SCSCR2_CKE0)

/*
 * Flags of the SCSSR2 register. The upper 8 bits are the number of receive
 * errors, so always zero in the emulator.
 */
#define SCSSR2_ER	(1U << 7)	/* Receive error */
#define SCSSR2_TEND	(1U << 6)	/* Transmit end */
#define SCSSR2_TDFE	(1U << 5)	/* Transmit FIFO data empty */
#define SCSSR2_BRK	(1U << 4)	/* Break detection */
#define SCSSR2_FER	(1U << 3)	/* Framing error */
#define SCSSR2_PER	(1U << 2)	/* Parity error */
#define SCSSR2_RDF	(1U << 1)	/* Receive FIFO data full */
#define SCSSR2_DR	(1U << 0)	/* Receive data ready */

/* Flags of the SCFCR2 register */
#define SCFCR2_RTRG1	(1U << 7)	/* Trigger of the Number of Receive FIFO Data 1 */
#define SCFCR2_RTRG0	(1U << 6)	/* Trigger of the Number of Receive FIFO Data 0 */
#define SCFCR2_TTRG1	(1U << 5)	/* Trigger of the Number of Transmit FIFO Data 1 */
#define SCFCR2_TTRG0	(1U << 4)	/* Trigger of the Number of Transmit FIFO Data 0 */
#define SCFCR2_MCE		(1U << 3)	/* Modem Control Enable */
#define SCFCR2_TFRST	(1U << 2)	/* Transmit FIFO Data Register Reset */
#define SCFCR2_RFRST	(1U << 1)	/* Receive FIFO Data Register Reset */
#define SCFCR2_LOOP		(1U << 0)	/* Loop Back Test */

static bool is_scif_byte_address(uint32_t addr)
{
	if (addr < SCIF_REGS_OFF || addr >= SCIF_REGS_OFF + SCIF_REGS_SIZE)
		return false;
	switch (addr) {
	case SCIF_SCSMR2_OFF:
	case SCIF_SCBRR2_OFF:
	case SCIF_SCSCR2_OFF:
	case SCIF_SCFTDR2_OFF:
	case SCIF_SCFRDR2_OFF:
	case SCIF_SCFCR2_OFF:
		return true;
	default:
		return false;
	}
}

static bool is_scif_word_address(uint32_t addr)
{
	if (addr < SCIF_REGS_OFF || addr >= SCIF_REGS_OFF + SCIF_REGS_SIZE)
		return false;
	switch (addr) {
	case SCIF_SCSSR2_OFF:
	case SCIF_SCFDR2_OFF:
		return true;
	default:
		return false;
	}
}

/* The only register for the Clock Pulse Generator */
#define CPG_FRQCR_OFF	0xFFFFFF80

struct cpg {
	uint16_t FRQCR;		/* Frequency Control Register */
} cpg = {
	.FRQCR = 0x0102,
};

static bool is_cpg_word_address(uint32_t addr)
{
	return addr == CPG_FRQCR_OFF;
}

static uint16_t cpg_read_word_reg(uint32_t addr)
{
	switch (addr) {
	case CPG_FRQCR_OFF:
		return cpg.FRQCR;
	default:
		panic("BUG: nonexistent register for the clock pulse generator\n");
		return 0;
	}
}

static void cpg_write_word_reg(uint32_t addr, uint16_t val)
{
	switch (addr) {
	case CPG_FRQCR_OFF:
		/* TODO: actually implement this register */
		cpg.FRQCR = val;
		return notice("CPG Frequency control register set to 0x%.4x\n", val);
	default:
		return panic("BUG: nonexistent register for the clock pulse generator\n");
	}
}

/*
 * The registers for the Watchdog Timer (WDT) are accessed at address range
 * 0xFFFFFF84 - 0xFFFFFF87.
 */
#define WDT_REGS_SIZE	4
uint8_t wdt_regs[WDT_REGS_SIZE] = {0};
#define WDT_REGS_OFF	0xFFFFFF84
#define WDT_REGS_END	(WDT_REGS_OFF + WDT_REGS_SIZE)

#define WDT_WTCNT_OFF	0xFFFFFF84
#define WDT_WTCSR_OFF	0xFFFFFF86

/* TODO: these seem to be redefined later? Probably a mistake. */
uint8_t stbcr_reg;
#define STBCR_OFF		0xFFFFFF82
uint8_t stbcr_2_reg;
#define STBCR2_OFF		0xFFFFFF88

/*
 * Some registers for the Interrupt Controller are accessed at address range
 * 0xFFFFFEE0-0xFFFFFEE5, others at 0x04000010-0x0400001B, and three byte-sized
 * more at 0x04000004, 0x04000006 and 0x04000008.
 */
#define INTC_ICR0_OFF	0xFFFFFEE0
#define INTC_IPRA_OFF	0xFFFFFEE2
#define INTC_IPRB_OFF	0xFFFFFEE4
#define	INTC_ICR1_OFF	0x04000010
#define	INTC_ICR2_OFF	0x04000012
#define	INTC_PINTER_OFF	0x04000014
#define	INTC_IPRC_OFF	0x04000016
#define	INTC_IPRD_OFF	0x04000018
#define	INTC_IPRE_OFF	0x0400001A
#define INTC_IRR0_OFF	0x04000004
#define INTC_IRR1_OFF	0x04000006
#define INTC_IRR2_OFF	0x04000008

/* Flags of the IRR0 register */
#define IRR0_PINT0R		(1U << 7)	/* PINT0 to PINT7 Interrupt Request */
#define IRR0_PINT1R		(1U << 6)	/* PINT8 to PINT15 Interrupt Request */
#define IRR0_IRQ5R		(1U << 5)	/* IRQ5 Interrupt Request */
#define IRR0_IRQ4R		(1U << 4)	/* IRQ4 Interrupt Request */
#define IRR0_IRQ3R		(1U << 3)	/* IRQ3 Interrupt Request */
#define IRR0_IRQ2R		(1U << 2)	/* IRQ2 Interrupt Request */
#define IRR0_IRQ1R		(1U << 1)	/* IRQ1 Interrupt Request */
#define IRR0_IRQ0R		(1U << 0)	/* IRQ0 Interrupt Request */
/* These bits can be reset to zero on a write, but not set to 1 */
#define IRR0_UNSETTABLE_MASK	(IRR0_IRQ5R | IRR0_IRQ4R | IRR0_IRQ3R | IRR0_IRQ2R | IRR0_IRQ1R | IRR0_IRQ0R)

/* Flags of the ICR0 register */
#define ICR0_NMIL		(1U << 15)	/* NMI Input Level */
#define ICR0_NMIE		(1U << 8)	/* NMI Edge Select */

struct intc {
	uint16_t ICR0;		/* Interrupt control register 0 */
	uint16_t ICR1;		/* Interrupt control register 1 */
	uint16_t ICR2;		/* Interrupt control register 2 */
	uint16_t PINTER;	/* PINT interrupt enable register */
	uint16_t IPRA;		/* Interrupt priority level setting register A */
	uint16_t IPRB;		/* Interrupt priority level setting register B */
	uint16_t IPRC;		/* Interrupt priority level setting register C */
	uint16_t IPRD;		/* Interrupt priority level setting register D */
	uint16_t IPRE;		/* Interrupt priority level setting register E */
	uint8_t IRR0;		/* Interrupt request register 0 */
	uint8_t IRR1;		/* Interrupt request register 1 */
	uint8_t IRR2;		/* Interrupt request register 2 */
} intc = {
	/* The NMI pin must be high on boot, or else we get stuck */
	.ICR0 = ICR0_NMIL,
};

static bool is_intc_byte_address(uint32_t addr)
{
	switch (addr) {
	case INTC_IRR0_OFF:
	case INTC_IRR1_OFF:
	case INTC_IRR2_OFF:
		return true;
	default:
		return false;
	}
}

static bool is_intc_word_address(uint32_t addr)
{
	switch (addr) {
	case INTC_ICR0_OFF:
	case INTC_IPRA_OFF:
	case INTC_IPRB_OFF:
	case INTC_ICR1_OFF:
	case INTC_ICR2_OFF:
	case INTC_PINTER_OFF:
	case INTC_IPRC_OFF:
	case INTC_IPRD_OFF:
	case INTC_IPRE_OFF:
		return true;
	default:
		return false;
	}
}

/*
 * The registers for the Realtime Clock are accessed on the even bytes of
 * address range 0xFFFFFEC0-0xFFFFFEDE.
 */
#define RTC_REGS_SIZE	31
#define RTC_REGS_OFF	0xFFFFFEC0

#define RTC_R64CNT_OFF	0xFFFFFEC0
#define RTC_RSECCNT_OFF	0xFFFFFEC2
#define RTC_RMINCNT_OFF	0xFFFFFEC4
#define RTC_RHRCNT_OFF	0xFFFFFEC6
#define RTC_RWKCNT_OFF	0xFFFFFEC8
#define RTC_RDAYCNT_OFF	0xFFFFFECA
#define RTC_RMONCNT_OFF	0xFFFFFECC
#define RTC_RYRCNT_OFF	0xFFFFFECE
#define RTC_RSECAR_OFF	0xFFFFFED0
#define RTC_RMINAR_OFF	0xFFFFFED2
#define RTC_RHRAR_OFF	0xFFFFFED4
#define RTC_RWKAR_OFF	0xFFFFFED6
#define RTC_RDAYAR_OFF	0xFFFFFED8
#define RTC_RMONAR_OFF	0xFFFFFEDA
#define RTC_RCR1_OFF	0xFFFFFEDC
#define RTC_RCR2_OFF	0xFFFFFEDE

struct rtc {
	uint8_t R64CNT;		/* 64-Hz counter */

	uint8_t RSECCNT;	/* Second counter */
	uint8_t RMINCNT;	/* Minute counter */
	uint8_t RHRCNT;		/* Hour counter */
	uint8_t RWKCNT;		/* Day of week counter */
	uint8_t RDAYCNT;	/* Date counter */
	uint8_t RMONCNT;	/* Month counter */
	uint8_t RYRCNT;		/* Year counter */

	uint8_t RSECAR;		/* Second alarm register */
	uint8_t RMINAR;		/* Minute alarm register */
	uint8_t RHRAR;		/* Hour alarm register */
	uint8_t RWKAR;		/* Day of week alarm register */
	uint8_t RDAYAR;		/* Date alarm register */
	uint8_t RMONAR;		/* Month alarm register */

	uint8_t RCR1;		/* RTC control register 1 */
	uint8_t RCR2;		/* RTC control register 2 */

	/*
	 * Count of peripheral tics since last R64CNT update. TODO: don't keep
	 * separate counts for rtc and tmu, simplify this somehow.
	 */
	int peripheral_tics;
} rtc = {0};

/* Flags of the RCR1 register */
#define RCR1_CF		(1U << 7)	/* Carry flag */
#define RCR1_CIE	(1U << 4)	/* Carry interrupt enable flag */
#define RCR1_AIE	(1U << 3)	/* Alarm interrupt enable flag */
#define RCR1_AF		(1U << 0)	/* Alarm flag */
#define RCR1_BIT_MASK	(RCR1_CF | RCR1_CIE | RCR1_AIE | RCR1_AF)
/* These bits can be reset to zero on a write, but not set to 1 */
#define RCR1_UNSETTABLE_MASK	(RCR1_AF)

/* Flags of the RCR2 register */
#define RCR2_PEF	(1U << 7)	/* Periodic Interrupt Flag */
#define RCR2_PES	(7U << 4)	/* Periodic Interrupt Flags */
#define RCR2_RTCEN	(1U << 3)	/* Operation of the crystal oscillator */
#define RCR2_ADJ	(1U << 2)	/* 30 Second Adjustment */
#define RCR2_RESET	(1U << 1)	/* Reset */
#define RCR2_START	(1U << 0)	/* Start */
#define RCR2_BIT_MASK	(RCR2_PEF | RCR2_PES | RCR2_RTCEN | RCR2_ADJ | RCR2_RESET | RCR2_START)

static bool is_rtc_address(uint32_t addr)
{
	if (addr & 1)
		return false;
	return addr >= RTC_REGS_OFF && addr < RTC_REGS_OFF + RTC_REGS_SIZE;
}

/*
 * The control registers for the power-down modes are through the bytes on the
 * following two addresses:
 */
#define PDM_STBCR_OFF	0xFFFFFF82
#define PDM_STBCR2_OFF	0xFFFFFF88

/* Flags of the STBCR2 register */
#define STBCR2_MDCHG	(1U << 6)	/* Pin MD5 to MD0 Control */
#define STBCR2_MSTP8	(1U << 5)	/* Module Stop 8 */
#define STBCR2_MSTP7	(1U << 4)	/* Module Stop 7 */
#define STBCR2_MSTP6	(1U << 3)	/* Module Stop 6 */
#define STBCR2_MSTP5	(1U << 2)	/* Module Stop 5 */
#define STBCR2_MSTP4	(1U << 1)	/* Module Stop 4 */
#define STBCR2_MSTP3	(1U << 0)	/* Module Stop 3 */

struct pdm {
	uint8_t STBCR;	/* Standby control register */
	uint8_t STBCR2;	/* Standby control register 2 */
} pdm = {0};

static bool is_pdm_address(uint32_t addr)
{
	return addr == PDM_STBCR_OFF || addr == PDM_STBCR2_OFF;
}

/*
 * The registers for the User Break Controller are accessed through some words
 * and longwords in address range 0xFFFFFFA0-0xFFFFFFE9.
 */
#define UBC_REGS_SIZE	74
#define UBC_REGS_OFF	0xFFFFFFA0

#define UBC_BARA_OFF	0xFFFFFFB0
#define UBC_BAMRA_OFF	0xFFFFFFB4
#define UBC_BARB_OFF	0xFFFFFFA0
#define UBC_BAMRB_OFF	0xFFFFFFA4
#define UBC_BDRB_OFF	0xFFFFFF90
#define UBC_BDMRB_OFF	0xFFFFFF94
#define UBC_BRCR_OFF	0xFFFFFF98
#define UBC_BRSR_OFF	0xFFFFFFAC
#define UBC_BRDR_OFF	0xFFFFFFBC

struct ubc {
	uint32_t BARA;		/* Break address register A */
	uint32_t BAMRA;		/* Break address mask register A */
	uint32_t BARB;		/* Break address register B */
	uint32_t BAMRB;		/* Break address mask register B */
	uint32_t BDRB;		/* Break data register B */
	uint32_t BDMRB;		/* Break data mask register B */
	uint32_t BRCR;		/* Break control register */
	uint32_t BRSR;		/* Branch source register */
	uint32_t BRDR;		/* Branch destination register */
} ubc = {0};

/* TODO: support the word addresses as well */
static bool is_ubc_longword_address(uint32_t addr)
{
	if (addr < UBC_REGS_OFF || addr >= UBC_REGS_OFF + UBC_REGS_SIZE)
		return false;
	switch (addr) {
	case UBC_BARA_OFF:
	case UBC_BAMRA_OFF:
	case UBC_BARB_OFF:
	case UBC_BAMRB_OFF:
	case UBC_BDRB_OFF:
	case UBC_BDMRB_OFF:
	case UBC_BRCR_OFF:
	case UBC_BRSR_OFF:
	case UBC_BRDR_OFF:
		return true;
	default:
		return false;
	}
}

/*
 * The registers for cache control. We don't actually implement the cache so
 * these are trivial to handle.
 */
#define CACHE_CCR_OFF	0xFFFFFFEC
#define CACHE_CCR2_OFF	0x040000B0

struct cache {
	uint32_t CCR;	/* Cache control register */
} cache = {0};

/* Flags of the CCR register */
#define CCR_CF			(1U << 3)	/* Cache flush bit */
#define CCR_CB			(1U << 2)	/* Cache write-back bit */
#define CCR_WT			(1U << 1)	/* Write-through bit */
#define CCR_CE			(1U << 0)	/* Cache enable bit */
#define CCR_BIT_MASK	(CCR_CF | CCR_CB | CCR_WT | CCR_CE)

static bool is_cache_longword_address(uint32_t addr)
{
	switch (addr) {
	case CACHE_CCR_OFF:
	case CACHE_CCR2_OFF:
		return true;
	default:
		return false;
	}
}

static void cache_write_longword_reg(uint32_t addr, uint32_t val)
{
	switch (addr) {
	case CACHE_CCR_OFF:
		if (val & ~CCR_BIT_MASK)
			return panic("Bad value set on CCR\n");
		cache.CCR = val;
		return;
	default:
		return panic("Attempted write to unsupported cache register at 0x%.8x\n", addr);
	}
}

static uint32_t cache_read_longword_reg(uint32_t addr)
{
	switch (addr) {
	case CACHE_CCR_OFF:
		return cache.CCR;
	default:
		panic("Attempted read from unsupported cache register at 0x%.8x\n", addr);
		return 0;
	}
}

/*
 * The registers for the Timer Unit are accessed through bytes, words and
 * longwords in address range 0xFFFFFE90-0xFFFFFEBB.
 */
#define TMU_REGS_SIZE	44
#define TMU_REGS_OFF	0xFFFFFE90

#define TMU_TOCR_OFF	0xFFFFFE90
#define TMU_TSTR_OFF	0xFFFFFE92
#define TMU_TCR0_OFF	0xFFFFFE9C
#define TMU_TCR1_OFF	0xFFFFFEA8
#define TMU_TCR2_OFF	0xFFFFFEB4
#define TMU_TCOR0_OFF	0xFFFFFE94
#define TMU_TCNT0_OFF	0xFFFFFE98
#define TMU_TCOR1_OFF	0xFFFFFEA0
#define TMU_TCNT1_OFF	0xFFFFFEA4
#define TMU_TCOR2_OFF	0xFFFFFEAC
#define TMU_TCNT2_OFF	0xFFFFFEB0
#define TMU_TCPR2_OFF	0xFFFFFEB8

struct tmu {
	uint8_t TOCR;		/* Timer output control register */
	uint8_t TSTR;		/* Timer start register */
	uint16_t TCR[3];	/* Timer control register 0-2 */
	uint32_t TCOR[3];	/* Timer constant register 0-2 */
	uint32_t TCNT[3];	/* Timer counter 0-2 */
	uint32_t TCPR2;		/* Input capture register 2 */

	/* Count of peripheral clock tics since last corresponding TCNT update */
	int peripheral_tics[3];
} tmu = {0};

/* Flags of the TCR0-2 registers */
#define TCR_ICPF	(1U << 9)	/* Input capture interrupt flag */
#define TCR_UNF		(1U << 8)	/* Underflow flag */
#define TCR_ICPE	(3U << 6)	/* Input capture control */
#define TCR_UNIE	(1U << 5)	/* Underflow interrupt control */
#define TCR_CKEG	(3U << 3)	/* Clock edge 1 and 2 */
#define TCR_TPSC	(7U << 0)	/* Timer Prescalers */
/* Some bits are only valid for channel 2 */
#define TCR_0_1_BIT_MASK	(TCR_UNF | TCR_UNIE | TCR_CKEG | TCR_TPSC)
#define TCR_2_BIT_MASK		(TCR_0_1_BIT_MASK | TCR_ICPF | TCR_ICPE)
/* These bits can be reset to zero on a write, but not set to 1 */
#define TCR_UNSETTABLE_MASK	(TCR_ICPF | TCR_UNF)

#define TSTR_STR2	(1U << 2)	/* Counter Start 2 */
#define TSTR_STR1	(1U << 1)	/* Counter Start 1 */
#define TSTR_STR0	(1U << 0)	/* Counter Start 0 */
#define TSTR_BIT_MASK	(TSTR_STR2 | TSTR_STR1 | TSTR_STR0)

/* TODO: support longword addresses as well */
static bool is_tmu_byte_address(uint32_t addr)
{
	if (addr < TMU_REGS_OFF || addr >= TMU_REGS_OFF + TMU_REGS_SIZE)
		return false;
	switch (addr) {
	case TMU_TOCR_OFF:
	case TMU_TSTR_OFF:
		return true;
	default:
		return false;
	}
}

static bool is_tmu_word_address(uint32_t addr)
{
	if (addr < TMU_REGS_OFF || addr >= TMU_REGS_OFF + TMU_REGS_SIZE)
		return false;
	switch (addr) {
	case TMU_TCR0_OFF:
	case TMU_TCR1_OFF:
	case TMU_TCR2_OFF:
		return true;
	default:
		return false;
	}
}

static bool is_tmu_longword_address(uint32_t addr)
{
	if (addr < TMU_REGS_OFF || addr >= TMU_REGS_OFF + TMU_REGS_SIZE)
		return false;
	switch (addr) {
	case TMU_TCOR0_OFF:
	case TMU_TCNT0_OFF:
	case TMU_TCOR1_OFF:
	case TMU_TCNT1_OFF:
	case TMU_TCOR2_OFF:
	case TMU_TCNT2_OFF:
	case TMU_TCPR2_OFF:
		return true;
	default:
		return false;
	}
}

/* The registers for the Memory Management Unit */
#define MMU_PTEH_OFF		0xFFFFFFF0
#define MMU_PTEL_OFF		0xFFFFFFF4
#define MMU_TTB_OFF			0xFFFFFFF8
#define MMU_TEA_OFF			0xFFFFFFFC
#define MMU_MMUCR_OFF		0xFFFFFFE0
/* The TLB can also be accessed through these arrays */
#define MMU_TLB_ADDR_OFF	0xF2000000
#define MMU_TLB_ADDR_LEN	0x01000000
#define MMU_TLB_DATA_OFF	0xF3000000
#define MMU_TLB_DATA_LEN	0x01000000

struct mmu {
	uint32_t PTEH;		/* Page table entry register high */
	uint32_t PTEL;		/* Page table entry register low */
	uint32_t TTB;		/* Translation table base register */
	uint32_t TEA;		/* TLB exception address register */
	uint32_t MMUCR;		/* MMU control register */

	/* The actual TLB, split in address and data arrays */
	uint32_t tlb_addr[32][4];
	uint32_t tlb_data[32][4];
} mmu = {0};

/* Fields of the PTEH register */
#define PTEH_VPN_MASK	0xFFFFFC00	/* Virtual page number */
#define PTEH_VPN_SHIFT	10
#define PTEH_ASID_MASK	0x000000FF	/* Address space identifier */
#define PTEH_ASID_SHIFT	0
#define PTEH_BIT_MASK	(PTEH_VPN_MASK | PTEH_ASID_MASK)

/* Fields of the PTEL register */
#define PTEL_PPN_MASK	0xFFFFFC00	/* Physical page number */
#define PTEL_PPN_SHIFT	10
#define PTEL_V			(1U << 8)	/* Valid bit */
#define PTEL_PR_MASK	(3U << 5)	/* Protection key */
#define PTEL_PR_SHIFT	5
#define PTEL_SZ			(1U << 4)	/* Size bit */
#define PTEL_C			(1U << 3)	/* Cacheable bit */
#define PTEL_D			(1U << 2)	/* Dirty bit */
#define PTEL_SH			(1U << 1)	/* Share status bit */
#define PTEL_BIT_MASK	(PTEL_PPN_MASK | PTEL_V | PTEL_PR_MASK| PTEL_SZ | PTEL_C | PTEL_D | PTEL_SH)

/* Fields of the MMUCR register */
#define MMUCR_SV		(1U << 8)	/* Single virtual memory mode bit */
#define MMUCR_RC_MASK	(3U << 4)	/* Random counter */
#define MMUCR_RC_SHIFT	4
#define MMUCR_TF		(1U << 2)	/* TLB flush bit */
#define MMUCR_IX		(1U << 1)	/* Index mode bit */
#define MMUCR_AT		(1U << 0)	/* Address translation bit */
#define MMUCR_BIT_MASK	(MMUCR_SV | MMUCR_RC_MASK | MMUCR_TF | MMUCR_IX | MMUCR_AT)

/* Fields of each TLB address entry */
#define TLB_VPN_31_17_MASK	(0x007FFFU << 13)	/* Virtual page number (bits 31-17) */
#define TLB_VPN_31_17_SHIFT	13
#define TLB_VPN_11_10_MASK	(0x000003U << 11)	/* Virtual page number (bits 11-10) */
#define TLB_VPN_11_10_SHIFT	11
#define TLB_ASID_MASK		(0x0000FFU << 3)	/* Address space identifier */
#define TLB_ASID_SHIFT		3
#define TLB_SH				(0x000001U << 2)	/* Share status bit */
#define TLB_SZ				(0x000001U << 1)	/* Page-size bit */
#define TLB_V				(0x000001U << 0)	/* Valid bit */

/* Fields of each TLB data entry */
#define TLB_PPN_MASK		(0x3FFFFFU << 4)	/* Physical page number (top 22 bits) */
#define TLB_PPN_SHIFT		4
#define TLB_PR_MASK			(0x000003U << 2)	/* Protection key field */
#define TLB_PR_SHIFT		2
#define TLB_C				(0x000001U << 1)	/* Cacheable bit */
#define TLB_D				(0x000001U << 0)	/* Dirty bit */

static bool is_mmu_longword_address(uint32_t addr)
{
	if (addr >= MMU_TLB_ADDR_OFF && addr < MMU_TLB_ADDR_OFF + MMU_TLB_ADDR_LEN)
		return true;
	if (addr >= MMU_TLB_DATA_OFF && addr < MMU_TLB_DATA_OFF + MMU_TLB_DATA_LEN)
		return true;
	switch (addr) {
	case MMU_PTEH_OFF:
	case MMU_PTEL_OFF:
	case MMU_TTB_OFF:
	case MMU_TEA_OFF:
	case MMU_MMUCR_OFF:
		return true;
	default:
		return false;
	}
}

static void mmu_flush_tlb(void)
{
	int way, entry;

	for (way = 0; way < 4; ++way) {
		for (entry = 0; entry < 32; ++entry)
			mmu.tlb_addr[entry][way] &= ~TLB_V;
	}
}

/* Extract bits 16-12 of the virtual address to be used as the TLB index */
static int mmu_virt_to_index(uint32_t va)
{
	return (va >> 12) & 0x1F;
}

static void mmu_load_pte_to_tlb(void)
{
	uint32_t tlb_addr, tlb_data;
	uint32_t *tlb_addr_p = NULL, *tlb_data_p = NULL;
	int way, entry;
	uint32_t virt_addr, phys_addr, asid, protection;

	/* We always assume 1 KiB page size */
	virt_addr = mmu.PTEH & PTEH_VPN_MASK;
	phys_addr = mmu.PTEL & PTEL_PPN_MASK;
	asid = (mmu.PTEH & PTEH_ASID_MASK) >> PTEH_ASID_SHIFT;
	protection = (mmu.PTEL & PTEL_PR_MASK) >> PTEL_PR_SHIFT;

	way = (mmu.MMUCR & MMUCR_RC_MASK) >> MMUCR_RC_SHIFT;
	entry = mmu_virt_to_index(virt_addr);

	tlb_addr_p = &mmu.tlb_addr[entry][way];
	tlb_data_p = &mmu.tlb_data[entry][way];

	/* Each TLB address entry has 28 bits */
	tlb_addr = 0;
	tlb_addr |= (virt_addr >> 4) & TLB_VPN_31_17_MASK;
	tlb_addr |= (virt_addr << 1) & TLB_VPN_11_10_MASK;
	tlb_addr |= asid << 3;
	if (mmu.PTEL & PTEL_SH)
		tlb_addr |= TLB_SH;
	if (mmu.PTEL & PTEL_SZ)
		return panic("4 KiB page size not supported\n");
	if (mmu.PTEL & PTEL_V)
		tlb_addr |= TLB_V;

	/* Each TLB data entry has 26 bits */
	tlb_data = 0;
	tlb_data |= phys_addr >> 6;
	tlb_data |= protection << 2;
	if (mmu.PTEL & PTEL_C)
		tlb_data |= TLB_C;
	if (mmu.PTEL & PTEL_D)
		return panic("Attempt to dirty a page\n");

	*tlb_addr_p = tlb_addr;
	*tlb_data_p = tlb_data;
}

static void mmu_write_longword_reg(uint32_t addr, uint32_t val)
{
	switch (addr) {
	case MMU_PTEH_OFF:
		mmu.PTEH = val & PTEH_BIT_MASK;
		break;
	case MMU_PTEL_OFF:
		mmu.PTEL = val & PTEL_BIT_MASK;
		break;
	case MMU_TTB_OFF:
		mmu.TTB = val;
		break;
	case MMU_TEA_OFF:
		mmu.TEA = val;
		break;
	case MMU_MMUCR_OFF:
		if (val & MMUCR_TF)
			mmu_flush_tlb();
		/* I only support the configuration used by my Jornada's firmware */
		if (val & MMUCR_SV)
			return panic("MMU single vm mode not supported (0x%.8x)\n", val);
		if (val & MMUCR_IX)
			return panic("MMU index mode 1 not supported (0x%.8x)\n", val);
		/*
		 * According to the manual, the reserved bits in the MMUCR are special
		 * in that "0 should also be specified in a write to MMUCR only". No
		 * idea what that means, so I'll just ignore those bits as usual.
		 */
		mmu.MMUCR = val & MMUCR_BIT_MASK;
		break;
	default:
		panic("Attempted write to unsupported MMU register at 0x%.8x\n", addr);
		return;
	}
}

static uint32_t mmu_read_longword_reg(uint32_t addr)
{
	switch (addr) {
	case MMU_PTEH_OFF:
		return mmu.PTEH;
	default:
		panic("Attempted read from unsupported MMU register at 0x%.8x\n", addr);
		return 0;
	}
}

/* Offset of each of the virtual memory areas P0 to P4 */
static uint32_t mmu_area_offs[] = {
	0x00000000,
	0x80000000,
	0xA0000000,
	0xC0000000,
	0xE0000000
};

static int mmu_virt_to_area(uint32_t va)
{
	int i;

	for (i = 4; i >= 0; --i) {
		if (va >= mmu_area_offs[i])
			return i;
	}
	panic("BUG: va not covered by any area\n");
	return 0;
}

static uint32_t mmu_tlb_to_va(uint32_t tlb_addr, int index)
{
	uint32_t va;

	va = 0;
	va |= (tlb_addr & TLB_VPN_31_17_MASK) << (17 - TLB_VPN_31_17_SHIFT);
	va |= index << 12;	/* The index are bits 16-12 */
	va |= (tlb_addr & TLB_VPN_11_10_MASK) >> (TLB_VPN_11_10_SHIFT - 10);
	return va;
}

static uint32_t mmu_tlb_to_pa(uint32_t tlb_data)
{
	return (tlb_data & TLB_PPN_MASK) << (10 - TLB_PPN_SHIFT);
}

#define PAGE_MASK	(~((1 << 10) - 1))

/* TODO: handle overlaps between mmu and debugger mappings */
static uint32_t mmu_virt_to_phys(uint32_t va)
{
	int area, entry, way;
	uint32_t tlb_addr, tlb_data, vpage_addr;

	/* No translation if the mmu is disabled */
	if (!(mmu.MMUCR & MMUCR_AT))
		return va;

	/* Only vm areas P0 and P3 get translated */
	area = mmu_virt_to_area(va);
	if (area != 0 && area != 3)
		return va;

	vpage_addr = va & PAGE_MASK;

	/* TODO: check protections and process privilege */
	entry = mmu_virt_to_index(va);
	for (way = 0; way < 4; ++way) {
		tlb_addr = mmu.tlb_addr[entry][way];
		tlb_data = mmu.tlb_data[entry][way];
		if (!(tlb_addr & TLB_V))
			continue;
		if (mmu_tlb_to_va(tlb_addr, entry) == vpage_addr)
			return mmu_tlb_to_pa(tlb_data) + (va - vpage_addr);
	}

	panic("Page faults not yet implemented\n");
	return 0;
}



char *progname = NULL;
bool interactive = false;
FILE *script_file = NULL;

#define MAX_BREAKPOINTS	128

struct breakpoints {
	/* Breakpoints for a given program counter */
	int pcs_count;
	uint32_t pcs[MAX_BREAKPOINTS];

	bool hit;
} breakpoints = {0};

struct vm_mapping {
	uint32_t virt;
	uint32_t phys;
	uint32_t len;
};

#define MAX_MAPPINGS	32

/* Fake vm mappings */
struct vm_mappings {
	int map_count;
	struct vm_mapping maps[MAX_MAPPINGS];
} vm_mappings = {0};

struct patch {
	uint32_t addr;
	uint32_t insn;
};

#define MAX_PATCHES		128

struct patches {
	int patch_count;
	struct patch p[MAX_PATCHES];
} patches = {0};

struct bt_entry {
	uint32_t origin;
	uint32_t target;
	bool interrupt;
};

#define MAX_BACKTRACE	128

/* I don't think I can reliably rebuild the backtrace from the stack alone */
struct backtrace {
	int bt_count;
	struct bt_entry bt_entries[MAX_BACKTRACE];
} backtrace = {0};

static void backtrace_push(uint32_t origin, uint32_t target, bool interrupt)
{
	struct bt_entry *entry = NULL;

	if (backtrace.bt_count == MAX_BACKTRACE) {
		notice("Backtrace stack seems full?\n");
		return;
	}
	entry = &backtrace.bt_entries[backtrace.bt_count++];
	entry->origin = origin - 4;
	entry->target = target - 4;
	entry->interrupt = interrupt;
}

static void backtrace_pop(void)
{
	if (backtrace.bt_count == 0) {
		notice("Backtrace stack seems empty?\n");
		return;
	}
	--backtrace.bt_count;
}

static void backtrace_clear(void)
{
	backtrace.bt_count = 0;
}

static void usage(void)
{
	fprintf(stderr, "usage: %s [-i|-f script] [-C cardfile] path-to-firmware\n", progname);
	exit(1);
}

static int emulate(void);

int main(int argc, char *argv[])
{
	char *fw_name = NULL;
	FILE *fw_file = NULL;
	char *script_name = NULL;
	char *card_name = NULL;
	size_t ret;
	int i;

	if (argc == 0)
		return 1;
	progname = argv[0];

	if (argc < 2)
		usage();
	for (i = 1; i < argc; ++i) {
		if (strcmp(argv[i], "-i") == 0) {
			interactive = true;
		} else if (strcmp(argv[i], "-f") == 0) {
			if (++i == argc)
				usage();
			script_name = argv[i];
		} else if (strcmp(argv[i], "-C") == 0) {
			if (++i == argc)
				usage();
			card_name = argv[i];
		} else if (i == argc - 1) {
			fw_name = argv[i];
		} else {
			usage();
		}
	}
	if (!fw_name)
		usage();
	if (interactive && script_name)
		usage();

	fw_file = fopen(fw_name, "rb");
	if (!fw_file) {
		perror(progname);
		return 1;
	}
	ret = fread(firmware, 1, FIRMWARE_SIZE, fw_file);
	if (ret != FIRMWARE_SIZE) {
		if (ferror(fw_file))
			fprintf(stderr, "%s: failed to read firmware file\n", progname);
		else
			fprintf(stderr, "%s: wrong size of firmware file\n", progname);
		return 1; /* TODO: cleanup */
	}
	fclose(fw_file);
	fw_file = NULL;

	/* TODO: cleanups on exit */
	if (script_name) {
		script_file = fopen(script_name, "r");
		if (!script_file) {
			perror(progname);
			return 1;
		}
	}

	/* TODO: debugger commands to insert and eject the card */
	if (card_name) {
		card_file = fopen(card_name, "r+");
		if (!card_file) {
			perror(progname);
			return 1;
		}
		if (fseek(card_file, 0, SEEK_END)) {
			perror(progname);
			return 1;
		}
		/* TODO: should I check that the size is sensible? */
		card_size = ftell(card_file);
		if (card_size < 0) {
			perror(progname);
			return 1;
		}
	}

	set_signal_handlers();
	return emulate();
}

static uint32_t read_gp_register(int n)
{
	if (cpu.SR & SR_RB_BIT && n < 8)
		return cpu.R_BANK1[n];
	return cpu.R[n];
}

static void write_gp_register(int n, uint32_t val)
{
	if (cpu.SR & SR_RB_BIT && n < 8)
		cpu.R_BANK1[n] = val;
	else
		cpu.R[n] = val;
}

static uint32_t sign_extend_byte(uint8_t val)
{
	return val | (val & 0x80 ? 0xFFFFFF00 : 0);
}

static uint32_t sign_extend_word(uint16_t val)
{
	return val | (val & 0x8000 ? 0xFFFF0000 : 0);
}

static uint32_t sign_extend_lower_8(uint16_t val)
{
	val = (val & 0x00FFU);
	return val | (val & 0x0080 ? 0xFFFFFF00 : 0);
}

static uint32_t sign_extend_lower_12(uint16_t val)
{
	val = (val & 0x0FFFU);
	return val | (val & 0x0800 ? 0xFFFFF000 : 0);
}

static uint32_t p1_p2_to_phys(uint32_t addr)
{
	/*
	 * Top 3 bits are ignored for both P1 and P2 areas, so they map to the same
	 * physical range.
	 */
	if (addr >= 0x80000000 && addr < 0xC0000000)
		return addr & ~0xE0000000;
	return addr;
}

static uint32_t mock_va_translation(uint32_t addr)
{
	int i;

	for (i = 0; i < vm_mappings.map_count; ++i) {
		if (addr < vm_mappings.maps[i].virt)
			continue;
		if (addr >= vm_mappings.maps[i].virt + vm_mappings.maps[i].len)
			continue;
		return addr - vm_mappings.maps[i].virt + vm_mappings.maps[i].phys;
	}
	return addr;
}

#if 0
#define DEBUG_PRINT(...)	printf(__VA_ARGS__)
#else
#define DEBUG_PRINT(...)	do {} while (0)
#endif

static uint16_t read_scif_word_reg(uint32_t addr)
{
	switch (addr) {
	case SCIF_SCSSR2_OFF:
		scif.SCSSR2_unread = 0;
		return scif.SCSSR2;
	case SCIF_SCFDR2_OFF:
		return (scif.SCFTDR2_count << 8) & scif.SCFRDR2_count;
	default:
		panic("Attempted read of unsupported SCIF register at 0x%.8x\n", addr);
		return 0;
	}
}

static uint8_t read_scif_byte_reg(uint32_t addr)
{
	switch (addr) {
	case SCIF_SCSMR2_OFF:
		return scif.SCSMR2;
	case SCIF_SCBRR2_OFF:
		return scif.SCBRR2;
	case SCIF_SCSCR2_OFF:
		return scif.SCSCR2;
	case SCIF_SCFRDR2_OFF:
		if (scif.SCFRDR2_count == 0)
			return 0; /* "Undefined", so whatever */
		if (scif.SCFRDR2_count == 1)
			scif.SCSSR2 &= ~SCSSR2_RDF;
		/* TODO: data ready bit? */
		return scif.SCFRDR2[--scif.SCFRDR2_count];
	case SCIF_SCFCR2_OFF:
		return scif.SCFCR2;
	case SCIF_SCFTDR2_OFF:
	default:
		panic("Attempted read of unsupported SCIF register at 0x%.8x\n", addr);
		return 0;
	}
}

static void write_scif_word_reg(uint32_t addr, uint16_t val)
{
	switch (addr) {
	case SCIF_SCSSR2_OFF:
		val &= scif.SCSSR2; /* No flags can be set to 1 by a write */
		scif.SCSSR2 = (scif.SCSSR2 & scif.SCSSR2_unread) | val;
		return;
	case SCIF_SCFDR2_OFF:
		return;
	default:
		panic("Attempted write to unsupported SCIF register at 0x%.8x\n", addr);
		return;
	}
}

static void write_scif_byte_reg(uint32_t addr, uint8_t val)
{
	switch (addr) {
	case SCIF_SCSMR2_OFF:
		/* TODO: character length could matter? Is this register even used? */
		scif.SCSMR2 = val & 0x7BU;
		return;
	case SCIF_SCBRR2_OFF:
		scif.SCBRR2 = val;
		return;
	case SCIF_SCSCR2_OFF:
		scif.SCSCR2 = val & SCSCR2_BIT_MASK;
		return;
	case SCIF_SCFTDR2_OFF:
		/* TODO: generic fifo structure? Ring buffer implementation? */
		if (scif.SCFTDR2_count == 16)
			return;
		memmove(&scif.SCFTDR2[1], &scif.SCFTDR2[0], scif.SCFTDR2_count++);
		scif.SCFTDR2[0] = val;
		scif.SCSSR2 &= ~(SCSSR2_TEND | SCSSR2_TDFE);
		/* TODO: serial interrupts? Are they even used by the jornada? */
		return;
	case SCIF_SCFRDR2_OFF:
		/* TODO: exception or something? Not documented */
		return;
	case SCIF_SCFCR2_OFF:
		/* Not encountered yet, fallthrough */
	default:
		panic("Attempted write to unsupported SCIF register at 0x%.8x\n", addr);
		return;
	}
}

static uint32_t read_tmu_longword_reg(uint32_t addr)
{
	switch (addr) {
	case TMU_TCOR0_OFF:
		return tmu.TCOR[0];
	case TMU_TCNT0_OFF:
		return tmu.TCNT[0];
	case TMU_TCOR1_OFF:
		return tmu.TCOR[1];
	case TMU_TCNT1_OFF:
		return tmu.TCNT[1];
	case TMU_TCOR2_OFF:
		return tmu.TCOR[2];
	case TMU_TCNT2_OFF:
		return tmu.TCNT[2];
	case TMU_TCPR2_OFF:
		return tmu.TCPR2;
	default:
		panic("Attempted read of unsupported TMU register at 0x%.8x\n", addr);
		return 0;
	}
}

static uint16_t read_tmu_word_reg(uint32_t addr)
{
	switch (addr) {
	case TMU_TCR0_OFF:
		return tmu.TCR[0];
	case TMU_TCR1_OFF:
		return tmu.TCR[1];
	case TMU_TCR2_OFF:
		return tmu.TCR[2];
	default:
		panic("Attempted read of unsupported TMU register at 0x%.8x\n", addr);
		return 0;
	}
}

static uint8_t read_tmu_byte_reg(uint32_t addr)
{
	switch (addr) {
	case TMU_TOCR_OFF:
		return tmu.TOCR;
	case TMU_TSTR_OFF:
		return tmu.TSTR;
	default:
		panic("Attempted read of unsupported TMU register at 0x%.8x\n", addr);
		return 0;
	}
}

static void tmu_write_byte_reg(uint32_t addr, uint8_t val)
{
	switch (addr) {
	case TMU_TSTR_OFF:
		val &= TSTR_BIT_MASK;
		tmu.TSTR = val;
		return;
	default:
		panic("Attempted write to unsupported TMU register at 0x%.8x\n", addr);
		return;
	}
}

static void tmu_write_word_reg(uint32_t addr, uint16_t val)
{
	uint16_t *tcr = NULL;
	uint16_t preserved_bits;

	if (addr == TMU_TCR0_OFF)
		tcr = &tmu.TCR[0];
	else if (addr == TMU_TCR1_OFF)
		tcr = &tmu.TCR[1];
	else if (addr == TMU_TCR2_OFF)
		tcr = &tmu.TCR[2];

	switch (addr) {
	case TMU_TCR0_OFF:
	case TMU_TCR1_OFF:
		val &= TCR_0_1_BIT_MASK;
		if (val & TCR_CKEG) {
			panic("Unsupported timer configuration for TCR (0x%.2x)\n", val);
			return;
		}
		if ((val & TCR_TPSC) & 4) {
			panic("Unsupported TCNT clock input (TCR: 0x%.2x)\n", val);
			return;
		}
		preserved_bits = val & TCR_UNSETTABLE_MASK;
		val = (val & ~preserved_bits) | (*tcr & preserved_bits);
		*tcr = val;
		return;
	default:
		panic("Attempted write to unsupported TMU register at 0x%.8x\n", addr);
		return;
	}
}

static void tmu_write_longword_reg(uint32_t addr, uint32_t val)
{
	switch (addr) {
	case TMU_TCOR0_OFF:
		tmu.TCOR[0] = val;
		break;
	case TMU_TCNT0_OFF:
		tmu.TCNT[0] = val;
		break;
	case TMU_TCOR1_OFF:
		tmu.TCOR[1] = val;
		break;
	case TMU_TCNT1_OFF:
		tmu.TCNT[1] = val;
		break;
	case TMU_TCOR2_OFF:
		tmu.TCOR[2] = val;
		break;
	case TMU_TCNT2_OFF:
		tmu.TCNT[2] = val;
		break;
	default:
		panic("Attempted write to unsupported TMU register at 0x%.8x\n", addr);
		return;
	}
}

static uint32_t read_ubc_longword_reg(uint32_t addr)
{
	switch (addr) {
	case UBC_BARA_OFF:
		return ubc.BARA;
	case UBC_BAMRA_OFF:
		return ubc.BAMRA;
	case UBC_BARB_OFF:
		return ubc.BARB;
	case UBC_BAMRB_OFF:
		return ubc.BAMRB;
	case UBC_BDRB_OFF:
		return ubc.BDRB;
	case UBC_BDMRB_OFF:
		return ubc.BDMRB;
	case UBC_BRCR_OFF:
		return ubc.BRCR;
	case UBC_BRSR_OFF:
		return ubc.BRSR;
	case UBC_BRDR_OFF:
		return ubc.BRDR;
	default:
		panic("Attempted read of unsupported UBC register at 0x%.8x\n", addr);
		return 0;
	}
}

static uint8_t read_pdm_reg(uint32_t addr)
{
	switch (addr) {
	case PDM_STBCR_OFF:
		return pdm.STBCR;
	case PDM_STBCR2_OFF:
		return pdm.STBCR2;
	default:
		panic("Attempted read of unsupported pdm register at 0x%.8x\n", addr);
		return 0;
	}
}

static void pdm_write_byte_reg(uint32_t addr, uint8_t val)
{
	switch (addr) {
	case PDM_STBCR2_OFF:
		if (val & STBCR2_MDCHG)
			return panic("Unsupported power-down mode (0x%.2x)\n", val);
		/*
		 * I guess the DMAC isn't used, so one less thing to worry about.
		 * Nothing needs to be done here but print a notice just in case.
		 */
		if (val & STBCR2_MSTP7)
			notice("Clock supply to DMAC is halted\n");
		else
			panic("Started clock supply to unsupported DMAC\n");
		/*
		 * It seems that all these other clocks also get stopped later. TODO:
		 * maybe don't let these components work while their clock is stopped.
		 */
		notice("Clock supply to UBC is %s\n", val & STBCR2_MSTP8 ? "halted" : "running");
		notice("Clock supply to DAC is %s\n", val & STBCR2_MSTP6 ? "halted" : "running");
		notice("Clock supply to ADC is %s\n", val & STBCR2_MSTP5 ? "halted" : "running");
		notice("Clock supply to SCIF is %s\n", val & STBCR2_MSTP4 ? "halted" : "running");
		notice("Clock supply to IrDA is %s\n", val & STBCR2_MSTP3 ? "halted" : "running");
		return;
	default:
		return panic("Attempted write to unsupported pdm register at 0x%.8x\n", addr);
	}
}

static void intc_write_byte_reg(uint32_t addr, uint16_t val)
{
	uint8_t preserved_bits;

	switch (addr) {
	case INTC_IRR0_OFF:
		/* The IRQ bits can only be reset to zero */
		preserved_bits = val & IRR0_UNSETTABLE_MASK;
		/* The PINT bits can't be written at all (TODO: handle these) */
		preserved_bits |= (IRR0_PINT0R | IRR0_PINT1R);
		val = (val & ~preserved_bits) | (rtc.RCR1 & preserved_bits);
		intc.IRR0 = val;
		return;
	case INTC_IRR1_OFF:
	case INTC_IRR2_OFF:
		/* Read-only registers, but the firmware does try to set them to 0 */
		return;
	default:
		panic("Attempted write to unsupported INTC register at 0x%.8x\n", addr);
		return;
	}
}

/* Was a port interrupt detected on pin @i? */
static bool pint_detected(int i)
{
	enum pin_sense_mode mode;

	if (i < 8 || i > 12) {
		panic("Unsupported PINT line %d\n", i); /* Should be a bug actually */
		return false;
	}

	/* Interrupts may be outright disabled to this port */
	if ((intc.PINTER & (1U << i)) == 0)
		return false;
	mode = low + ((intc.ICR2 >> i) & 0x0001);
	/* TODO: this is all very ugly, rethink it */
	return (bool)(button_state & (BUTTON_EXIT_PUSHED << (12 - i))) == (mode == low);
}

/* Recalculates the state of the PINT interrupts */
static void pint_refresh(void)
{
	bool interrupt;

	interrupt = false;
	interrupt |= pint_detected(12);
	interrupt |= pint_detected(11);
	interrupt |= pint_detected(10);
	interrupt |= pint_detected(9);
	interrupt |= pint_detected(8);
	write_flag_to_byte(&intc.IRR0, IRR0_PINT1R, interrupt);
}

static void intc_write_word_reg(uint32_t addr, uint16_t val)
{
	/*
	 * Some registers have bits that _should_ always be written as zero, but
	 * it seems that there is no exception and they are just read as zero later.
	 * We just mask those away.
	 */
	switch (addr) {
	case INTC_ICR0_OFF:
		intc.ICR0 = val & 0x0100U;
		return;
	case INTC_ICR1_OFF:
		intc.ICR1 = val; /* TODO: "write 1 to these bits is inhibited" */
		return;
	case INTC_ICR2_OFF:
		intc.ICR2 = val;
		pint_refresh();
		return;
	case INTC_PINTER_OFF:
		intc.PINTER = val;
		pint_refresh();
		return;
	case INTC_IPRA_OFF:
		intc.IPRA = val;
		return;
	case INTC_IPRB_OFF:
		intc.IPRB = val;
		return;
	case INTC_IPRC_OFF:
		intc.IPRC = val;
		return;
	case INTC_IPRD_OFF:
		intc.IPRD = val;
		return;
	case INTC_IPRE_OFF:
		intc.IPRE = val;
		return;
	default:
		panic("Attempted write to unsupported INTC register at 0x%.8x\n", addr);
		return;
	}
}

static uint8_t intc_read_byte_reg(uint32_t addr)
{
	switch (addr) {
	case INTC_IRR0_OFF:
		return intc.IRR0;
	case INTC_IRR1_OFF:
		return intc.IRR1;
	case INTC_IRR2_OFF:
		return intc.IRR2;
	default:
		panic("Attempted read of unsupported INTC register at 0x%.8x\n", addr);
		return 0;
	}
}

static uint16_t intc_read_word_reg(uint32_t addr)
{
	switch (addr) {
	case INTC_ICR0_OFF:
		return intc.ICR0;
	case INTC_ICR1_OFF:
		return intc.ICR1;
	case INTC_ICR2_OFF:
		return intc.ICR2;
	case INTC_PINTER_OFF:
		return intc.PINTER;
	case INTC_IPRA_OFF:
		return intc.IPRA;
	case INTC_IPRB_OFF:
		return intc.IPRB;
	case INTC_IPRC_OFF:
		return intc.IPRB;
	case INTC_IPRD_OFF:
		return intc.IPRB;
	case INTC_IPRE_OFF:
		return intc.IPRB;
	default:
		panic("Attempted read of unsupported INTC register at 0x%.8x\n", addr);
		return 0;
	}
}

static void write_rtc_reg(uint32_t addr, uint8_t val)
{
	/* TODO: implement the counters and alarm for real */
	switch (addr) {
	case RTC_R64CNT_OFF:
		panic("Attempted write to R64CNT register\n");
		return;
	case RTC_RSECCNT_OFF:
		rtc.RSECCNT = val & 0x7FU;
		return;
	case RTC_RMINCNT_OFF:
		rtc.RMINCNT = val & 0x7FU;
		return;
	case RTC_RHRCNT_OFF:
		rtc.RHRCNT = val & 0x3FU;
		return;
	case RTC_RWKCNT_OFF:
		rtc.RWKCNT = val & 0x07U;
		return;
	case RTC_RDAYCNT_OFF:
		rtc.RDAYCNT = val & 0x3FU;
		return;
	case RTC_RMONCNT_OFF:
		rtc.RMONCNT = val & 0x1FU;
		return;
	case RTC_RYRCNT_OFF:
		rtc.RYRCNT = val;
		return;
	case RTC_RSECAR_OFF:
		rtc.RSECAR = val;
		return;
	case RTC_RMINAR_OFF:
		rtc.RMINAR = val;
		return;
	case RTC_RHRAR_OFF:
		rtc.RHRAR = val & 0xBFU;
		return;
	case RTC_RWKAR_OFF:
		rtc.RWKAR = val & 0x87U;
		return;
	case RTC_RDAYAR_OFF:
		rtc.RDAYAR = val & 0xBFU;
		return;
	case RTC_RMONAR_OFF:
		rtc.RMONAR = val & 0x9FU;
		return;
	case RTC_RCR1_OFF:
		if (val & RCR1_UNSETTABLE_MASK) {
			uint8_t preserved_bits = val & RCR1_UNSETTABLE_MASK;
			val = (val & ~preserved_bits) | (rtc.RCR1 & preserved_bits);
		}
		rtc.RCR1 = val & RCR1_BIT_MASK;
		return;
	case RTC_RCR2_OFF:
		/*
		 * TODO: I haven't encountered writes to this register yet so, once I
		 * do, I should review all the bits carefully.
		 */
		if (val & (RCR2_ADJ | RCR2_PEF | RCR2_PES)) {
			panic("Unsupported RTC configuration");
			return;
		}
		if (val & RCR2_RESET)
			rtc.R64CNT = 0;
		rtc.RCR2 = val & 0xF9U;	/* The rest always read 0 */
		return;
	default:
		panic("Attempted write of unsupported RTC register at 0x%.8x\n", addr);
		return;
	}
}

static uint8_t read_rtc_reg(uint32_t addr)
{
	switch (addr) {
	case RTC_R64CNT_OFF:
		return rtc.R64CNT;
	case RTC_RSECCNT_OFF:
		return rtc.RSECCNT;
	case RTC_RMINCNT_OFF:
		return rtc.RMINCNT;
	case RTC_RHRCNT_OFF:
		return rtc.RHRCNT;
	case RTC_RWKCNT_OFF:
		return rtc.RWKCNT;
	case RTC_RDAYCNT_OFF:
		return rtc.RDAYCNT;
	case RTC_RMONCNT_OFF:
		return rtc.RMONCNT;
	case RTC_RYRCNT_OFF:
		return rtc.RYRCNT;
	case RTC_RSECAR_OFF:
		return rtc.RSECAR;
	case RTC_RMINAR_OFF:
		return rtc.RMINAR;
	case RTC_RHRAR_OFF:
		return rtc.RHRAR;
	case RTC_RWKAR_OFF:
		return rtc.RWKAR;
	case RTC_RDAYAR_OFF:
		return rtc.RDAYAR;
	case RTC_RMONAR_OFF:
		return rtc.RMONAR;
	case RTC_RCR1_OFF:
		return rtc.RCR1;
	case RTC_RCR2_OFF:
		return rtc.RCR2;
	default:
		panic("Attempted read of unsupported RTC register at 0x%.8x\n", addr);
		return 0;
	}
}

static void write_nonsdmr_bsc_reg(uint32_t addr, uint16_t val)
{
	/*
	 * Some registers have bits that _should_ always be written as zero, but
	 * it seems that there is no exception and they are just read as zero later.
	 * We just mask those away.
	 */
	switch (addr) {
	case BSC_BCR1_OFF:
		bsc.BCR1 = val;
		return;
	case BSC_BCR2_OFF:
		bsc.BCR2 = val & 0x3FF0U;
		return;
	case BSC_WCR1_OFF:
		bsc.WCR1 = val & 0xBFF3U;
		return;
	case BSC_WCR2_OFF:
		bsc.WCR2 = val;
		return;
	case BSC_MCR_OFF:
		bsc.MCR = val;
		return;
	case BSC_PCR_OFF:
		bsc.PCR = val & 0xCFFFU;
		return;
	case BSC_RTCSR_OFF:
		/* Some bits are ignored if they are set to 1 */
		if (val & RTCSR_UNSETTABLE_MASK) {
			uint16_t preserved_bits = val & RTCSR_UNSETTABLE_MASK;
			val = (val & ~preserved_bits) | (bsc.RTCSR & preserved_bits);
		}
		bsc.RTCSR = val & 0x00FFU;
		return;
	case BSC_RTCNT_OFF:
		if ((val & 0xFF00U) != 0xA500U)
			panic("Bad value set on RTCNT!\n");
		bsc.RTCNT = val & 0x00FFU;
		return;
	case BSC_RTCOR_OFF:
		if ((val & 0xFF00U) != 0xA500U)
			panic("Bad value set on RTCOR!\n");
		bsc.RTCOR = val & 0x00FFU;
		return;
	case BSC_RFCR_OFF:
		if ((val & 0xFC00U) != 0xA400U)
			panic("Bad value set on RFCR!\n");
		bsc.RFCR = val & 0x03FFU;
		return;
	case BSC_MCSCR0_OFF:
		bsc.MCSCR[0] = val & 0x007FU;
		notice("Control register for unused pin MCS0 set to 0x%.4x\n", bsc.MCSCR[0]);
		return;
	case BSC_MCSCR1_OFF:
		bsc.MCSCR[1] = val & 0x007FU;
		notice("Control register for unused pin MCS1 set to 0x%.4x\n", bsc.MCSCR[1]);
		return;
	case BSC_MCSCR2_OFF:
		bsc.MCSCR[2] = val & 0x007FU;
		notice("Control register for unused pin MCS2 set to 0x%.4x\n", bsc.MCSCR[2]);
		return;
	case BSC_MCSCR3_OFF:
		bsc.MCSCR[3] = val & 0x007FU;
		notice("Control register for unused pin MCS3 set to 0x%.4x\n", bsc.MCSCR[3]);
		return;
	case BSC_MCSCR4_OFF:
		bsc.MCSCR[4] = val & 0x007FU;
		if (val == (MCSCR_CAP1 | MCSCR_CAP0))
			notice("MCS4 will get asserted on access to physical range 0x00000000:0x02000000\n");
		else
			panic("Wrong MCS4 pin output configuration 0x%.4x\n", val);
		return;
	case BSC_MCSCR5_OFF:
		bsc.MCSCR[5] = val & 0x007FU;
		notice("Control register for unused pin MCS5 set to 0x%.4x\n", bsc.MCSCR[5]);
		return;
	case BSC_MCSCR6_OFF:
		bsc.MCSCR[6] = val & 0x007FU;
		/* This range is for USB commands; no idea what makes it "mask ROM" */
		if (val == (MCSCR_CS20 | MCSCR_A25 | MCSCR_A24 | MCSCR_A23 | MCSCR_A22))
			notice("MCS6 will get asserted on access to physical range 0x0BC00000:0x0C000000\n");
		else
			panic("Wrong MCS6 pin output configuration 0x%.4x\n", val);
		return;
	case BSC_MCSCR7_OFF:
		bsc.MCSCR[7] = val & 0x007FU;
		notice("Control register for unused pin MCS7 set to 0x%.4x\n", bsc.MCSCR[7]);
		return;
	default:
		panic("Attempted write of unsupported BSC register at 0x%.8x\n", addr);
		return;
	}
}

static void write_sdmr_bsc_reg(uint32_t addr, uint8_t val)
{
	/*
	 * These registers are write-only and concern hardware behaviour, so I don't
	 * think I need any detail in the emulation. My firmware always uses this
	 * address, which sets CAS latency to 2, whatever that means.
	 */
	if (addr != 0xFFFFE880) {
		panic("Write to bad address of SDMR: 0x%.8x\n", addr);
		return;
	}
}

static uint16_t read_nonsdmr_bsc_reg(uint32_t addr)
{
	switch (addr) {
	case BSC_BCR1_OFF:
		return bsc.BCR1;
	case BSC_BCR2_OFF:
		return bsc.BCR2;
	case BSC_WCR1_OFF:
		return bsc.WCR1;
	case BSC_WCR2_OFF:
		return bsc.WCR2;
	case BSC_MCR_OFF:
		return bsc.MCR;
	case BSC_PCR_OFF:
		return bsc.PCR;
	case BSC_RTCSR_OFF:
		return bsc.RTCSR;
	case BSC_RTCNT_OFF:
		return bsc.RTCNT;
	case BSC_RTCOR_OFF:
		return bsc.RTCOR;
	case BSC_RFCR_OFF:
		return bsc.RFCR;
	case BSC_MCSCR4_OFF:
		return bsc.MCSCR[4];
	default:
		panic("Attempted read of unsupported BSC register at 0x%.8x\n", addr);
		return 0;
	}
}

static bool is_except_longword_address(uint32_t addr)
{
	switch (addr) {
	case EXCEPT_TRA_OFF:
	case EXCEPT_EXPEVT_OFF:
	case EXCEPT_INTEVT_OFF:
	case EXCEPT_INTEVT2_OFF:
		return true;
	default:
		return false;
	}
}

static uint32_t except_read_longword_reg(uint32_t addr)
{
	switch (addr) {
	case EXCEPT_TRA_OFF:
		return cpu.TRA;
	case EXCEPT_EXPEVT_OFF:
		return cpu.EXPEVT;
	case EXCEPT_INTEVT_OFF:
		return cpu.INTEVT;
	case EXCEPT_INTEVT2_OFF:
		return cpu.INTEVT2;
	default:
		panic("Attempted read of unsupported exception register at 0x%.8x\n", addr);
		return 0;
	}
}

static uint8_t read_byte(uint32_t addr)
{
	DEBUG_PRINT("Reading byte from 0x%.8x\n", addr);
	addr = mock_va_translation(addr);
	addr = mmu_virt_to_phys(addr);
	addr = p1_p2_to_phys(addr);

	switch (addr & 0xFF000000) {
	case FIRMWARE_OFF:
	case BOOTLOADER_OFF:
		return *(uint8_t *)(firmware + (addr & FIRMWARE_MASK));
	case MEMORY_OFF:
	case MEMORY_SHADOW:
		return *(uint8_t *)(memory + (addr & MEMORY_MASK));
	case DISPLAY_OFF:
		if (addr < DISPLAY_FB_OFF || addr >= DISPLAY_FB_OFF + DISPLAY_RAM_SIZE) {
			panic("Unsupported display register 0x%.8x\n", addr);
			return 0;
		}
		return display.fb[addr - DISPLAY_FB_OFF];
	case 0x13000000:
		return xB3A_read_byte_reg(addr);
	case 0x18000000:
	case 0x1A000000:
		if (is_compactflash_cis_byte_address(addr))
			return compactflash_cis_read_byte_reg(addr);
		if (is_cfcard_ata_byte_address(addr))
			return cfcard_ata_read_byte_reg(addr);
		break;
	case 0xFF000000:
	case 0x04000000:
		if (addr >= PFC_REGS_OFF && addr < PFC_REGS_OFF + PFC_REGS_SIZE) {
			panic("Bad width (8) for read from PFC\n");
			return 0;
		}
		if (is_ioports_byte_address(addr))
			return ioports_read_byte_reg(addr);
		if (is_intc_byte_address(addr))
			return intc_read_byte_reg(addr);
		if (is_rtc_address(addr))
			return read_rtc_reg(addr);
		if (is_pdm_address(addr))
			return read_pdm_reg(addr);
		if (is_ubc_longword_address(addr)) {
			panic("Bad width (8) for read from UBC longword\n");
			return 0;
		}
		if (is_tmu_byte_address(addr))
			return read_tmu_byte_reg(addr);
		if (is_tmu_word_address(addr)) {
			panic("Bad width (8) for read from TMU word\n");
			return 0;
		}
		if (is_tmu_longword_address(addr)) {
			panic("Bad width (8) for read from TMU longword\n");
			return 0;
		}
		if (addr >= BSC_REGS_OFF && addr < BSC_REGS_OFF + BSC_REGS_SIZE) {
			panic("Bad width (8) for read from BSC\n");
			return 0;
		}
		if (is_scif_byte_address(addr))
			return read_scif_byte_reg(addr);
		if (is_scif_word_address(addr)) {
			panic("Bad width (8) for read from SCIF word\n");
			return 0;
		}
		if (is_adconv_byte_address(addr))
			return adconv_read_byte_reg(addr);
		if (is_daconv_byte_address(addr))
			return daconv_read_byte_reg(addr);
		break;
	}
	panic("Attempted read of unknown address 0x%.8x\n", addr);
	return 0;
}

static uint16_t read_word(uint32_t addr)
{
	DEBUG_PRINT("Reading word from 0x%.8x\n", addr);

	if (addr & 1) {
		panic("Unaligned word read from 0x%.8x\n", addr);
		return 0;
	}

	addr = mock_va_translation(addr);
	addr = mmu_virt_to_phys(addr);
	addr = p1_p2_to_phys(addr);

	switch (addr & 0xFF000000) {
	case FIRMWARE_OFF:
	case BOOTLOADER_OFF:
		return *(uint16_t *)(firmware + (addr & FIRMWARE_MASK));
	case MEMORY_OFF:
	case MEMORY_SHADOW:
		return *(uint16_t *)(memory + (addr & MEMORY_MASK));
	case DISPLAY_OFF:
		if (addr < DISPLAY_FB_OFF || addr >= DISPLAY_FB_OFF + DISPLAY_RAM_SIZE) {
			panic("Unsupported display register 0x%.8x\n", addr);
			return 0;
		}
		return *(uint16_t *)(display.fb + (addr - DISPLAY_FB_OFF));
	case MBOARD_REGS_OFF:
		if (!is_motherboard_word_address(addr)) {
			panic("Unsupported motherboard register 0x%.8x\n", addr);
			return 0;
		}
		return motherboard_read_word_reg(addr);
	case 0x18000000:
	case 0x1A000000:
		if (is_cfcard_ata_word_address(addr))
			return cfcard_ata_read_word_reg(addr);
		break;
	case 0xFF000000:
	case 0x04000000:
		if (addr >= PFC_REGS_OFF && addr < PFC_REGS_OFF + PFC_REGS_SIZE)
			/* TODO: use a wrapper function too because reads must be aligned */
			return *(uint16_t *)(pfc_regs + (addr - PFC_REGS_OFF));
		if (is_intc_word_address(addr))
			return intc_read_word_reg(addr);
		if (is_rtc_address(addr)) {
			panic("Bad width (16) for read from RTC\n");
			return 0;
		}
		if (is_pdm_address(addr)) {
			panic("Bad width (16) for read from power-down mode register\n");
			return 0;
		}
		if (is_ubc_longword_address(addr)) {
			panic("Bad width (16) for read from UBC longword\n");
			return 0;
		}
		if (is_tmu_byte_address(addr)) {
			panic("Bad width (16) for read from TMU byte\n");
			return 0;
		}
		if (is_tmu_word_address(addr))
			return read_tmu_word_reg(addr);
		if (is_tmu_longword_address(addr)) {
			panic("Bad width (16) for read from TMU longword\n");
			return 0;
		}
		if (addr >= BSC_REGS_OFF && addr < BSC_REGS_OFF + BSC_REGS_SIZE)
			return read_nonsdmr_bsc_reg(addr);
		if (is_scif_byte_address(addr)) {
			panic("Bad width (16) for read from SCIF byte\n");
			return 0;
		}
		if (is_scif_word_address(addr))
			return read_scif_word_reg(addr);
		if (is_cpg_word_address(addr))
			return cpg_read_word_reg(addr);
		break;
	}
	panic("Attempted read of unknown address 0x%.8x\n", addr);
	return 0;
}

static uint32_t read_longword(uint32_t addr)
{
	DEBUG_PRINT("Reading longword from 0x%.8x\n", addr);

	if (addr & 3) {
		panic("Unaligned longword read from 0x%.8x\n", addr);
		return 0;
	}

	addr = mock_va_translation(addr);
	addr = mmu_virt_to_phys(addr);
	addr = p1_p2_to_phys(addr);

	switch (addr & 0xFF000000) {
	case FIRMWARE_OFF:
	case BOOTLOADER_OFF:
		return *(uint32_t *)(firmware + (addr & FIRMWARE_MASK));
	case MEMORY_OFF:
	case MEMORY_SHADOW:
		return *(uint32_t *)(memory + (addr & MEMORY_MASK));
	case DISPLAY_OFF:
		if (addr < DISPLAY_FB_OFF || addr >= DISPLAY_FB_OFF + DISPLAY_RAM_SIZE) {
			panic("Unsupported display register 0x%.8x\n", addr);
			return 0;
		}
		return *(uint32_t *)(display.fb + (addr - DISPLAY_FB_OFF));
	case 0xFF000000:
	case 0x04000000:
		if (addr >= PFC_REGS_OFF && addr < PFC_REGS_OFF + PFC_REGS_SIZE) {
			panic("Bad width (32) for read from PFC\n");
			return 0;
		}
		if (is_rtc_address(addr)) {
			panic("Bad width (32) for read from RTC\n");
			return 0;
		}
		if (is_pdm_address(addr)) {
			panic("Bad width (32) for read from power-down mode register\n");
			return 0;
		}
		if (is_ubc_longword_address(addr))
			return read_ubc_longword_reg(addr);
		if (is_tmu_byte_address(addr)) {
			panic("Bad width (32) for read from TMU byte\n");
			return 0;
		}
		if (is_tmu_word_address(addr)) {
			panic("Bad width (32) for read from TMU word\n");
			return 0;
		}
		if (is_tmu_longword_address(addr))
			return read_tmu_longword_reg(addr);
		if (addr >= BSC_REGS_OFF && addr < BSC_REGS_OFF + BSC_REGS_SIZE) {
			/*
			 * I've actually seen this in the firmware during boot so I have no
			 * choice but to allow it. TODO: test this on the Jornada.
			 */
			notice("Bad width (32) for read from BSC at address 0x%.8x\n", addr);
			return 0;
		}
		if (is_except_longword_address(addr))
			return except_read_longword_reg(addr);
		if (is_dmac_longword_address(addr))
			return dmac_read_longword_reg(addr);
		if (is_mmu_longword_address(addr))
			return mmu_read_longword_reg(addr);
		if (is_cache_longword_address(addr))
			return cache_read_longword_reg(addr);
	}
	panic("Attempted read of unknown address 0x%.8x\n", addr);
	return 0;
}

static void write_byte(uint32_t addr, uint8_t val)
{
	DEBUG_PRINT("Writing byte 0x%.2x to 0x%.8x\n", val, addr);
	addr = mock_va_translation(addr);
	addr = mmu_virt_to_phys(addr);
	addr = p1_p2_to_phys(addr);

	switch (addr & 0xFF000000) {
	case FIRMWARE_OFF:
	case BOOTLOADER_OFF:
		panic("Attempted write to firmware address! (0x%.8x)\n", addr);
		return;
	case MEMORY_OFF:
	case MEMORY_SHADOW:
#if 0
		if (addr < 0x8C008000)
			printf("Too low memory address 0x%.8x\n", addr);
#endif
		*(uint8_t *)(memory + (addr & MEMORY_MASK)) = val;
		return;
	case DISPLAY_OFF:
		if (addr >= DISPLAY_FB_OFF + DISPLAY_RAM_SIZE) {
			panic("Unsupported display register 0x%.8x\n", addr);
			return;
		}
		/*
		 * I've encountered situations where the framebuffer gets zeroed, and
		 * before and after there are multiple writes to 0xB4000024 and
		 * 0xB4000026. Eventually I would like to know what these registers do
		 * exactly (TODO).
		 */
		if (addr < DISPLAY_FB_OFF) {
			notice("Writing 0x%.2x to unknown display register 0x%.8x (PC: 0x%.8x)\n", val, addr, cpu.PC);
			return;
		}
		display.fb[addr - DISPLAY_FB_OFF] = val;
		return;
	case 0x13000000:
		return xB3A_write_byte_reg(addr, val);
	case 0x18000000:
	case 0x1A000000:
		if (is_cfcard_ata_byte_address(addr))
			return cfcard_ata_write_byte_reg(addr, val);
		if (is_cfcard_config_byte_address(addr))
			return cfcard_config_write_byte_reg(addr, val);
		break;
	case 0xFF000000:
	case 0x04000000:
		if (is_ioports_byte_address(addr))
			return ioports_write_byte_reg(addr, val);
		if (is_intc_byte_address(addr))
			return intc_write_byte_reg(addr, val);
		if (is_rtc_address(addr))
			return write_rtc_reg(addr, val);
		if (addr >= BSC_REGS_OFF && addr < BSC_REGS_OFF + BSC_REGS_SIZE) {
			panic("Bad width (8) for write to BSC\n");
			return;
#if 0
			printf("writing byte 0x%x to addr 0x%x\n", val, addr);
			*(uint8_t *)(bsc_regs + (addr - BSC_REGS_OFF)) = val;
			return;
#endif
		}
		if (addr >= BSC_SDMR_OFF && addr < BSC_SDMR_OFF + BSC_SDMR_SIZE)
			return write_sdmr_bsc_reg(addr, val);
		if (addr >= WDT_REGS_OFF && addr < WDT_REGS_END) {
			panic("Bad width (8) for write to WDT\n");
			return;
		}
		if (is_tmu_byte_address(addr))
			return tmu_write_byte_reg(addr, val);
		if (addr == STBCR_OFF) {
			stbcr_reg = val;
			return;
		}
		if (addr >= PFC_REGS_OFF && addr < PFC_REGS_OFF + PFC_REGS_SIZE) {
			panic("Bad width (8) for write to PFC\n");
			return;
		}
		if (is_scif_byte_address(addr))
			return write_scif_byte_reg(addr, val);
		if (is_scif_word_address(addr)) {
			panic("Bad width (8) for write to SCIF word\n");
			return;
		}
		if (is_adconv_byte_address(addr))
			return adconv_write_byte_reg(addr, val);
		if (is_daconv_byte_address(addr))
			return daconv_write_byte_reg(addr, val);
		if (is_pdm_address(addr))
			return pdm_write_byte_reg(addr, val);
		break;
	}
	panic("Attempted write to unknown address 0x%.8x (value: 0x%.2x)\n", addr, val);
}

static void write_word(uint32_t addr, uint16_t val)
{
	DEBUG_PRINT("Writing word 0x%.4x to 0x%.8x\n", val, addr);

	if (addr & 1)
		return panic("Unaligned word write to 0x%.8x\n", addr);

	addr = mock_va_translation(addr);
	addr = mmu_virt_to_phys(addr);
	addr = p1_p2_to_phys(addr);

	switch (addr & 0xFF000000) {
	case FIRMWARE_OFF:
	case BOOTLOADER_OFF:
		panic("Attempted write to firmware address! (0x%.8x)\n", addr);
		return;
	case MEMORY_OFF:
	case MEMORY_SHADOW:
#if 0
		if (addr < 0x8C008000)
			printf("Too low memory address 0x%.8x\n", addr);
#endif
		*(uint16_t *)(memory + (addr & MEMORY_MASK)) = val;
		return;
	case DISPLAY_OFF:
		if (addr < DISPLAY_FB_OFF || addr >= DISPLAY_FB_OFF + DISPLAY_RAM_SIZE) {
			panic("Unsupported display register 0x%.8x\n", addr);
			return;
		}
		*(uint16_t *)(display.fb + (addr - DISPLAY_FB_OFF)) = val;
		return;
	case MBOARD_REGS_OFF:
		if (!is_motherboard_word_address(addr)) {
			panic("Unsupported motherboard register 0x%.8x\n", addr);
			return;
		}
		return motherboard_write_word_reg(addr, val);
	case 0x18000000:
	case 0x1A000000:
		if (is_cfcard_ata_word_address(addr))
			return cfcard_ata_write_word_reg(addr, val);
		break;
	case 0x04000000:
	case 0xFF000000:
		if (is_intc_word_address(addr))
			return intc_write_word_reg(addr, val);
		if (is_intc_byte_address(addr)) {
			/*
			 * The firmware does this at 0x8003F0B6 and 0x8003F0B8, which also
			 * happen to be read-only registers. I don't yet know the proper
			 * way to handle wrong-width accesses (TODO), but obviously these
			 * are just supposed to be ignored, so let's do that.
			 */
			return intc_write_byte_reg(addr, val);
		}
		if (is_rtc_address(addr)) {
			panic("Bad width (16) for write to RTC\n");
			return;
		}
		if (addr >= BSC_REGS_OFF && addr < BSC_REGS_OFF + BSC_REGS_SIZE)
			return write_nonsdmr_bsc_reg(addr, val);
		if (addr >= BSC_SDMR_OFF && addr < BSC_SDMR_OFF + BSC_SDMR_SIZE) {
			panic("Bad width (16) for write to SDMR\n");
			return;
		}
		if (addr >= WDT_REGS_OFF && addr < WDT_REGS_END) {
			/* TODO: check that the upper byte is as expected for addr */
			if ((val & 0xFF00) != 0x5A00 && (val & 0xFF00) != 0xA500)
				panic("Bad upper byte for write to WDT\n");
			*(uint16_t *)(wdt_regs + (addr - WDT_REGS_OFF)) = val;
			return;
		}
		if (addr >= PFC_REGS_OFF && addr < PFC_REGS_OFF + PFC_REGS_SIZE) {
			return pfc_write_word_reg(addr, val);
		}
		if (is_tmu_word_address(addr))
			return tmu_write_word_reg(addr, val);
		if (is_scif_byte_address(addr)) {
			panic("Bad width (16) for write to SCIF byte\n");
			return;
		}
		if (is_scif_word_address(addr))
			return write_scif_word_reg(addr, val);
		if (is_dmac_word_address(addr))
			return dmac_write_word_reg(addr, val);
		if (is_cpg_word_address(addr))
			return cpg_write_word_reg(addr, val);
		break;
	}
	panic("Attempted write to unknown address 0x%.8x (value: 0x%.4x)\n", addr, val);
}

static void write_longword(uint32_t addr, uint32_t val)
{
	DEBUG_PRINT("Writing longword 0x%.8x to 0x%.8x\n", val, addr);

	if (addr & 3)
		return panic("Unaligned longword write to 0x%.8x\n", addr);

	addr = mock_va_translation(addr);
	addr = mmu_virt_to_phys(addr);
	addr = p1_p2_to_phys(addr);

	switch (addr & 0xFF000000) {
	case FIRMWARE_OFF:
	case BOOTLOADER_OFF:
		panic("Attempted write to firmware address! (0x%.8x)\n", addr);
		return;
	case MEMORY_OFF:
	case MEMORY_SHADOW:
#if 0
		/* TODO: 0x8C008000 checks? Got this from ulRAMStart from romtools */
		if (addr < 0x8C008000)
			printf("Too low memory address 0x%.8x\n", addr);
#endif
		*(uint32_t *)(memory + (addr & MEMORY_MASK)) = val;
		return;
	case DISPLAY_OFF:
		if (addr < DISPLAY_FB_OFF || addr >= DISPLAY_FB_OFF + DISPLAY_RAM_SIZE) {
			panic("Unsupported display register 0x%.8x\n", addr);
			return;
		}
		*(uint32_t *)(display.fb + (addr - DISPLAY_FB_OFF)) = val;
		return;
	case 0xFF000000:
	case 0x04000000:
		if (is_rtc_address(addr)) {
			panic("Bad width (32) for write to RTC\n");
			return;
		}
		if (addr >= BSC_REGS_OFF && addr < BSC_REGS_OFF + BSC_REGS_SIZE) {
			panic("Bad width (32) for write to BSC\n");
			return;
#if 0
			printf("writing longword 0x%x to addr 0x%x\n", val, addr);
			*(uint32_t *)(bsc_regs + (addr - BSC_REGS_OFF)) = val;
			return;
#endif
		}
		if (addr >= BSC_SDMR_OFF && addr < BSC_SDMR_OFF + BSC_SDMR_SIZE) {
			panic("Bad width (32) for write to SDMR\n");
			return;
		}
		if (addr >= WDT_REGS_OFF && addr < WDT_REGS_END) {
			panic("Bad width (32) for write to WDT\n");
			return;
		}
		if (addr >= PFC_REGS_OFF && addr < PFC_REGS_OFF + PFC_REGS_SIZE) {
			panic("Bad width (32) for write to PFC\n");
			return;
		}
		if (is_tmu_longword_address(addr))
			return tmu_write_longword_reg(addr, val);
		if (is_dmac_longword_address(addr))
			return dmac_write_longword_reg(addr, val);
		if (is_cache_longword_address(addr))
			return cache_write_longword_reg(addr, val);
		if (is_mmu_longword_address(addr))
			return mmu_write_longword_reg(addr, val);
		break;
	}
	panic("Attempted write to unknown address 0x%.8x (value: 0x%.8x)\n", addr, val);
}

static void print_longword_reg(const char *name, uint32_t val)
{
	printf("%s:%.8x  ", name, val);
}


static void print_word_reg(const char *name, uint16_t val)
{
	printf("%s:%.4x  ", name, val);
}

static void print_byte_reg(const char *name, uint8_t val)
{
	printf("%s:%.2x  ", name, val);
}

static void print_tmu_byte_reg(const char *name, uint8_t val)
{
	printf("%s:%.2x  ", name, val);
}

static void dump_scif(void)
{
	printf("SCIF registers:\n");

	print_byte_reg("SCSMR2", scif.SCSMR2);
	print_byte_reg("SCBRR2", scif.SCBRR2);
	print_byte_reg("SCSCR2", scif.SCSCR2);
	if (scif.SCFTDR2_count == 0)
		printf("SCSMR2:**  ");
	else
		print_byte_reg("SCSMR2", scif.SCFTDR2[scif.SCFTDR2_count - 1]);
	printf("\n");

	print_word_reg("SCSSR2", scif.SCSSR2);
	if (scif.SCFRDR2_count == 0)
		printf("SCFRDR2:**  ");
	else
		print_byte_reg("SCFRDR2", scif.SCFRDR2[scif.SCFRDR2_count - 1]);
	print_byte_reg("SCFCR2", scif.SCFCR2);
	print_word_reg("SCFDR2", (scif.SCFTDR2_count << 8) & scif.SCFRDR2_count);
	printf("\n");
}

static void dump_tmu(void)
{
	printf("TMU registers:\n");
	print_tmu_byte_reg("TOCR", tmu.TOCR);
	print_tmu_byte_reg("TSTR", tmu.TSTR);
	print_word_reg("TCR0", tmu.TCR[0]);
	print_word_reg("TCR1", tmu.TCR[1]);
	print_word_reg("TCR2", tmu.TCR[2]);
	printf("\n");
	print_longword_reg("TCOR0", tmu.TCOR[0]);
	print_longword_reg("TCOR1", tmu.TCOR[1]);
	print_longword_reg("TCOR2", tmu.TCOR[2]);
	printf("\n");
	print_longword_reg("TCNT0", tmu.TCNT[0]);
	print_longword_reg("TCNT1", tmu.TCNT[1]);
	print_longword_reg("TCNT2", tmu.TCNT[2]);
	print_longword_reg("TCPR2", tmu.TCPR2);
	printf("\n");
}

static void print_ubc_longword_reg(const char *name, uint8_t val)
{
	printf("%s:%.8x  ", name, val);
}

static void dump_ubc(void)
{
	printf("UBC registers:\n");
	print_ubc_longword_reg("BARA", ubc.BARA);
	print_ubc_longword_reg("BAMRA", ubc.BAMRA);
	print_ubc_longword_reg("BARB", ubc.BARB);
	print_ubc_longword_reg("BAMRB", ubc.BAMRB);
	print_ubc_longword_reg("BDRB", ubc.BDRB);
	printf("\n");
	print_ubc_longword_reg("BDMRB", ubc.BDMRB);
	print_ubc_longword_reg("BRCR", ubc.BRCR);
	print_ubc_longword_reg("BRSR", ubc.BRSR);
	print_ubc_longword_reg("BRDR", ubc.BRDR);
	printf("\n");
}

static void print_pdm_reg(const char *name, uint8_t val)
{
	printf("%s:%.2x  ", name, val);
}

static void dump_pdm(void)
{
	printf("Power-down mode control registers:\n");
	print_pdm_reg("STBCR", pdm.STBCR);
	print_pdm_reg("STBCR2", pdm.STBCR2);
	printf("\n");
}

static void print_rtc_reg(const char *name, uint8_t val)
{
	printf("%s:%.2x  ", name, val);
}

static void dump_rtc(void)
{
	printf("RTC registers:\n");
	print_rtc_reg("R64CNT", rtc.R64CNT);
	print_rtc_reg("RSECCNT", rtc.RSECCNT);
	print_rtc_reg("RMINCNT", rtc.RMINCNT);
	print_rtc_reg("RHRCNT", rtc.RHRCNT);
	print_rtc_reg("RWKCNT", rtc.RWKCNT);
	print_rtc_reg("RDAYCNT", rtc.RDAYCNT);
	printf("\n");
	print_rtc_reg("RMONCNT", rtc.RMONCNT);
	print_rtc_reg("RYRCNT", rtc.RYRCNT);
	print_rtc_reg("RSECAR", rtc.RSECAR);
	print_rtc_reg("RMINAR", rtc.RMINAR);
	print_rtc_reg("RHRAR", rtc.RHRAR);
	print_rtc_reg("RWKAR", rtc.RWKAR);
	printf("\n");
	print_rtc_reg("RDAYAR", rtc.RDAYAR);
	print_rtc_reg("RMONAR", rtc.RMONAR);
	print_rtc_reg("RCR1", rtc.RCR1);
	print_rtc_reg("RCR2", rtc.RCR2);
	printf("\n");
}

static void print_intc_reg(const char *name, uint16_t val)
{
	printf("%s:%.4x  ", name, val);
}

static void dump_intc(void)
{
	printf("INTC registers:\n");
	print_intc_reg("ICR0", intc.ICR0);
	print_intc_reg("ICR1", intc.ICR1);
	print_intc_reg("ICR2", intc.ICR2);
	print_intc_reg("PINTER", intc.PINTER);
	printf("\n");
	print_intc_reg("IPRA", intc.IPRA);
	print_intc_reg("IPRB", intc.IPRB);
	print_intc_reg("IPRC", intc.IPRC);
	print_intc_reg("IPRD", intc.IPRD);
	print_intc_reg("IPRE", intc.IPRE);
	printf("\n");
}

static void print_bsc_reg(const char *name, uint16_t val)
{
	printf("%s:%.4x  ", name, val);
}

static void dump_bsc(void)
{
	printf("BSC registers:\n");
	print_bsc_reg("BCR1", bsc.BCR1);
	print_bsc_reg("BCR2", bsc.BCR2);
	print_bsc_reg("WCR1", bsc.WCR1);
	print_bsc_reg("WCR2", bsc.WCR2);
	printf("\n");
	print_bsc_reg("MCR", bsc.MCR);
	print_bsc_reg("DCR", bsc.DCR);
	print_bsc_reg("PCR", bsc.PCR);
	print_bsc_reg("RTCSR", bsc.RTCSR);
	printf("\n");
	print_bsc_reg("RTCNT", bsc.RTCNT);
	print_bsc_reg("RTCOR", bsc.RTCOR);
	print_bsc_reg("RFCR", bsc.RFCR);
	print_bsc_reg("BCR3", bsc.BCR3);
	printf("\n");
	print_bsc_reg("MCSCR0", bsc.MCSCR[0]);
	print_bsc_reg("MCSCR1", bsc.MCSCR[1]);
	print_bsc_reg("MCSCR2", bsc.MCSCR[2]);
	print_bsc_reg("MCSCR3", bsc.MCSCR[3]);
	printf("\n");
	print_bsc_reg("MCSCR4", bsc.MCSCR[4]);
	print_bsc_reg("MCSCR5", bsc.MCSCR[5]);
	print_bsc_reg("MCSCR6", bsc.MCSCR[6]);
	print_bsc_reg("MCSCR7", bsc.MCSCR[7]);
	printf("\n");
}

static void print_pfc_reg(const char *name, uint32_t off)
{
	printf("%s:%.4x  ", name, *(uint16_t *)(pfc_regs + (off - PFC_REGS_OFF)));
}

static void dump_pfc(void)
{
	printf("PFC registers:\n");
	print_pfc_reg("PACR", PFC_PACR_OFF);
	print_pfc_reg("PBCR", PFC_PBCR_OFF);
	print_pfc_reg("PCCR", PFC_PCCR_OFF);
	print_pfc_reg("PDCR", PFC_PDCR_OFF);
	printf("\n");
	print_pfc_reg("PECR", PFC_PECR_OFF);
	print_pfc_reg("PFCR", PFC_PFCR_OFF);
	print_pfc_reg("PGCR", PFC_PGCR_OFF);
	print_pfc_reg("PHCR", PFC_PHCR_OFF);
	printf("\n");
	print_pfc_reg("PJCR", PFC_PJCR_OFF);
	print_pfc_reg("PKCR", PFC_PKCR_OFF);
	print_pfc_reg("PLCR", PFC_PLCR_OFF);
	print_pfc_reg("SCPCR", PFC_SCPCR_OFF);
	printf("\n");
}

static void print_wdt_reg(const char *name, uint32_t off)
{
	/* Only 8-bit reads are sane; the top byte isn't really set on writes */
	printf("%s:%.2x  ", name, *(uint16_t *)(wdt_regs + (off - WDT_REGS_OFF)));
}

static void dump_wdt(void)
{
	printf("WDT registers:\n");
	print_wdt_reg("WTCNT", WDT_WTCNT_OFF);
	print_wdt_reg("WTCSR", WDT_WTCSR_OFF);
	printf("\n");
}

static void dump_stbcr(void)
{
	printf("STBCR registers:\n");
	printf("%s:%.1x  ", "STBCR", stbcr_reg);
	printf("%s:%.1x  ", "STBCR2", stbcr_2_reg);
	printf("\n");
}

static void dump_cpu(void)
{
	int i;

	for (i = 0; i < 4; ++i)
		printf("R%d:%.8x  ", i, cpu.R[i]);
	printf("(BANK0)\n");
	for (i = 4; i < 8; ++i)
		printf("R%d:%.8x  ", i, cpu.R[i]);
	printf("(BANK0)\n");

	for (i = 0; i < 4; ++i)
		printf("R%d:%.8x  ", i, cpu.R_BANK1[i]);
	printf("(BANK1)\n");
	for (i = 4; i < 8; ++i)
		printf("R%d:%.8x  ", i, cpu.R_BANK1[i]);
	printf("(BANK1)\n");

	for (i = 8; i < 12; ++i)
		printf("R%d:%.8x  ", i, cpu.R[i]);
	printf("\n");
	for (i = 12; i < 16; ++i)
		printf("R%d:%.8x  ", i, cpu.R[i]);
	printf("\n");

	printf("GBR:%.8x  SR:%.8x  SSR:%.8x  SPC:%.8x  VBR:%.8x\n", cpu.GBR, cpu.SR, cpu.SSR, cpu.SPC, cpu.VBR);

	printf("MACH:%.8x  MACL:%.8x  PR:%.8x  PC:%.8x\n", cpu.MACH, cpu.MACL, cpu.PR, cpu.PC);

	printf("TRA:%.8x  EXPEVT:%.8x  INTEVT:%.8x  INTEVT2:%.8x\n", cpu.TRA, cpu.EXPEVT, cpu.INTEVT, cpu.INTEVT2);
}

static void dump_micro(void)
{
	int i;

	dump_cpu();

	printf("CODE:");
	for (i = 0; i < 12; ++i)
		printf(" %.4x", read_word((cpu.PC - 4) + (i << 1)));
	printf("\n");

	dump_intc();
	dump_bsc();
	dump_pfc();
	dump_wdt();
	dump_stbcr();
	dump_rtc();
	dump_pdm();
	dump_ubc();
	dump_tmu();
	dump_scif();
}

static void init_scif(void)
{
	scif.SCSMR2 = 0x00;
	scif.SCBRR2 = 0xFF;
	scif.SCSCR2 = 0x00;
	scif.SCFTDR2_count = 0;
	scif.SCSSR2 = 0x0060;
	scif.SCSSR2_unread = 0x60;
	scif.SCFRDR2_count = 0;
	scif.SCFCR2 = 0x00;
}

static void init_tmu(void)
{
	tmu.TCOR[0] = tmu.TCOR[1] = tmu.TCOR[2] = 0xFFFFFFFF;
	tmu.TCNT[0] = tmu.TCNT[1] = tmu.TCNT[2] = 0xFFFFFFFF;
}

static void init_ubc(void)
{
}

static void init_pdm(void)
{
}

static void init_rtc(void)
{
	rtc.RCR2 = 0x09;
}

static void init_intc(void)
{
	intc.ICR1 = 0x4000; /* The docs also claim 0x0000 somewhere else... */
}

static void init_bsc(void)
{
	bsc.BCR2 = 0x3FF0;
	bsc.WCR1 = 0x3FF3;
	bsc.WCR2 = 0xFFFF;
}

/* TODO: make sure that all initial configurations actually work */
static void init_pfc(void)
{
	*(uint16_t *)(pfc_regs + (PFC_PCCR_OFF - PFC_REGS_OFF)) = 0xAAAA;
	*(uint16_t *)(pfc_regs + (PFC_PDCR_OFF - PFC_REGS_OFF)) = 0xAA8A;

	/* TODO: implement low ASEMD0 pin? */
	*(uint16_t *)(pfc_regs + (PFC_PECR_OFF - PFC_REGS_OFF)) = 0xAAAA; /* TODO: sda should be high? */
	*(uint16_t *)(pfc_regs + (PFC_PFCR_OFF - PFC_REGS_OFF)) = 0xAAAA;
	*(uint16_t *)(pfc_regs + (PFC_PGCR_OFF - PFC_REGS_OFF)) = 0xAAAA;
	*(uint16_t *)(pfc_regs + (PFC_PHCR_OFF - PFC_REGS_OFF)) = 0xAAAA;

	*(uint16_t *)(pfc_regs + (PFC_SCPCR_OFF - PFC_REGS_OFF)) = 0xA888;
}

static void init_wdt(void)
{
}

static void init_stbcr(void)
{
}

static void init_ioports(void)
{
}

static void init_eeprom(void)
{
	unsigned int i;

	/*
	 * This is all just for testing. I don't know the actual contents yet, but
	 * page zero should be at 0xac002800 and page 2 at 0xac002840 in physical
	 * hardware (TODO).
	 */
	for (i = 0; i < EEPROM_SIZE - 4; ++i)
		eeprom.mem[i] = 0xff - i;
	/*
	 * The last word of the os configuration must be like this or else the
	 * firmware configuration doesn't get read. I don't know much about this
	 * yet.
	 */
	eeprom.mem[EEPROM_SIZE - 4] = 0xA0;
	eeprom.mem[EEPROM_SIZE - 3] = 0xA5;
	/* The last two bytes are a checksum. See <0x800300A8> fo details. */
	eeprom.mem[EEPROM_SIZE - 2] = 0x4b;
	eeprom.mem[EEPROM_SIZE - 1] = 0xbb;

	eeprom.fc = 0xA2;
	eeprom.fd = 0x7C;

	eeprom.state = EEPROM_STOPPED;
}

static void init_motherboard(void)
{
	if (!card_file)
		motherboard.status |= MBOARD_CARD_SLOT_EMPTY;
}

static void reset(void)
{
	/* Some registers should be undefined, does that matter? (TODO) */

	cpu.SR |= SR_MD_BIT;
	cpu.SR |= SR_RB_BIT;
	cpu.SR |= SR_BL_BIT;
	cpu.SR |= SR_I_BITS;
	cpu.VBR = 0x00000000;
	cpu.EXPEVT = 0x00000000;

	/* Due to the pipeline, the instruction on execution is always at PC-4 */
	cpu.PC = BOOTLOADER_OFF + 4;
	backtrace_push(0, cpu.PC, false /* interrupt */);

	init_intc();
	init_bsc();
	init_pfc();
	init_wdt();
	init_stbcr();
	init_rtc();
	init_pdm();
	init_ubc();
	init_tmu();
	init_scif();
	init_ioports();
	init_eeprom();
	init_motherboard();
}

static int execute(uint32_t pc);

/*
 * We name the instructions here using the pseudocode function names from the
 * manual. TODO: change all the older names with this new scheme.
 */
#define INSN_N_STC_SR_RN			0x0002
#define INSN_N_STCVBR				0x0022
#define INSN_N_PREF					0x0083
#define INSN_NM_MOVBS0				0x0004
#define INSN_NM_MOVWS0				0x0005
#define INSN_NM_MOVLS0				0x0006
#define INSN_NM_MULL				0x0007
#define INSN_0_CLRT					0x0008
#define INSN_0_NOP					0x0009
#define INSN_N_STSMACH				0x000A
#define INSN_NM_MOVWL0				0x000D
#define INSN_NM_MOVLL0				0x000E
#define INSN_0_DIV0U				0x0019
#define INSN_0_SLEEP				0x001B
#define INSN_N_STSMACL				0x001A
#define INSN_N_MOVT					0x0029
#define INSN_0_RTS					0x000B
#define INSN_0_RTE					0x002B
#define INSN_0_LDTLB				0x0038
#define INSN_NM_MOVBL0				0x000C
#define INSN_M_BRAF_RM				0x0023
#define INSN_MOVL_RM_TO_AT_DISP_RN	0x1000
#define INSN_NM_MOVB_RM_ATRN		0x2000
#define INSN_NM_MOVW_ATRN_RM		0x2001 /* TODO: fix older names like this */
#define INSN_NM_MOVL_RM_ATRN		0x2002
#define INSN_NM_MOVL_RM_AT_MINUS_RN	0x2006
#define INSN_NM_DIV0S				0x2007
#define INSN_NM_TST_RM_RN			0x2008
#define INSN_NM_AND_RN_RM			0x2009
#define INSN_NM_XOR					0x200A
#define INSN_NM_OR_RM_RN			0x200B
#define INSN_NM_CMPSTR				0x200C
#define INSN_NM_XTRCT				0x200D
#define INSN_NM_CMPEQ_RM_RN			0x3000
#define INSN_NM_CMPHS				0x3002
#define INSN_NM_CMPGE				0x3003
#define INSN_NM_DIV1				0x3004
#define INSN_NM_DMULU				0x3005
#define INSN_NM_CMPHI				0x3006
#define INSN_NM_CMPGT_RM_RN			0x3007
#define INSN_NM_SUB					0x3008
#define INSN_NM_SUBC				0x300A
#define INSN_NM_ADD					0x300C
#define INSN_NM_ADDC				0x300E
#define INSN_N_SHLL					0x4000
#define INSN_N_SHLR					0x4001
#define INSN_N_SHLL2				0x4008
#define INSN_N_SHLL8				0x4018
#define INSN_N_SHLR2				0x4009
#define INSN_N_SHLR8				0x4019
#define INSN_M_LDS_RM_MACH			0x400A
#define INSN_M_JSR_AT_RM			0x400B
#define INSN_NM_SHAD				0x400C
#define INSN_NM_SHLD				0x400D
#define INSN_N_DT_RN				0x4010
#define INSN_N_CMPPZ				0x4011
#define INSN_N_CMPPL				0x4015
#define INSN_M_LDS_RM_MACL			0x401A
#define INSN_M_LDC_RM_GBR			0x401E
#define INSN_M_LDCVBR				0x402E
#define INSN_N_STSMMACH				0x4002
#define INSN_N_STSMMACL				0x4012
#define INSN_N_STCMSR				0x4003
#define INSN_N_STCMGBR				0x4013
#define INSN_N_STCMVBR				0x4023
#define INSN_N_STCMSSR				0x4033
#define INSN_N_STCMSPC				0x4043
#define INSN_M_LDCSR				0x400E
#define INSN_N_ROTR					0x4005
#define INSN_M_LDSMMACH				0x4006
#define INSN_M_LDCMSR				0x4007
#define INSN_M_LDSMMACL				0x4016
#define INSN_M_LDCMGBR				0x4017
#define INSN_N_SHAR					0x4021
#define INSN_N_STSL_PR_AT_MINUS_RN	0x4022
#define INSN_N_ROTCL				0x4024
#define INSN_N_ROTCR				0x4025
#define INSN_M_LDSMPR				0x4026
#define INSN_N_SHLL16				0x4028
#define INSN_N_SHLR16				0x4029
#define INSN_M_JMP					0x402B
#define INSN_M_LDCMVBR				0x4027
#define INSN_M_LDCMSSR				0x4037
#define INSN_M_LDCMSPC				0x4047
#define INSN_MOVL_AT_DISP_RM_TO_RN	0x5000
#define INSN_NM_MOVB_ATRM_RN		0x6000
#define INSN_NM_MOVW_RN_ATRM		0x6001
#define INSN_NM_MOVL_RN_ATRM		0x6002
#define INSN_NM_MOV_RN_RM			0x6003
#define INSN_NM_MOVBP				0x6004
#define INSN_NM_MOVLP				0x6006
#define INSN_NM_NOT					0x6007
#define INSN_NM_NEGC				0x600A
#define INSN_NM_NEG					0x600B
#define INSN_NM_EXTUB_RM_RN			0x600C
#define INSN_NM_EXTUW_RM_RN			0x600D
#define INSN_NM_EXTSB_RM_RN			0x600E
#define INSN_NM_EXTSW				0x600F
#define INSN_ADD_I8_RN				0x7000
#define INSN_ND4_MOVBS4				0x8000
#define INSN_ND4_MOVW				0x8100
#define INSN_ND4_MOVBL4				0x8400
#define INSN_ND4_MOVWL4				0x8500
#define INSN_I_CMPEQ				0x8800
#define INSN_BT						0x8900
#define INSN_BF						0x8B00
#define INSN_BTS					0x8D00
#define INSN_D_BFS					0x8F00
#define INSN_ND8_MOV_W				0x9000
#define INSN_BRA					0xA000
#define INSN_MOVB_R0_TO_AT_DISP_GBR	0xC000
#define INSN_MOVW_R0_TO_AT_DISP_GBR	0xC100
#define INSN_D_MOVLSG				0xC200
#define INSN_I_TRAPA				0xC300
#define INSN_I_MOVBLG				0xC400
#define INSN_I_MOVWLG				0xC500
#define INSN_D_MOVA					0xC700
#define INSN_TSTI					0xC800
#define INSN_AND_I8_R0				0xC900
#define INSN_I_XORI					0xCA00
#define INSN_I_ORI					0xCB00
#define INSN_ND8_MOV_L				0xD000
#define INSN_MOV_I8_RN				0xE000

/* sh-3 has delayed branch instructions */
/* TODO: make the target an argument too, to avoid documenting that in all the callers */
static int execute_delayed_slot(uint32_t pc)
{
	uint32_t target_pc = cpu.PC;
	int ret;

	if (cpu.extra_state & EXTRA_IN_DELAYED) {
		panic("Branch after delayed branch!\n");
		return 1;
	}

	cpu.extra_state |= EXTRA_IN_DELAYED;
	ret = execute(pc + 2);
	cpu.extra_state &= ~EXTRA_IN_DELAYED;

	/* We increased the pc on execution, but the target remains the same */
	cpu.PC = target_pc;
	return ret;
}

/* Instructions of the form 0000 xxxx xxxx xxxx */
static int execute_0_format(uint32_t pc, uint16_t insn)
{
	switch (insn & 0xFFFF) {
	case INSN_0_CLRT:
		cpu.SR &= ~SR_T_BIT;
		cpu.PC += 2;
		return 0;
	case INSN_0_NOP:
		cpu.PC += 2;
		return 0;
	case INSN_0_RTS:
		if (cpu.extra_state & EXTRA_IN_DELAYED)
			panic("Invalid delay slot! (TODO)\n");
		backtrace_pop();
		cpu.PC = cpu.PR + 4;
		return execute_delayed_slot(pc);
	case INSN_0_RTE:
		if (cpu.extra_state & EXTRA_IN_DELAYED)
			panic("Invalid delay slot! (TODO)\n");
		if (!(cpu.SR & SR_MD_BIT))
			panic("Privilege violation! (TODO)\n");
		backtrace_pop();
		cpu.PC = cpu.SPC;
		cpu.SR = cpu.SSR;
		return execute_delayed_slot(pc);
	case INSN_0_DIV0U:
		cpu.SR &= ~(SR_T_BIT | SR_Q_BIT | SR_M_BIT);
		cpu.PC += 2;
		return 0;
	case INSN_0_SLEEP:
		cpu.extra_state |= EXTRA_POWER_DOWN;
		/*
		 * The PC doesn't move forward yet because we want to stay in the sleep
		 * instruction if we get a keyboard interrupt before any actual emulated
		 * interrupts. So this instruction may get executed more than once, but
		 * that shouldn't matter. Strangely, the manual claims that the PC
		 * should be reduced by 2... no idea.
		 */
		return 0;
	case INSN_0_LDTLB:
		mmu_load_pte_to_tlb();
		cpu.PC += 2;
		return 0;
	}

	panic("0 format instruction 0x%x not implemented\n", insn);
	return 1;
}

/* Instructions of the form xxxx nnnn dddd dddd */
static int execute_nd8_format(uint32_t pc, uint16_t insn)
{
	uint8_t d = (insn & 0x00FFU);
	uint8_t n = (insn & 0x0F00U) >> 8;

	switch (insn & 0xF000) {
	case INSN_ND8_MOV_W:
		write_gp_register(n, sign_extend_word(read_word(pc + (d << 1))));
		cpu.PC += 2;
		return 0;
	case INSN_ND8_MOV_L:
		pc &= 0xFFFFFFFC; /* Aligned longword read */
		write_gp_register(n, read_longword(pc + (d << 2)));
		cpu.PC += 2;
		return 0;
	default:
		break;
	}

	panic("ND8 format instruction 0x%x not implemented\n", insn);
	return 1;
}

/* Instructions of the form xxxx nnnn iiii iiii */
static int execute_ni8_format(uint32_t pc, uint16_t insn)
{
	uint8_t i = (insn & 0x00FFU);
	uint8_t n = (insn & 0x0F00U) >> 8;

	switch (insn & 0xF000) {
	case INSN_ADD_I8_RN:
		write_gp_register(n, read_gp_register(n) + sign_extend_byte(i));
		cpu.PC += 2;
		return 0;
	case INSN_MOV_I8_RN:
		write_gp_register(n, sign_extend_byte(i));
		cpu.PC += 2;
		return 0;
	default:
		break;
	}

	panic("NI8 format instruction 0x%x not implemented\n", insn);
	return 1;
}

/* Instructions of the form xxxx nnnn xxxx xxxx */
static int execute_n_format(uint32_t pc, uint16_t insn)
{
	uint8_t n = (insn & 0x0F00U) >> 8;
	uint32_t nval;
	bool tbit;

	switch (insn & 0xF0FF) {
	case INSN_N_STC_SR_RN:
		if (!(cpu.SR & SR_MD_BIT))
			panic("Privilege violation! (TODO)\n");
		write_gp_register(n, cpu.SR);
		cpu.PC += 2;
		return 0;
	case INSN_N_STCVBR:
		if (!(cpu.SR & SR_MD_BIT))
			panic("Privilege violation! (TODO)\n");
		write_gp_register(n, cpu.VBR);
		cpu.PC += 2;
		return 0;
	case INSN_N_PREF:
		/* Prefetch to cache - does nothing in the emulator */
		cpu.PC += 2;
		return 0;
	case INSN_N_STSMACH:
		write_gp_register(n, cpu.MACH);
		cpu.PC += 2;
		return 0;
	case INSN_N_STSMACL:
		write_gp_register(n, cpu.MACL);
		cpu.PC += 2;
		return 0;
	case INSN_N_MOVT:
		write_gp_register(n, cpu.SR & SR_T_BIT ? 1 : 0);
		cpu.PC += 2;
		return 0;
	case INSN_N_SHLL:
		nval = read_gp_register(n);
		if (nval & 0x80000000U)
			cpu.SR |= SR_T_BIT;
		else
			cpu.SR &= ~SR_T_BIT;
		write_gp_register(n, nval << 1);
		cpu.PC += 2;
		return 0;
	case INSN_N_SHLR:
		nval = read_gp_register(n);
		if (nval & 0x00000001U)
			cpu.SR |= SR_T_BIT;
		else
			cpu.SR &= ~SR_T_BIT;
		write_gp_register(n, nval >> 1);
		cpu.PC += 2;
		return 0;
	case INSN_N_STSMMACH:
		nval = read_gp_register(n) - 4;
		write_gp_register(n, nval);
		/*
		 * The manual seems to do sign extension here, but I think MACH will
		 * always be in the correct format? TODO: confirm this.
		 */
		write_longword(nval, cpu.MACH);
		cpu.PC += 2;
		return 0;
	case INSN_N_STSMMACL:
		nval = read_gp_register(n) - 4;
		write_gp_register(n, nval);
		write_longword(nval, cpu.MACL);
		cpu.PC += 2;
		return 0;
	case INSN_N_STCMSR:
		if (!(cpu.SR & SR_MD_BIT))
			panic("Privilege violation! (TODO)\n");
		nval = read_gp_register(n) - 4;
		write_gp_register(n, nval);
		write_longword(nval, cpu.SR);
		cpu.PC += 2;
		return 0;
	case INSN_N_STCMGBR:
		nval = read_gp_register(n) - 4;
		write_gp_register(n, nval);
		write_longword(nval, cpu.GBR);
		cpu.PC += 2;
		return 0;
	case INSN_N_STCMVBR:
		if (!(cpu.SR & SR_MD_BIT))
			panic("Privilege violation! (TODO)\n");
		nval = read_gp_register(n) - 4;
		write_gp_register(n, nval);
		write_longword(nval, cpu.VBR);
		cpu.PC += 2;
		return 0;
	case INSN_N_STCMSSR:
		if (!(cpu.SR & SR_MD_BIT))
			panic("Privilege violation! (TODO)\n");
		nval = read_gp_register(n) - 4;
		write_gp_register(n, nval);
		write_longword(nval, cpu.SSR);
		cpu.PC += 2;
		return 0;
	case INSN_N_STCMSPC:
		if (!(cpu.SR & SR_MD_BIT))
			panic("Privilege violation! (TODO)\n");
		nval = read_gp_register(n) - 4;
		write_gp_register(n, nval);
		write_longword(nval, cpu.SPC);
		cpu.PC += 2;
		return 0;
	case INSN_N_SHLL2:
		nval = read_gp_register(n);
		write_gp_register(n, nval << 2);
		cpu.PC += 2;
		return 0;
	case INSN_N_SHLL8:
		nval = read_gp_register(n);
		write_gp_register(n, nval << 8);
		cpu.PC += 2;
		return 0;
	case INSN_N_SHLR2:
		nval = read_gp_register(n);
		write_gp_register(n, nval >> 2);
		cpu.PC += 2;
		return 0;
	case INSN_N_SHLR8:
		nval = read_gp_register(n);
		write_gp_register(n, nval >> 8);
		cpu.PC += 2;
		return 0;
	case INSN_N_DT_RN:
		nval = read_gp_register(n) - 1;
		write_gp_register(n, nval);
		if (nval == 0)
			cpu.SR |= SR_T_BIT;
		else
			cpu.SR &= ~SR_T_BIT;
		cpu.PC += 2;
		return 0;
	case INSN_N_CMPPZ:
		if ((int32_t)read_gp_register(n) >= 0)
			cpu.SR |= SR_T_BIT;
		else
			cpu.SR &= ~SR_T_BIT;
		cpu.PC += 2;
		return 0;
	case INSN_N_CMPPL:
		if ((int32_t)read_gp_register(n) > 0)
			cpu.SR |= SR_T_BIT;
		else
			cpu.SR &= ~SR_T_BIT;
		cpu.PC += 2;
		return 0;
	case INSN_N_SHAR:
		nval = read_gp_register(n);
		if (nval & 0x00000001U)
			cpu.SR |= SR_T_BIT;
		else
			cpu.SR &= ~SR_T_BIT;
		write_gp_register(n, (int32_t)nval >> 1);
		cpu.PC += 2;
		return 0;
	case INSN_N_STSL_PR_AT_MINUS_RN:
		nval = read_gp_register(n) - 4;
		write_gp_register(n, nval);
		write_longword(nval, cpu.PR);
		cpu.PC += 2;
		return 0;
	case INSN_N_ROTR:
		nval = read_gp_register(n);
		tbit = nval & 0x00000001U;
		write_flag_to_long(&cpu.SR, SR_T_BIT, tbit);
		nval >>= 1;
		write_flag_to_long(&nval, 1U << 31, tbit);
		write_gp_register(n, nval);
		cpu.PC += 2;
		return 0;
	case INSN_N_ROTCL:
		nval = read_gp_register(n);
		tbit = cpu.SR & SR_T_BIT;
		if (nval & 0x80000000U)
			cpu.SR |= SR_T_BIT;
		else
			cpu.SR &= ~SR_T_BIT;
		write_gp_register(n, (nval << 1) | (uint32_t)tbit);
		cpu.PC += 2;
		return 0;
	case INSN_N_ROTCR:
		nval = read_gp_register(n);
		tbit = cpu.SR & SR_T_BIT;
		if (nval & 0x00000001U)
			cpu.SR |= SR_T_BIT;
		else
			cpu.SR &= ~SR_T_BIT;
		write_gp_register(n, (nval >> 1) | ((uint32_t)tbit << 31));
		cpu.PC += 2;
		return 0;
	case INSN_N_SHLL16:
		nval = read_gp_register(n);
		write_gp_register(n, nval << 16);
		cpu.PC += 2;
		return 0;
	case INSN_N_SHLR16:
		nval = read_gp_register(n);
		write_gp_register(n, nval >> 16);
		cpu.PC += 2;
		return 0;
	default:
		break;
	}

	panic("N format instruction 0x%x not implemented\n", insn);
	return 1;
}

/* Instructions of the form xxxx mmmm xxxx xxxx */
static int execute_m_format(uint32_t pc, uint16_t insn)
{
	uint8_t m = (insn & 0x0F00U) >> 8;
	uint32_t mval;
	int32_t target;

	switch (insn & 0xF0FF) {
	case INSN_M_BRAF_RM:
		/* The manual seems to be missing the "+4" here */
		target = cpu.PC + read_gp_register(m) + 4;
		/* Delayed slots use the target for any pc relative addressing */
		cpu.PC = target;
		return execute_delayed_slot(pc);
	case INSN_M_JSR_AT_RM:
		target = read_gp_register(m) + 4;
		backtrace_push(cpu.PC, target, false /* interrupt */);
		cpu.PR = cpu.PC;
		/* Delayed slots use the target for any pc relative addressing */
		cpu.PC = target;
		return execute_delayed_slot(pc);
	case INSN_M_LDS_RM_MACH:
		cpu.MACH = read_gp_register(m);
		/* Sign extension, I guess. It's copied from the reference */
		if ((cpu.MACH & 0x00000200) == 0)
			cpu.MACH &= 0x000003FF;
		else
			cpu.MACH |= 0xFFFFFC00;
		cpu.PC += 2;
		return 0;
	case INSN_M_LDS_RM_MACL:
		cpu.MACL = read_gp_register(m);
		cpu.PC += 2;
		return 0;
	case INSN_M_LDC_RM_GBR:
		cpu.GBR = read_gp_register(m);
		cpu.PC += 2;
		return 0;
	case INSN_M_LDCVBR:
		if (!(cpu.SR & SR_MD_BIT))
			panic("Privilege violation! (TODO)\n");
		cpu.VBR = read_gp_register(m);
		cpu.PC += 2;
		return 0;
	case INSN_M_LDSMPR:
		mval = read_gp_register(m);
		cpu.PR = read_longword(mval);
		write_gp_register(m, mval + 4);
		cpu.PC += 2;
		return 0;
	case INSN_M_LDCSR:
		if (cpu.extra_state & EXTRA_IN_DELAYED)
			panic("Invalid delay slot! (TODO)\n");
		if (!(cpu.SR & SR_MD_BIT))
			panic("Privilege violation! (TODO)\n");
		/*
		 * The pseudocode also wants me to unset a few flags, but that doesn't
		 * make much sense and it quickly leads to trouble with the firmware.
		 */
		cpu.SR = read_gp_register(m) & SR_BIT_MASK;
		cpu.PC += 2;
		return 0;
	case INSN_M_LDSMMACH:
		mval = read_gp_register(m);
		cpu.MACH = read_longword(mval);
		/* TODO: move this to a common function for every time MACH gets set */
		if ((cpu.MACH & 0x00000200) == 0)
			cpu.MACH &= 0x000003FF;
		else
			cpu.MACH |= 0xFFFFFC00;
		write_gp_register(m, mval + 4);
		cpu.PC += 2;
		return 0;
	case INSN_M_LDCMSR:
		if (cpu.extra_state & EXTRA_IN_DELAYED)
			panic("Invalid delay slot! (TODO)\n");
		if (!(cpu.SR & SR_MD_BIT))
			panic("Privilege violation! (TODO)\n");
		mval = read_gp_register(m);
		/* Same as LDCSR, the manual's pseudocode looks wrong to me */
		cpu.SR = read_longword(mval) & SR_BIT_MASK;
		write_gp_register(m, mval + 4);
		cpu.PC += 2;
		return 0;
	case INSN_M_LDSMMACL:
		mval = read_gp_register(m);
		cpu.MACL = read_longword(mval);
		write_gp_register(m, mval + 4);
		cpu.PC += 2;
		return 0;
	case INSN_M_LDCMGBR:
		mval = read_gp_register(m);
		cpu.GBR = read_longword(mval);
		write_gp_register(m, mval + 4);
		cpu.PC += 2;
		return 0;
	case INSN_M_JMP:
		target = read_gp_register(m) + 4;
		/* Delayed slots use the target for any pc relative addressing */
		cpu.PC = target;
		return execute_delayed_slot(pc);
	case INSN_M_LDCMVBR:
		if (!(cpu.SR & SR_MD_BIT))
			panic("Privilege violation! (TODO)\n");
		mval = read_gp_register(m);
		cpu.VBR = read_longword(mval);
		write_gp_register(m, mval + 4);
		cpu.PC += 2;
		return 0;
	case INSN_M_LDCMSSR:
		if (!(cpu.SR & SR_MD_BIT))
			panic("Privilege violation! (TODO)\n");
		mval = read_gp_register(m);
		/* According to the manual's pseudocode, some flags get unset... */
		cpu.SSR = read_longword(mval) & 0x700003F3;
		write_gp_register(m, mval + 4);
		cpu.PC += 2;
		return 0;
	case INSN_M_LDCMSPC:
		if (!(cpu.SR & SR_MD_BIT))
			panic("Privilege violation! (TODO)\n");
		mval = read_gp_register(m);
		cpu.SPC = read_longword(mval);
		write_gp_register(m, mval + 4);
		cpu.PC += 2;
		return 0;
	default:
		break;
	}

	panic("M format instruction 0x%x not implemented\n", insn);
	return 1;
}

static int execute_nm_format(uint32_t pc, uint16_t insn);

/* Instructions of the form xxxx **** **** xxxx */
static int execute_xuux_format(uint32_t pc, uint16_t insn)
{
	switch (insn & 0x000F) {
	case 0x0000:
	case 0x0001:
	case 0x0002:
	case 0x0003:
	case 0x0004:
	case 0x0005:
	case 0x0008:
	case 0x0009:
		return execute_n_format(pc, insn);
	case 0x0006:
	case 0x0007:
	case 0x000A:
	case 0x000B:
	case 0x000E:
		return execute_m_format(pc, insn);
	case 0x000C:
	case 0x000D:
		return execute_nm_format(pc, insn);
	default:
		panic("xuux instruction 0x%x not implemented\n", insn);
		return 1;
	}
}

/* Is there a byte in @longword1 that equals a byte in @longword2? */
static bool have_equal_byte(uint32_t longword1, uint32_t longword2)
{
	uint32_t xor;

	xor = longword1 ^ longword2;
	if ((xor & 0x000000FFU) == 0)
		return true;
	if ((xor & 0x0000FF00U) == 0)
		return true;
	if ((xor & 0x00FF0000U) == 0)
		return true;
	if ((xor & 0xFF000000U) == 0)
		return true;
	return false;
}

/*
 * Copied from the manual's pseudocode and reformatted to our style. TODO:
 * understand what this actually does...
 */
static int execute_div1(uint32_t m, uint32_t n)
{
	uint32_t mval, nval;
	uint32_t tmp0;
	bool old_qbit, new_qbit, tbit, mbit, tmp1;

	mval = read_gp_register(m);
	nval = read_gp_register(n);
	old_qbit = cpu.SR & SR_Q_BIT;
	tbit = cpu.SR & SR_T_BIT;
	mbit = cpu.SR & SR_M_BIT;

	new_qbit = nval & 0x80000000U;
	nval <<= 1;
	nval |= (uint32_t)tbit;

	if (!old_qbit) {
		if (!mbit) {
			tmp0 = nval;
			nval -= mval;
			tmp1 = nval > tmp0;
			if (!new_qbit)
				new_qbit = tmp1;
			else
				new_qbit = !tmp1;
		} else {
			tmp0 = nval;
			nval += mval;
			tmp1 = nval < tmp0;
			if (!new_qbit)
				new_qbit = !tmp1;
			else
				new_qbit = tmp1;
		}
	} else {
		if (!mbit) {
			tmp0 = nval;
			nval += mval;
			tmp1 = nval < tmp0;
			if (!new_qbit)
				new_qbit = tmp1;
			else
				new_qbit = !tmp1;
		} else {
			tmp0 = nval;
			nval -= mval;
			tmp1 = nval > tmp0;
			if (!new_qbit)
				new_qbit = !tmp1;
			else
				new_qbit = tmp1;
		}
	}
	tbit = new_qbit == mbit;

	write_flag_to_long(&cpu.SR, SR_T_BIT, tbit);
	write_flag_to_long(&cpu.SR, SR_Q_BIT, new_qbit);
	write_flag_to_long(&cpu.SR, SR_M_BIT, mbit);
	write_gp_register(m, mval);
	write_gp_register(n, nval);
	cpu.PC += 2;
	return 0;
}

/* Instructions of the form xxxx nnnn mmmm xxxx */
static int execute_nm_format(uint32_t pc, uint16_t insn)
{
	uint8_t n = (insn & 0x0F00U) >> 8;
	uint8_t m = (insn & 0x00F0U) >> 4;
	uint32_t nval, mval, src_addr, shift_cnt;
	bool tbit, qbit, mbit;
	uint64_t tmp64;

	switch (insn & 0xF00F) {
	case INSN_NM_MOVBS0:
		mval = read_gp_register(m);
		nval = read_gp_register(n);
		write_byte(read_gp_register(0) + nval, mval);
		cpu.PC += 2;
		return 0;
	case INSN_NM_MOVWS0:
		mval = read_gp_register(m);
		nval = read_gp_register(n);
		write_word(read_gp_register(0) + nval, mval);
		cpu.PC += 2;
		return 0;
	case INSN_NM_MOVLS0:
		mval = read_gp_register(m);
		nval = read_gp_register(n);
		write_longword(read_gp_register(0) + nval, mval);
		cpu.PC += 2;
		return 0;
	case INSN_NM_MULL:
		cpu.MACL = read_gp_register(n) * read_gp_register(m);
		cpu.PC += 2;
		return 0;
	case INSN_NM_MOVWL0:
		mval = read_gp_register(m);
		nval = sign_extend_word(read_word(read_gp_register(0) + mval));
		write_gp_register(n, nval);
		cpu.PC += 2;
		return 0;
	case INSN_NM_MOVLL0:
		mval = read_gp_register(m);
		write_gp_register(n, read_longword(read_gp_register(0) + mval));
		cpu.PC += 2;
		return 0;
	case INSN_NM_MOVBL0:
		src_addr = read_gp_register(0) + read_gp_register(m);
		write_gp_register(n, sign_extend_byte(read_byte(src_addr)));
		cpu.PC += 2;
		return 0;
	case INSN_NM_OR_RM_RN:
		write_gp_register(n, read_gp_register(n) | read_gp_register(m));
		cpu.PC += 2;
		return 0;
	case INSN_NM_XOR:
		write_gp_register(n, read_gp_register(n) ^ read_gp_register(m));
		cpu.PC += 2;
		return 0;
	case INSN_NM_CMPSTR:
		nval = read_gp_register(n);
		mval = read_gp_register(m);
		if (have_equal_byte(nval, mval))
			cpu.SR |= SR_T_BIT;
		else
			cpu.SR &= ~SR_T_BIT;
		cpu.PC += 2;
		return 0;
	case INSN_NM_XTRCT:
		nval = read_gp_register(n);
		mval = read_gp_register(m);
		write_gp_register(n, (nval >> 16) | (mval << 16));
		cpu.PC += 2;
		return 0;
	case INSN_NM_CMPEQ_RM_RN:
		if (read_gp_register(n) == read_gp_register(m))
			cpu.SR |= SR_T_BIT;
		else
			cpu.SR &= ~SR_T_BIT;
		cpu.PC += 2;
		return 0;
	case INSN_NM_CMPHS:
		if (read_gp_register(n) >= read_gp_register(m))
			cpu.SR |= SR_T_BIT;
		else
			cpu.SR &= ~SR_T_BIT;
		cpu.PC += 2;
		return 0;
	case INSN_NM_CMPGE:
		if ((int32_t)read_gp_register(n) >= (int32_t)read_gp_register(m))
			cpu.SR |= SR_T_BIT;
		else
			cpu.SR &= ~SR_T_BIT;
		cpu.PC += 2;
		return 0;
	case INSN_NM_DIV1:
		return execute_div1(m, n);
	case INSN_NM_DMULU:
		tmp64 = (uint64_t)read_gp_register(n) * (uint64_t)read_gp_register(m);
		cpu.MACL = tmp64;
		cpu.MACH = tmp64 >> 32;
		cpu.PC += 2;
		return 0;
	case INSN_NM_CMPHI:
		/* Things like this assume that the host runs two's complement... */
		if (read_gp_register(n) > read_gp_register(m))
			cpu.SR |= SR_T_BIT;
		else
			cpu.SR &= ~SR_T_BIT;
		cpu.PC += 2;
		return 0;
	case INSN_NM_CMPGT_RM_RN:
		/* Things like this assume that the host runs two's complement... */
		if ((int32_t)read_gp_register(n) > (int32_t)read_gp_register(m))
			cpu.SR |= SR_T_BIT;
		else
			cpu.SR &= ~SR_T_BIT;
		cpu.PC += 2;
		return 0;
	case INSN_NM_SUB:
		write_gp_register(n, read_gp_register(n) - read_gp_register(m));
		cpu.PC += 2;
		return 0;
	case INSN_NM_SUBC:
		nval = read_gp_register(n);
		nval -= read_gp_register(m);
		/*
		 * Careful here: if mval is -1 and the T bit is set, the overflow will
		 * leave nval right where it started. The pseudocode in the manual has
		 * this right, but I didn't get it at first and I almost messed up.
		 * TODO: check similar instructions for mistakes of this kind.
		 */
		if (cpu.SR & SR_T_BIT) {
			nval -= 1;
			write_flag_to_long(&cpu.SR, SR_T_BIT, nval >= read_gp_register(n));
		} else {
			write_flag_to_long(&cpu.SR, SR_T_BIT, nval > read_gp_register(n));
		}
		write_gp_register(n, nval);
		cpu.PC += 2;
		return 0;
	case INSN_NM_ADD:
		write_gp_register(n, read_gp_register(n) + read_gp_register(m));
		cpu.PC += 2;
		return 0;
	case INSN_NM_ADDC:
		nval = read_gp_register(n);
		nval += read_gp_register(m);
		/* See the comment in SUBC */
		if (cpu.SR & SR_T_BIT) {
			nval += 1;
			write_flag_to_long(&cpu.SR, SR_T_BIT, nval <= read_gp_register(n));
		} else {
			write_flag_to_long(&cpu.SR, SR_T_BIT, nval < read_gp_register(n));
		}
		write_gp_register(n, nval);
		cpu.PC += 2;
		return 0;
	case INSN_NM_SHAD:
		mval = read_gp_register(m);
		nval = read_gp_register(n);
		/* Top bit decides the direction, bottom 5 decide the magnitude */
		shift_cnt = mval & 0x0000001FU;
		if (mval & 0x80000000U) {
			/*
			 * TODO: it's not clear to me from the manual what should happen
			 * when the right shift is of 32 bits. This follows the pseudocode
			 * which is often buggy.
			 */
			write_gp_register(n, (int32_t)nval >> ((~shift_cnt + 1) & 0x1FU));
		} else {
			write_gp_register(n, nval << shift_cnt);
		}
		cpu.PC += 2;
		return 0;
	case INSN_NM_SHLD:
		mval = read_gp_register(m);
		nval = read_gp_register(n);
		/* Top bit decides the direction, bottom 5 decide the magnitude */
		shift_cnt = mval & 0x0000001FU;
		if (mval & 0x80000000U) {
			/* TODO: same issue as SHAD */
			write_gp_register(n, nval >> ((~shift_cnt + 1) & 0x1FU));
		} else {
			write_gp_register(n, nval << shift_cnt);
		}
		cpu.PC += 2;
		return 0;
	case INSN_NM_MOVB_ATRM_RN:
		write_gp_register(n, sign_extend_byte(read_byte(read_gp_register(m))));
		cpu.PC += 2;
		return 0;
	case INSN_NM_MOVB_RM_ATRN:
		write_byte(read_gp_register(n), read_gp_register(m));
		cpu.PC += 2;
		return 0;
	case INSN_NM_MOVW_ATRN_RM:
		write_word(read_gp_register(n), read_gp_register(m));
		cpu.PC += 2;
		return 0;
	case INSN_NM_MOVL_RM_ATRN:
		write_longword(read_gp_register(n), read_gp_register(m));
		cpu.PC += 2;
		return 0;
	case INSN_NM_MOVL_RM_AT_MINUS_RN:
		nval = read_gp_register(n) - 4;
		write_gp_register(n, nval);
		write_longword(nval, read_gp_register(m));
		cpu.PC += 2;
		return 0;
	case INSN_NM_DIV0S:
		qbit = read_gp_register(n) & 0x80000000U;
		mbit = read_gp_register(m) & 0x80000000U;
		tbit = mbit != qbit;
		write_flag_to_long(&cpu.SR, SR_Q_BIT, qbit);
		write_flag_to_long(&cpu.SR, SR_M_BIT, mbit);
		write_flag_to_long(&cpu.SR, SR_T_BIT, tbit);
		cpu.PC += 2;
		return 0;
	case INSN_NM_TST_RM_RN:
		if (read_gp_register(n) & read_gp_register(m))
			cpu.SR &= ~SR_T_BIT;
		else
			cpu.SR |= SR_T_BIT;
		cpu.PC += 2;
		return 0;
	case INSN_NM_AND_RN_RM:
		write_gp_register(n, read_gp_register(n) & read_gp_register(m));
		cpu.PC += 2;
		return 0;
	case INSN_NM_MOV_RN_RM:
		write_gp_register(n, read_gp_register(m));
		cpu.PC += 2;
		return 0;
	case INSN_NM_MOVBP:
		mval = read_gp_register(m);
		write_gp_register(n, sign_extend_byte(read_byte(mval)));
		/*
		 * The pseudocode claims that "+1" only happens when source and target
		 * registers are not the same, but that doesn't match the behaviour of
		 * the instructions that go in the other direction, and functions such
		 * as <8003F398> would end up with their registers shifted. So I'll
		 * ignore the manual for now (TODO: confirm on physical hardware).
		 *
		 * On further thought: I think trying to save R15 to the stack is
		 * probably a bug in <8003F398>, and the manual is right here. So it
		 * won't matter much either way, because no sane code will try this?
		 */
		write_gp_register(m, mval + 1);
		cpu.PC += 2;
		return 0;
	case INSN_NM_MOVLP:
		mval = read_gp_register(m);
		write_gp_register(n, read_longword(mval));
		/* See the comment in INSN_NM_MOVBP */
		write_gp_register(m, mval + 4);
		cpu.PC += 2;
		return 0;
	case INSN_NM_NOT:
		write_gp_register(n, ~read_gp_register(m));
		cpu.PC += 2;
		return 0;
	case INSN_NM_NEGC:
		mval = read_gp_register(m);
		tbit = cpu.SR & SR_T_BIT;
		if (mval == 0 && !tbit)
			cpu.SR &= ~SR_T_BIT;
		else
			cpu.SR |= SR_T_BIT;
		write_gp_register(n, 0 - mval - (uint32_t)tbit);
		cpu.PC += 2;
		return 0;
	case INSN_NM_NEG:
		write_gp_register(n, 0 - read_gp_register(m));
		cpu.PC += 2;
		return 0;
	case INSN_NM_MOVW_RN_ATRM:
		write_gp_register(n, sign_extend_word(read_word(read_gp_register(m))));
		cpu.PC += 2;
		return 0;
	case INSN_NM_MOVL_RN_ATRM:
		write_gp_register(n, read_longword(read_gp_register(m)));
		cpu.PC += 2;
		return 0;
	case INSN_NM_EXTUB_RM_RN:
		write_gp_register(n, (uint8_t)read_gp_register(m));
		cpu.PC += 2;
		return 0;
	case INSN_NM_EXTUW_RM_RN:
		write_gp_register(n, (uint16_t)read_gp_register(m));
		cpu.PC += 2;
		return 0;
	case INSN_NM_EXTSB_RM_RN:
		write_gp_register(n, sign_extend_byte(read_gp_register(m)));
		cpu.PC += 2;
		return 0;
	case INSN_NM_EXTSW:
		write_gp_register(n, sign_extend_word(read_gp_register(m)));
		cpu.PC += 2;
		return 0;
	default:
		panic("nm instruction 0x%x not implemented\n", insn);
		return 1;
	}
}

/* Instructions of the form xxxx dddd dddd dddd */
static int execute_d12_format(uint32_t pc, uint16_t insn)
{
	uint32_t d = sign_extend_lower_12(insn);
	int32_t target;

	switch (insn & 0xF000) {
	case INSN_BRA:
		target = cpu.PC + (d << 1) + 4;
		/* Delayed slots use the target for any pc relative addressing */
		cpu.PC = target;
		return execute_delayed_slot(pc);
	default:
		break;
	}

	panic("d12 format instruction 0x%x not implemented\n", insn);
	return 1;
}

/* Instructions of the form xxxx xxxx dddd dddd (or xxxx xxxx iiii iiii) */
static int execute_d_format(uint32_t pc, uint16_t insn)
{
	uint32_t d, i;
	int32_t target;

	switch (insn & 0xFF00) {
	case INSN_I_CMPEQ:
		i = sign_extend_lower_8(insn);
		if (read_gp_register(0) == i)
			cpu.SR |= SR_T_BIT;
		else
			cpu.SR &= ~SR_T_BIT;
		cpu.PC += 2;
		return 0;
	case INSN_BT:
		d = sign_extend_lower_8(insn);
		if (!(cpu.SR & SR_T_BIT)) {
			cpu.PC += 2;
			return 0;
		}
		target = cpu.PC + (d << 1) + 4;
		cpu.PC = target;
		return 0;
	case INSN_BF:
		d = sign_extend_lower_8(insn);
		if (cpu.SR & SR_T_BIT) {
			cpu.PC += 2;
			return 0;
		}
		target = cpu.PC + (d << 1) + 4;
		cpu.PC = target;
		return 0;
	case INSN_BTS:
		d = sign_extend_lower_8(insn);
		if (!(cpu.SR & SR_T_BIT)) {
			cpu.PC += 2;
			return 0;
		}
		target = cpu.PC + (d << 1) + 4;
		cpu.PC = target;
		return execute_delayed_slot(pc);
	case INSN_D_BFS:
		d = sign_extend_lower_8(insn);
		if (cpu.SR & SR_T_BIT) {
			cpu.PC += 2;
			return 0;
		}
		target = cpu.PC + (d << 1) + 4;
		cpu.PC = target;
		return execute_delayed_slot(pc);
	case INSN_MOVB_R0_TO_AT_DISP_GBR:
		d = insn & 0x00FFU;
		write_byte(cpu.GBR + d, read_gp_register(0));
		cpu.PC += 2;
		return 0;
	case INSN_MOVW_R0_TO_AT_DISP_GBR:
		d = insn & 0x00FFU;
		write_word(cpu.GBR + (d << 1), read_gp_register(0));
		cpu.PC += 2;
		return 0;
	case INSN_D_MOVLSG:
		d = insn & 0x00FFU;
		write_longword(cpu.GBR + (d << 2), read_gp_register(0));
		cpu.PC += 2;
		return 0;
	case INSN_I_TRAPA:
		/* TODO: find all instructions that can't be in a delayed slot */
		if (cpu.extra_state & EXTRA_IN_DELAYED)
			panic("Invalid delay slot! (TODO)\n");
		i = insn & 0x00FFU;
		cpu.TRA = i << 4;
		cpu.SSR = cpu.SR;
		/*
		 * The pseudocode in the ISA manual doesn't have the "+2" here, but
		 * according to the SH7709A manual "the PC of the instruction after the
		 * TRAPA instruction is saved to the SPC". This makes sense, otherwise
		 * RTE would bring us back to the same TRAPA instruction...
		 */
		cpu.SPC = cpu.PC + 2;
		cpu.SR |= (SR_BL_BIT | SR_RB_BIT | SR_MD_BIT);
		cpu.EXPEVT = 0x160;
		/* The manual seems to be missing the "+4" here... */
		cpu.PC = cpu.VBR + 0x0100 + 4;
		backtrace_push(cpu.SPC - 2, cpu.PC, false /* interrupt */);
		return 0;
	case INSN_D_MOVA:
		d = insn & 0x00FFU;
		pc &= 0xFFFFFFFC;
		write_gp_register(0, pc + (d << 2));
		cpu.PC += 2;
		return 0;
	case INSN_TSTI:
		i = insn & 0x00FFU;
		if (read_gp_register(0) & i)
			cpu.SR &= ~SR_T_BIT;
		else
			cpu.SR |= SR_T_BIT;
		cpu.PC += 2;
		return 0;
	case INSN_AND_I8_R0:
		i = insn & 0x00FFU;
		write_gp_register(0, read_gp_register(0) & i);
		cpu.PC += 2;
		return 0;
	case INSN_I_XORI:
		i = insn & 0x00FFU;
		write_gp_register(0, read_gp_register(0) ^ i);
		cpu.PC += 2;
		return 0;
	case INSN_I_ORI:
		i = insn & 0x00FFU;
		write_gp_register(0, read_gp_register(0) | i);
		cpu.PC += 2;
		return 0;
	case INSN_I_MOVBLG:
		d = insn & 0x00FFU;
		write_gp_register(0, sign_extend_byte(read_byte(cpu.GBR + d)));
		cpu.PC += 2;
		return 0;
	case INSN_I_MOVWLG:
		d = insn & 0x00FFU;
		write_gp_register(0, sign_extend_word(read_word(cpu.GBR + (d << 1))));
		cpu.PC += 2;
		return 0;
	default:
		break;
	}

	panic("d format instruction 0x%x not implemented\n", insn);
	return 1;
}

/* Instructions of the form xxxx xxxx nnnn dddd, or maybe xxxx xxxx mmmm dddd */
static int execute_nd4_format(uint32_t pc, uint16_t insn)
{
	uint32_t d = insn & 0x000FU;
	uint8_t m, n;
	uint32_t mval;

	/*
	 * Slightly confusing: technically instructions that use the "m" are a
	 * different format called "md", but it would be annoying to try to tell
	 * them apart.
	 */
	m = n = (insn & 0x00F0U) >> 4;

	switch (insn & 0xFF00) {
	case INSN_ND4_MOVBS4:
		write_byte(read_gp_register(n) + d, read_gp_register(0));
		cpu.PC += 2;
		return 0;
	case INSN_ND4_MOVW:
		write_word(read_gp_register(n) + (d << 1), read_gp_register(0));
		cpu.PC += 2;
		return 0;
	case INSN_ND4_MOVBL4:
		mval = read_gp_register(m);
		write_gp_register(0, sign_extend_byte(read_byte(mval + d)));
		cpu.PC += 2;
		return 0;
	case INSN_ND4_MOVWL4:
		mval = read_gp_register(m);
		write_gp_register(0, sign_extend_word(read_word(mval + (d << 1))));
		cpu.PC += 2;
		return 0;
	default:
		panic("nd4 format instruction 0x%x not implemented\n", insn);
		return 1;
	}
}

/* Instructions of the form xxxx xxxx **** **** */
static int execute_xxuu_format(uint32_t pc, uint16_t insn)
{
	switch (insn & 0xF800) {
	case 0x8000:
		return execute_nd4_format(pc, insn);
	case 0x8800:
	case 0xC000:
	case 0xC800:
		return execute_d_format(pc, insn);
	default:
		panic("xxuu instruction 0x%x not implemented\n", insn);
		return 1;
	}
}

/* Instructions of the form 0000 **** **** **** */
static int execute_0uuu_format(uint32_t pc, uint16_t insn)
{
	switch (insn & 0xF00F) {
	case 0x0002:
	case 0x000A:
		return execute_n_format(pc, insn);
	case 0x0003:
		if ((insn & 0x00FF) == 0x0083)
			return execute_n_format(pc, insn);
		return execute_m_format(pc, insn);
	case 0x0008:
	case 0x0009:
	case 0x000B:
		if ((insn & 0x00FF) == 0x0029) /* Only MOVT I think */
			return execute_n_format(pc, insn);
		return execute_0_format(pc, insn);
	case 0x0004:
	case 0x0005:
	case 0x0006:
	case 0x0007:
	case 0x000C:
	case 0x000D:
	case 0x000E:
		return execute_nm_format(pc, insn);
	default:
		panic("0uuu instruction 0x%x not implemented\n", insn);
		return 1;
	}
}

/* Instructions of the form xxxx nnnn mmmm dddd */
static int execute_nmd_format(uint32_t pc, uint16_t insn)
{
	uint8_t n = (insn & 0x0F00U) >> 8;
	uint8_t m = (insn & 0x00F0U) >> 4;
	uint32_t d = insn & 0x000FU;

	switch (insn & 0xF000) {
	case INSN_MOVL_AT_DISP_RM_TO_RN:
		write_gp_register(n, read_longword(read_gp_register(m) + (d << 2)));
		cpu.PC += 2;
		return 0;
	case INSN_MOVL_RM_TO_AT_DISP_RN:
		write_longword(read_gp_register(n) + (d << 2), read_gp_register(m));
		cpu.PC += 2;
		return 0;
	default:
		panic("nmd format instruction 0x%x not implemented\n", insn);
		return 1;
	}
}

static uint16_t read_insn(uint32_t pc)
{
	struct patch *p = NULL;
	int i;

	/* Due to the pipeline, the instruction on execution is always at PC-4 */
	pc -= 4;

	for (i = 0; i < patches.patch_count; ++i) {
		p = &patches.p[i];
		if (pc == p->addr)
			return p->insn;
	}
	return read_word(pc);
}

/* Execute the instruction at @pc */
static int execute(uint32_t pc)
{
	uint16_t insn;
	int ret;

	if (panicked) {
		printf("Emulator bug: execution can't continue\n");
		return 1;
	}

	insn = read_insn(pc);
	switch (insn & 0xF000) {
	case 0x0000:
		ret = execute_0uuu_format(pc, insn);
		break;
	case 0x9000:
	case 0xD000:
		ret = execute_nd8_format(pc, insn);
		break;
	case 0x4000:
		ret = execute_xuux_format(pc, insn);
		break;
	case 0x2000:
	case 0x3000:
	case 0x6000:
		ret = execute_nm_format(pc, insn);
		break;
	case 0xA000:
		ret = execute_d12_format(pc, insn);
		break;
	case 0x7000:
	case 0xE000:
		ret = execute_ni8_format(pc, insn);
		break;
	case 0x8000:
	case 0xC000:
		ret = execute_xxuu_format(pc,insn);
		break;
	case 0x1000:
	case 0x5000:
		ret = execute_nmd_format(pc, insn);
		break;
	case 0xF000:
		/*
		 * These are 32-bit DSP instructions, not supported by this processor.
		 * The firmware has a short burst of them early on for some reason. The
		 * BL bit is set at this point so this should trigger a reset according
		 * to the sh7709a manual (see section 4.2.5, "Exception Request Masks"),
		 * but then we would never boot.
		 *
		 * I found this comment on a forum:
		 *
		 *   https://www.cemetech.net/forum/viewtopic.php?p=239954#239954
		 *
		 * Apparently some sh3 CPUs will accept DSP instructions as valid but
		 * mostly ignore them. The sh7709a must be one of them, but I have no
		 * way to be sure at the moment (TODO).
		 */
		if (cpu.SR & SR_DSP_BIT) {
			notice("Ignoring DSP instruction (PC: 0x%.8x)\n", cpu.PC);
			cpu.PC += 4;
			ret = 0;
		} else {
			panic("DSP instruction encountered outside DSP mode (PC: 0x%.8x)\n", cpu.PC);
			ret = 1;
		}
		break;
	default:
		panic("Instruction 0x%x not implemented\n", insn);
		ret = 1;
		break;
	}

	return ret;
}

/* Instructions of the form 0000 xxxx xxxx xxxx */
static void disassemble_0_format(uint32_t pc, uint16_t insn)
{
	switch (insn & 0xFFFF) {
	case INSN_0_CLRT:
		printf("CLRT\n");
		return;
	case INSN_0_NOP:
		printf("NOP\n");
		return;
	case INSN_0_RTS:
		printf("RTS\n");
		return;
	case INSN_0_RTE:
		printf("RTE\n");
		return;
	case INSN_0_DIV0U:
		printf("DIV0U\n");
		return;
	case INSN_0_SLEEP:
		printf("SLEEP\n");
		return;
	case INSN_0_LDTLB:
		printf("LDTLB\n");
		return;
	}
	printf("0 format instruction 0x%x not implemented\n", insn);
}

/* Instructions of the form xxxx nnnn dddd dddd */
static void disassemble_nd8_format(uint32_t pc, uint16_t insn)
{
	uint8_t d = (insn & 0x00FFU);
	uint8_t n = (insn & 0x0F00U) >> 8;

	switch (insn & 0xF000) {
	case INSN_ND8_MOV_W:
		printf("MOV.W @($%x,PC),R%u\n", d, n);
		return;
	case INSN_ND8_MOV_L:
		printf("MOV.L @($%x,PC),R%u\n", d, n);
		return;
	default:
		printf("ND8 format instruction 0x%x not implemented\n", insn);
		return;
	}
}

/* Instructions of the form xxxx nnnn iiii iiii */
static void disassemble_ni8_format(uint32_t pc, uint16_t insn)
{
	uint8_t i = (insn & 0x00FFU);
	uint8_t n = (insn & 0x0F00U) >> 8;

	switch (insn & 0xF000) {
	case INSN_ADD_I8_RN:
		printf("ADD $%x,R%u\n", i, n);
		return;
	case INSN_MOV_I8_RN:
		printf("MOV $%x,R%u\n", i, n);
		return;
	default:
		printf("NI8 format instruction 0x%x not implemented\n", insn);
		return;
	}
}

/* Instructions of the form xxxx nnnn xxxx xxxx */
static void disassemble_n_format(uint32_t pc, uint16_t insn)
{
	uint8_t n = (insn & 0x0F00U) >> 8;

	switch (insn & 0xF0FF) {
	case INSN_N_STC_SR_RN:
		printf("STC SR,R%u\n", n);
		return;
	case INSN_N_STCVBR:
		printf("STC VBR,R%u\n", n);
		return;
	case INSN_N_PREF:
		printf("PREF @R%u\n", n);
		return;
	case INSN_N_STSMACH:
		printf("STS MACH,R%u\n", n);
		return;
	case INSN_N_STSMACL:
		printf("STS MACL,R%u\n", n);
		return;
	case INSN_N_MOVT:
		printf("MOVT R%u\n", n);
		return;
	case INSN_N_SHLL:
		printf("SHLL R%u\n", n);
		return;
	case INSN_N_SHLR:
		printf("SHLR R%u\n", n);
		return;
	case INSN_N_STSMMACH:
		printf("STS.L MACH,@-R%u\n", n);
		return;
	case INSN_N_STSMMACL:
		printf("STS.L MACL,@-R%u\n", n);
		return;
	case INSN_N_STCMSR:
		printf("STC.L SR,@-R%u\n", n);
		return;
	case INSN_N_STCMGBR:
		printf("STC.L GBR,@-R%u\n", n);
		return;
	case INSN_N_STCMVBR:
		printf("STC.L VBR,@-R%u\n", n);
		return;
	case INSN_N_STCMSSR:
		printf("STC.L SSR,@-R%u\n", n);
		return;
	case INSN_N_STCMSPC:
		printf("STC.L SPC,@-R%u\n", n);
		return;
	case INSN_N_SHLL2:
		printf("SHLL2 R%u\n", n);
		return;
	case INSN_N_SHLL8:
		printf("SHLL8 R%u\n", n);
		return;
	case INSN_N_SHLR2:
		printf("SHLR2 R%u\n", n);
		return;
	case INSN_N_SHLR8:
		printf("SHLR8 R%u\n", n);
		return;
	case INSN_N_DT_RN:
		printf("DT R%u\n", n);
		return;
	case INSN_N_CMPPZ:
		printf("CMP/PZ R%u\n", n);
		return;
	case INSN_N_CMPPL:
		printf("CMP/PL R%u\n", n);
		return;
	case INSN_N_SHAR:
		printf("SHAR R%u\n", n);
		return;
	case INSN_N_STSL_PR_AT_MINUS_RN:
		printf("STS.L PR,@-R%u\n", n);
		return;
	case INSN_N_ROTR:
		printf("ROTR R%u\n", n);
		return;
	case INSN_N_ROTCL:
		printf("ROTCL R%u\n", n);
		return;
	case INSN_N_ROTCR:
		printf("ROTCR R%u\n", n);
		return;
	case INSN_N_SHLL16:
		printf("SHLL16 R%u\n", n);
		return;
	case INSN_N_SHLR16:
		printf("SHLR16 R%u\n", n);
		return;
	default:
		printf("N format instruction 0x%x not implemented\n", insn);
		return;
	}
}

/* Instructions of the form xxxx mmmm xxxx xxxx */
static void disassemble_m_format(uint32_t pc, uint16_t insn)
{
	uint8_t m = (insn & 0x0F00U) >> 8;

	switch (insn & 0xF0FF) {
	case INSN_M_BRAF_RM:
		printf("BRAF R%u\n", m);
		return;
	case INSN_M_JSR_AT_RM:
		printf("JSR @R%u\n", m);
		return;
	case INSN_M_LDS_RM_MACH:
		printf("LDS R%u,MACH\n", m);
		return;
	case INSN_M_LDS_RM_MACL:
		printf("LDS R%u,MACL\n", m);
		return;
	case INSN_M_LDCSR:
		printf("LDC R%u,SR\n", m);
		return;
	case INSN_M_LDSMMACH:
		printf("LDS.L @R%u+,MACH\n", m);
		return;
	case INSN_M_LDCMSR:
		printf("LDC.L @R%u+,SR\n", m);
		return;
	case INSN_M_LDSMMACL:
		printf("LDS.L @R%u+,MACL\n", m);
		return;
	case INSN_M_LDCMGBR:
		printf("LDC.L @R%u+,GBR\n", m);
		return;
	case INSN_M_LDC_RM_GBR:
		printf("LDC R%u,GBR\n", m);
		return;
	case INSN_M_LDCVBR:
		printf("LDC R%u,VBR\n", m);
		return;
	case INSN_M_LDSMPR:
		printf("LDS.L @R%u+,PR\n", m);
		return;
	case INSN_M_JMP:
		printf("JMP @R%u\n", m);
		return;
	case INSN_M_LDCMVBR:
		printf("LDC.L @R%u+,VBR\n", m);
		return;
	case INSN_M_LDCMSSR:
		printf("LDC.L @R%u+,SSR\n", m);
		return;
	case INSN_M_LDCMSPC:
		printf("LDC.L @R%u+,SPC\n", m);
		return;
	default:
		printf("M format instruction 0x%x not implemented\n", insn);
		return;
	}
}

static void disassemble_nm_format(uint32_t pc, uint16_t insn);

/* Instructions of the form xxxx **** **** xxxx */
static void disassemble_xuux_format(uint32_t pc, uint16_t insn)
{
	switch (insn & 0x000F) {
	case 0x0000:
	case 0x0001:
	case 0x0002:
	case 0x0003:
	case 0x0004:
	case 0x0005:
	case 0x0008:
	case 0x0009:
		return disassemble_n_format(pc, insn);
	case 0x0006:
	case 0x0007:
	case 0x000A:
	case 0x000B:
	case 0x000E:
		return disassemble_m_format(pc, insn);
	case 0x000C:
	case 0x000D:
		return disassemble_nm_format(pc, insn);
	default:
		printf("xuux instruction 0x%x not implemented\n", insn);
		return;
	}
}

/* Instructions of the form xxxx nnnn mmmm xxxx */
static void disassemble_nm_format(uint32_t pc, uint16_t insn)
{
	uint8_t n = (insn & 0x0F00U) >> 8;
	uint8_t m = (insn & 0x00F0U) >> 4;

	switch (insn & 0xF00F) {
	case INSN_NM_MOVBS0:
		printf("MOV.B R%u,@(R0,R%u)\n", m, n);
		return;
	case INSN_NM_MOVWS0:
		printf("MOV.W R%u,@(R0,R%u)\n", m, n);
		return;
	case INSN_NM_MOVLS0:
		printf("MOV.L R%u,@(R0,R%u)\n", m, n);
		return;
	case INSN_NM_MULL:
		printf("MUL.L R%u,R%u\n", m, n);
		return;
	case INSN_NM_MOVWL0:
		printf("MOV.W @(R0,R%u),R%u\n", m, n);
		return;
	case INSN_NM_MOVLL0:
		printf("MOV.L @(R0,R%u),R%u\n", m, n);
		return;
	case INSN_NM_MOVBL0:
		printf("MOV.B @(R0,R%u),R%u\n", m, n);
		return;
	case INSN_NM_OR_RM_RN:
		printf("OR R%u,R%u\n", m, n);
		return;
	case INSN_NM_XOR:
		printf("XOR R%u,R%u\n", m, n);
		return;
	case INSN_NM_CMPSTR:
		printf("CMP/STR R%u,R%u\n", m, n);
		return;
	case INSN_NM_XTRCT:
		printf("XTRCT R%u,R%u\n", m, n);
		return;
	case INSN_NM_CMPEQ_RM_RN:
		printf("CMP/EQ R%u,R%u\n", m, n);
		return;
	case INSN_NM_CMPHS:
		printf("CMP/HS R%u,R%u\n", m, n);
		return;
	case INSN_NM_CMPGE:
		printf("CMP/GE R%u,R%u\n", m, n);
		return;
	case INSN_NM_DIV1:
		printf("DIV1 R%u,R%u\n", m, n);
		return;
	case INSN_NM_DMULU:
		printf("DMULU.L R%u,R%u\n", m, n);
		return;
	case INSN_NM_CMPHI:
		printf("CMP/HI R%u,R%u\n", m, n);
		return;
	case INSN_NM_CMPGT_RM_RN:
		printf("CMP/GT R%u,R%u\n", m, n);
		return;
	case INSN_NM_SUB:
		printf("SUB R%u,R%u\n", m, n);
		return;
	case INSN_NM_SUBC:
		printf("SUBC R%u,R%u\n", m, n);
		return;
	case INSN_NM_ADD:
		printf("ADD R%u,R%u\n", m, n);
		return;
	case INSN_NM_ADDC:
		printf("ADDC R%u,R%u\n", m, n);
		return;
	case INSN_NM_SHAD:
		printf("SHAD R%u,R%u\n", m, n);
		return;
	case INSN_NM_SHLD:
		printf("SHLD R%u,R%u\n", m, n);
		return;
	case INSN_NM_MOVB_ATRM_RN:
		printf("MOV.B @R%u,R%u\n", m, n);
		return;
	case INSN_NM_MOVB_RM_ATRN:
		printf("MOV.B R%u,@R%u\n", m, n);
		return;
	case INSN_NM_MOVW_ATRN_RM:
		printf("MOV.W R%u,@R%u\n", m, n);
		return;
	case INSN_NM_MOVL_RM_ATRN:
		printf("MOV.L R%u,@R%u\n", m, n);
		return;
	case INSN_NM_MOVL_RM_AT_MINUS_RN:
		printf("MOV.L R%u,@-R%u\n", m, n);
		return;
	case INSN_NM_DIV0S:
		printf("DIV0S R%u,R%u\n", m, n);
		return;
	case INSN_NM_TST_RM_RN:
		printf("TST R%u,R%u\n", m, n);
		return;
	case INSN_NM_AND_RN_RM:
		printf("AND R%u,R%u\n", m, n);
		return;
	case INSN_NM_MOV_RN_RM:
		printf("MOV R%u,R%u\n", m, n);
		return;
	case INSN_NM_MOVBP:
		printf("MOV.B @R%u+,R%u\n", m, n);
		return;
	case INSN_NM_MOVLP:
		printf("MOV.L @R%u+,R%u\n", m, n);
		return;
	case INSN_NM_NOT:
		printf("NOT R%u,R%u\n", m, n);
		return;
	case INSN_NM_NEGC:
		printf("NEGC R%u,R%u\n", m, n);
		return;
	case INSN_NM_NEG:
		printf("NEG R%u,R%u\n", m, n);
		return;
	case INSN_NM_MOVW_RN_ATRM:
		printf("MOV.W @R%u,R%u\n", m, n);
		return;
	case INSN_NM_MOVL_RN_ATRM:
		printf("MOV.L @R%u,R%u\n", m, n);
		return;
	case INSN_NM_EXTUB_RM_RN:
		printf("EXTU.B R%u,R%u\n", m, n);
		return;
	case INSN_NM_EXTUW_RM_RN:
		printf("EXTU.W R%u,R%u\n", m, n);
		return;
	case INSN_NM_EXTSB_RM_RN:
		printf("EXTS.B R%u,R%u\n", m, n);
		return;
	case INSN_NM_EXTSW:
		printf("EXTS.W R%u,R%u\n", m, n);
		return;
	default:
		printf("nm instruction 0x%x not implemented\n", insn);
		return;
	}
}

/* Instructions of the form xxxx dddd dddd dddd */
static void disassemble_d12_format(uint32_t pc, uint16_t insn)
{
	uint32_t d = sign_extend_lower_12(insn);
	int32_t target;

	switch (insn & 0xF000) {
	case INSN_BRA:
		target = pc + (d << 1) + 4;
		printf("BRA $%.8x\n", target - 4);
		return;
	default:
		printf("d12 format instruction 0x%x not implemented\n", insn);
		return;
	}
}

/* Instructions of the form xxxx xxxx dddd dddd (or xxxx xxxx iiii iiii) */
static void disassemble_d_format(uint32_t pc, uint16_t insn)
{
	uint32_t d, i;
	int32_t target;

	switch (insn & 0xFF00) {
	case INSN_I_CMPEQ:
		i = sign_extend_lower_8(insn);
		printf("CMP/EQ #%d,R0\n", i);
		return;
	case INSN_BT:
		d = sign_extend_lower_8(insn);
		target = pc + (d << 1) + 4;
		printf("BT $%.8x\n", target - 4);
		return;
	case INSN_BF:
		d = sign_extend_lower_8(insn);
		target = pc + (d << 1) + 4;
		printf("BF $%.8x\n", target - 4);
		return;
	case INSN_BTS:
		d = sign_extend_lower_8(insn);
		target = pc + (d << 1) + 4;
		printf("BT/S $%.8x\n", target - 4);
		return;
	case INSN_D_BFS:
		d = sign_extend_lower_8(insn);
		target = pc + (d << 1) + 4;
		printf("BF/S $%.8x\n", target - 4);
		return;
	case INSN_MOVB_R0_TO_AT_DISP_GBR:
		d = insn & 0x00FFU;
		printf("MOV.B R0,@($%.2x,GBR)\n", d);
		return;
	case INSN_MOVW_R0_TO_AT_DISP_GBR:
		d = insn & 0x00FFU;
		printf("MOV.W R0,@($%.2x,GBR)\n", d);
		return;
	case INSN_D_MOVLSG:
		d = insn & 0x00FFU;
		printf("MOV.L R0,@($%.2x,GBR)\n", d);
		return;
	case INSN_I_TRAPA:
		i = insn & 0x00FFU;
		printf("TRAPA #%d\n", i);
		return;
	case INSN_D_MOVA:
		d = insn & 0x00FFU;
		printf("MOVA @($%x,PC),R0\n", d);
		return;
	case INSN_TSTI:
		i = insn & 0x00FFU;
		printf("TST $%x,R0\n", i);
		return;
	case INSN_AND_I8_R0:
		i = insn & 0x00FFU;
		printf("AND $%x,R0\n", i);
		return;
	case INSN_I_XORI:
		i = insn & 0x00FFU;
		printf("XOR $%x,R0\n", i);
		return;
	case INSN_I_ORI:
		i = insn & 0x00FFU;
		printf("OR $%x,R0\n", i);
		return;
	case INSN_I_MOVBLG:
		d = insn & 0x00FFU;
		printf("MOV.B @($%.2x,GBR),R0\n", d);
		return;
	case INSN_I_MOVWLG:
		d = insn & 0x00FFU;
		printf("MOV.W @($%.2x,GBR),R0\n", d);
		return;
	default:
		printf("d (or i) format instruction 0x%x not implemented\n", insn);
		return;
	}
}

/* Instructions of the form xxxx xxxx nnnn dddd, or maybe xxxx xxxx mmmm dddd */
static void disassemble_nd4_format(uint32_t pc, uint16_t insn)
{
	uint32_t d = insn & 0x000FU;
	uint8_t m, n;

	m = n = (insn & 0x00F0U) >> 4;

	switch (insn & 0xFF00) {
	case INSN_ND4_MOVBS4:
		printf("MOV.B R0,@($%.2x,R%u)\n", d, n);
		return;
	case INSN_ND4_MOVW:
		printf("MOV.W R0,@($%.2x,R%u)\n", d, n);
		return;
	case INSN_ND4_MOVBL4:
		printf("MOV.B @($%.2x,R%u),R0\n", d, m);
		return;
	case INSN_ND4_MOVWL4:
		printf("MOV.W @($%.2x,R%u),R0\n", d, m);
		return;
	default:
		printf("nd4 format instruction 0x%x not implemented\n", insn);
		return;
	}
}

/* Instructions of the form xxxx xxxx **** **** */
static void disassemble_xxuu_format(uint32_t pc, uint16_t insn)
{
	switch (insn & 0xF800) {
	case 0x8000:
		return disassemble_nd4_format(pc, insn);
	case 0x8800:
	case 0xC000:
	case 0xC800:
		return disassemble_d_format(pc, insn);
	default:
		printf("xxuu instruction 0x%x not implemented\n", insn);
		return;
	}
}

/* Instructions of the form 0000 **** **** **** */
static void disassemble_0uuu_format(uint32_t pc, uint16_t insn)
{
	switch (insn & 0xF00F) {
	case 0x0002:
	case 0x000A:
		return disassemble_n_format(pc, insn);
	case 0x0003:
		if ((insn & 0x00FF) == 0x0083)
			return disassemble_n_format(pc, insn);
		return disassemble_m_format(pc, insn);
	case 0x0008:
	case 0x0009:
	case 0x000B:
		if ((insn & 0x00FF) == 0x0029)
			return disassemble_n_format(pc, insn);
		return disassemble_0_format(pc, insn);
	case 0x0004:
	case 0x0005:
	case 0x0006:
	case 0x0007:
	case 0x000C:
	case 0x000D:
	case 0x000E:
		return disassemble_nm_format(pc, insn);
	default:
		printf("0uuu instruction 0x%x not implemented\n", insn);
		return;
	}
}

/* Instructions of the form xxxx nnnn mmmm dddd */
static void disassemble_nmd_format(uint32_t pc, uint16_t insn)
{
	uint8_t n = (insn & 0x0F00U) >> 8;
	uint8_t m = (insn & 0x00F0U) >> 4;
	uint32_t d = insn & 0x000FU;

	switch (insn & 0xF000) {
	case INSN_MOVL_AT_DISP_RM_TO_RN:
		printf("MOV.L @($%x,R%u),R%u\n", d, m, n);
		return;
	case INSN_MOVL_RM_TO_AT_DISP_RN:
		printf("MOV.L R%u,@($%x,R%u)\n", m, d, n);
		return;
	default:
		printf("nmd format instruction 0x%x not implemented\n", insn);
		return;
	}
}

/* Disassemble the instruction at @pc */
static void disassemble(uint32_t pc)
{
	uint16_t insn;

	insn = read_insn(pc);
	switch (insn & 0xF000) {
	case 0x0000:
		return disassemble_0uuu_format(pc, insn);
	case 0x9000:
	case 0xD000:
		return disassemble_nd8_format(pc, insn);
	case 0x4000:
		return disassemble_xuux_format(pc, insn);
	case 0x2000:
	case 0x3000:
	case 0x6000:
		return disassemble_nm_format(pc, insn);
	case 0xA000:
		return disassemble_d12_format(pc, insn);
	case 0x7000:
	case 0xE000:
		return disassemble_ni8_format(pc, insn);
	case 0x8000:
	case 0xC000:
		return disassemble_xxuu_format(pc,insn);
	case 0x1000:
	case 0x5000:
		return disassemble_nmd_format(pc, insn);
	default:
		printf("instruction 0x%x not implemented\n", insn);
		return;
	}
}

/* Increment the RFCR bsc register */
static void increment_bsc_rfcr(void)
{
	int limit = bsc.RTCSR & RTCSR_LMTS ? 512 : 1024;

	bsc.RFCR += 1;
	if (bsc.RFCR >= limit) {
		bsc.RFCR = 0;
		bsc.RTCSR |= RTCSR_OVF;
		if (bsc.RTCSR & RTCSR_OVIE)
			panic("Interrupt from OVF not implemented\n");
	}
}

/* Increment the RTCNT bsc register */
static void increment_bsc_rtcnt(void)
{
	bsc.RTCNT += 1;
	if (bsc.RTCNT == bsc.RTCOR) {
		bsc.RTCNT = 0;
		bsc.RTCSR |= RTCSR_CMF;
		if (bsc.RTCSR & RTCSR_CMIE)
			panic("Interrupt from CMF not implemented\n");
		increment_bsc_rfcr();
	}
}

/* Decrement the TCNT0-2 TMU register */
static void decrement_tmu_tcnt(int i)
{
	tmu.TCNT[i] -= 1;
	if (tmu.TCNT[i] == 0xFFFFFFFFU) {
		tmu.TCNT[i] = tmu.TCOR[i];
		tmu.TCR[i] |= TCR_UNF;
	}
}

/* Get the prescale value for the TCNT0-2 clocks */
static int tmu_prescaler(int i)
{
	int tpsc, result;

	result = 4;
	tpsc = tmu.TCR[i] & TCR_TPSC;
	while (tpsc--)
		result <<= 2;
	return result;
}

/*
 * We want the tests to be deterministic so, when running headless, the clock
 * update frequency is arbitrarily synced to the execution loop. All that
 * matters is that clocks remain consistent with each other, and that they don't
 * make the tests unnecessarily slow.
 */
static void update_clocks(void)
{
	int i;

	switch (bsc.RTCSR & RTCSR_CKS) {
	case 0x0000:
		break;	/* Clock is disabled */
	case 0x0010:
		++bsc.ckio_tics;
		if (bsc.ckio_tics == 16) {
			bsc.ckio_tics = 0;
			increment_bsc_rtcnt();
		}
		break;
	default:
		panic("RTCNT clock input 0x%x not implemented\n", bsc.RTCSR & RTCSR_CKS);
		return;
	}

	for (i = 0; i < 3; ++i) {
		if ((tmu.TSTR & (1U << i)) == 0)	/* Is this timer halted? */
			continue;
		tmu.peripheral_tics[i] += 4;
		if (tmu.peripheral_tics[i] >= tmu_prescaler(i)) {
			tmu.peripheral_tics[i] = 0;
			decrement_tmu_tcnt(i);
		}
		break;
	}

	/*
	 * R64CNT updates at 64 Hz, while the peripheral clock is set to ~22.12Mhz
	 * (or so it seems from looking at <8003BB0C>).
	 */
	rtc.peripheral_tics += 4;
	if (rtc.peripheral_tics >= 345600) {
		rtc.peripheral_tics = 0;
		/* TODO: update the seconds on overflow, and so on... */
		if (++rtc.R64CNT == 64) {
			rtc.R64CNT = 0;
			rtc.RCR1 |= RCR1_CF;
		}
	}
}

static void scif_receive_single_char(void)
{
	char c;

	if (scif.rx_queue_cnt == 0)
		return;
	c = scif.rx_queue[scif.rx_queue_start];
	scif.rx_queue_start = (scif.rx_queue_start + 1) % SCIF_RX_QUEUE_SIZE;
	scif.rx_queue_cnt--;

	/* TODO: generic fifo structure? Ring buffer implementation? */
	if (scif.SCFRDR2_count == 16)
		return;
	memmove(&scif.SCFRDR2[1], &scif.SCFRDR2[0], scif.SCFRDR2_count++);
	scif.SCFRDR2[0] = c;
	/* The number of receive triggers is probably always 1 */
	scif.SCSSR2 |= SCSSR2_RDF;
	scif.SCSSR2_unread |= SCSSR2_RDF;
	/* TODO: serial interrupts? Are they even used by the jornada? */
	/* TODO: break detection? */
}

static void send_single_char(void)
{
	/* TODO: shoudn't transmission be blocked when TE is unset? */
	/* TODO: interrupts? */
	if (scif.SCFTDR2_count == 0)
		return;
	/* TODO: send to an actual serial device */
	console_monitor_save_byte(&serial_monitor, scif.SCFTDR2[--scif.SCFTDR2_count]);
	if (scif.SCFTDR2_count == 0) {
		/* TDFE should probably not be set (TODO) */
		scif.SCSSR2 |= (SCSSR2_TEND | SCSSR2_TDFE);
		scif.SCSSR2_unread |= (SCSSR2_TEND | SCSSR2_TDFE);
	}
}

/*
 * The emulator just submits one byte over serial on each loop, and receives
 * one byte from its rx buffer every 80 loops.
 */
static void update_scif(void)
{
	static int loops = 0;

	send_single_char();

	/*
	 * Code that reads from serial such as <0x80032090> seems to expect that
	 * the RDF flag will always be set if data is available. The manual
	 * disagrees: RDF only gets set when new data arrives. I'm guessing this
	 * works for physical hardware because characters arrive slowly, and the
	 * code the unsets the flag is much faster. Try to fake that here: waiting
	 * 40 loops seems to work, so use 80 to be sure.
	 */
	if (++loops == 80) {
		loops = 0;
		scif_receive_single_char();
	}
}

/*
 * If the top light is set to blink, we make it happen once every 500000 loops.
 * Of course there isn't much to emulate right now, so we just print a message.
 * TODO: remember to implement this properly once we have SDL set up.
 */
static void update_top_light(void)
{
	static int loops = 0;

	if (motherboard.status & MBOARD_NOT_BLINKING) {
		loops = 0;
		return;
	}

	if (++loops == 500000) {
		loops = 0;
		notice("Blink!\n");
		/*
		 * <0x8002F968> gets called very early on boot, and in the end it sets
		 * the blink count to 4 for power-on-reset and to 2 for manual reset. I
		 * tested this on real hardware though, and the blinks are actually 5
		 * and 3, respectively. So my guess is that we actually count down to
		 * -1, but I'm not sure (TODO).
		 */
		if (motherboard.blinkcnt-- == 0)
			motherboard.status |= MBOARD_NOT_BLINKING;
	}
}

/* TODO: how to handle delayed slots? */
static bool pc_is_breakpoint(uint32_t pc)
{
	int i;

	/* Due to the pipeline, the instruction on execution is always at PC-4 */
	pc -= 4;

	/*
	 * Don't keep breaking in the same place. TODO: this seems to break down
	 * with interrupts sometimes.
	 */
	if (breakpoints.hit) {
		breakpoints.hit = false;
		return false;
	}

	for (i = 0; i < breakpoints.pcs_count; ++i) {
		if (pc == breakpoints.pcs[i]) {
			breakpoints.hit = true;
			return true;
		}
	}
	return false;
}

static void become_interactive(void)
{
	interactive = true;
	if (script_file) {
		fclose(script_file);
		script_file = NULL;
	}
}

/* Returns the maximum interrupt priority currently masked by the SR */
static int sr_interrupt_mask(void)
{
	return (cpu.SR & SR_I_BITS) >> SR_I_SHIFT;
}

static uint32_t priority_to_intevt(int priority)
{
	return 0x3C0 - (priority - 1) * 0x20;
}

/* Accept an IRQi interrupt, unless its priority is too low for the SR mask */
static void irq_accept(int i, int priority)
{
	if (priority <= sr_interrupt_mask())
		return;
	cpu.SPC = cpu.PC;
	cpu.SSR = cpu.SR;
	cpu.SR |= (SR_BL_BIT | SR_MD_BIT | SR_RB_BIT);
	cpu.PC = cpu.VBR + 0x600 + 4;
	cpu.INTEVT = priority_to_intevt(priority);
	cpu.INTEVT2 = 0x600 + i * 0x20;
	backtrace_push(cpu.SPC, cpu.PC, true /* interrupt */);
}

/*
 * Accept an interrupt among PINT0-7 (if i == 0) of PINT8-15 (if i == 1),
 * unless its priority is too low for the SR mask.
 */
static void pint_accept(int i, int priority)
{
	if (priority <= sr_interrupt_mask())
		return;
	cpu.SPC = cpu.PC;
	cpu.SSR = cpu.SR;
	cpu.SR |= (SR_BL_BIT | SR_MD_BIT | SR_RB_BIT);
	cpu.PC = cpu.VBR + 0x600 + 4;
	cpu.INTEVT = priority_to_intevt(priority);
	cpu.INTEVT2 = 0x700 + i * 0x20;
	backtrace_push(cpu.SPC, cpu.PC, true /* interrupt */);
}

/* Returns the priority level for IRQi */
static int irq_priority(int i)
{
	uint16_t reg;
	int bitoff;

	if (i < 4) {
		reg = intc.IPRC;
		bitoff = 4 * i;
	} else {
		reg = intc.IPRD;
		bitoff = 4 * (i - 4);
	}
	return (reg >> bitoff) & 0x000F;
}

/* Returns the priority level for PINT0-7 (if i == 0) or PINT8-15 (if i == 1) */
static int pint_priority(int i)
{
	if (i == 0)
		return (intc.IPRD >> 12) & 0x000F;
	return (intc.IPRD >> 8) & 0x000F;
}

static void interrupt_check(void)
{
	/*
	 * "Interrupts are accepted during sleep mode even when the BL bit in the
	 * SR register is 1."
	 */
	if (!(cpu.extra_state & EXTRA_POWER_DOWN) && (cpu.SR & SR_BL_BIT))
		return;

	if (!intc.IRR0)
		return;

	/*
	 * We got an interrupt so we wake up. The "+2" means that the exception
	 * handler will return to the next instruction after SLEEP. I don't know
	 * for sure if this is correct (TODO).
	 */
	if (cpu.extra_state & EXTRA_POWER_DOWN) {
		cpu.extra_state &= ~EXTRA_POWER_DOWN;
		cpu.PC += 2;
	}

	/*
	 * When multiple interrupts are set at the same time, I think I should
	 * accept the ones with higher priority first. For now I have neglected
	 * this (TODO), but it probably won't matter much at our speed.
	 */
	if (intc.IRR0 & IRR0_IRQ1R)
		return irq_accept(1, irq_priority(1));
	if (intc.IRR0 & IRR0_IRQ3R)
		return irq_accept(3, irq_priority(3));
	if (intc.IRR0 & IRR0_IRQ4R)
		return irq_accept(4, irq_priority(4));
	if (intc.IRR0 & IRR0_PINT1R)
		return pint_accept(1, pint_priority(1));
}

/* Emulate indefinitely if @steps < 0 */
static int run(int steps)
{
	bool forever = steps < 0;
	int ret;

	while (true) {
		if (!interactive && !script_file) {
			dump_micro();
			printf("\n******\n\n");
			printf("ASM: ");
			disassemble(cpu.PC);
		}

		/* This can change the PC, so do it before the breakpoint check */
		interrupt_check();

		/* TODO: actually sleep instead of looping around interrupt checks */
		if (!(cpu.extra_state & EXTRA_POWER_DOWN)) {
			if (pc_is_breakpoint(cpu.PC))
				return 0;
			ret = execute(cpu.PC);
			if (ret) {
				become_interactive();
				return ret;
			}
			update_clocks();
			update_scif();
			update_top_light();
			if (!forever) {
				if (--steps == 0)
					return 0;
			}
		}

		if (kbinterrupted)
			return 0;
	}
}

#define CLI_CONTINUE	0
#define CLI_EXIT		1

static int exit_command_handler(int argc, const char **argv)
{
	if (argc != 1) {
		printf("Invalid exit command\n");
		return CLI_CONTINUE;
	}
	return CLI_EXIT;
}

static void print_backtrace(void)
{
	struct bt_entry *curr = NULL, *prev = NULL;
	int i;

	if (backtrace.bt_count == 0)
		return;

	backtrace_push(cpu.PC, 0, false /* interrupt */);
	for (i = backtrace.bt_count - 1; i > 0; --i) {
		curr = &backtrace.bt_entries[i];
		prev = &backtrace.bt_entries[i - 1];
		printf("%c[0x%.8x] <0x%.8x>+0x%x\n", prev->interrupt ? '*' : ' ', curr->origin, prev->target, curr->origin - prev->target);
	}
	backtrace_pop();
}

static int backtrace_command_handler(int argc, const char **argv)
{
	if (argc != 1) {
		printf("Invalid backtrace command\n");
		return CLI_CONTINUE;
	}

	if (backtrace.bt_count == 0) {
		printf("Backtrace is empty\n");
		return CLI_CONTINUE;
	}

	print_backtrace();
	return CLI_CONTINUE;
}

/* Stop the execution of a script and become a debug console */
static int stop_command_handler(int argc, const char **argv)
{
	if (argc != 1) {
		printf("Invalid stop command\n");
		return CLI_CONTINUE;
	}
	if (!script_file) {
		printf("Not in execution\n");
		return CLI_CONTINUE;
	}

	become_interactive();
	return CLI_CONTINUE;
}

static int run_command_handler(int argc, const char **argv)
{
	int err;

	if (argc != 1) {
		printf("Invalid run command\n");
		return CLI_CONTINUE;
	}

	err = run(-1 /* steps*/);
	if (err)
		printf("An error was encountered\n");
	return CLI_CONTINUE;
}

static int step_command_handler(int argc, const char **argv)
{
	int stepcount = 1;

	if (argc > 2) {
		printf("Invalid step command\n");
		return CLI_CONTINUE;
	}
	if (argc == 2) {
		stepcount = atoi(argv[1]); /* TODO: don't use atoi? */
		if (stepcount < 1) {
			printf("Invalid step count %d\n", stepcount);
			return CLI_CONTINUE;
		}
	}

	if (run(stepcount))
		printf("An error was encountered\n");
	return CLI_CONTINUE;
}

static int dump_command_handler(int argc, const char **argv)
{
	const char *sel = NULL;

	if (argc == 1) {
		dump_micro();
		return CLI_CONTINUE;
	}

	if (argc > 2) {
		printf("Invalid dump command\n");
		return CLI_CONTINUE;
	}
	sel = argv[1];

	if (strcmp(sel, "cpu") == 0)
		dump_cpu();
	else
		printf("Unsupported dump selector \"%s\"\n", sel);
	return CLI_CONTINUE;
}

static int disas_command_handler(int argc, const char **argv)
{
	uint32_t true_pc = cpu.PC - 4;
	uint32_t length = 0x20;
	char *endptr = NULL;
	unsigned long tmp;

	if (argc > 3) {
		printf("Invalid disassemble command\n");
		return CLI_CONTINUE;
	}
	if (argc >= 2) {
		const char *address = argv[1];

		tmp = strtoul(address, &endptr, 0);
		if (*endptr != '\0' || (tmp == ULONG_MAX && errno == ERANGE)) {
			printf("Bad number \"%s\"\n", address);
			return CLI_CONTINUE;
		}
		if (address[0] == '+' || address[0] == '-')
			true_pc += tmp;
		else
			true_pc = tmp;
		if (true_pc & 1) {
			printf("Bad instruction address 0x%.8x: must be even\n", true_pc);
			return CLI_CONTINUE;
		}
	}
	if (argc == 3) {
		tmp = strtoul(argv[2], &endptr, 0);
		if (*endptr != '\0' || tmp == ULONG_MAX) {
			printf("Bad number \"%s\"\n", argv[2]);
			return CLI_CONTINUE;
		}
		length = tmp;
	}

	while (length--) {
		printf("0x%.8x:\t", true_pc);
		disassemble(true_pc + 4);
		true_pc += 2;
		if (kbinterrupted)
			break;
	}
	return CLI_CONTINUE;
}

static int break_command_handler(int argc, const char **argv)
{
	uint32_t pc;
	char *endptr = NULL;
	unsigned long tmp;

	if (argc != 2) {
		printf("Invalid break command\n");
		return CLI_CONTINUE;
	}

	tmp = strtoul(argv[1], &endptr, 0);
	if (*endptr != '\0' || tmp == ULONG_MAX) {
		printf("Bad number \"%s\"\n", argv[1]);
		return CLI_CONTINUE;
	}
	pc = tmp;

	if (breakpoints.pcs_count == MAX_BREAKPOINTS) {
		printf("Breakpoint limit reached\n");
		return CLI_CONTINUE;
	}
	breakpoints.pcs[breakpoints.pcs_count++] = pc;
	return CLI_CONTINUE;
}

static void do_xxd(uint32_t addr, uint32_t len)
{
	uint32_t end = addr + len;
	uint32_t line_start;
	uint8_t line_bytes[16] = {0};
	int i;

	while (addr < end) {
 		line_start = addr & ~0xFUL;
		printf("%.8x: ", line_start);
		for (i = 0; i < 16; ++i) {
			if (line_start + i < addr || line_start + i >= end) {
				printf("  ");
			} else {
				line_bytes[i] = read_byte(line_start + i);
				printf("%.2x", line_bytes[i]);
			}
			if (i & 1)
				printf(" ");
		}
		printf(" ");
		for (i = 0; i < 16; ++i) {
			if (line_start + i < addr || line_start + i >= end)
				printf(" ");
			else if (isprint(line_bytes[i]))
				printf("%c", line_bytes[i]);
			else
				printf(".");
		}
		printf("\n");
		addr = line_start + 16;
		if (kbinterrupted)
			break;
	}
}

static int xxd_command_handler(int argc, const char **argv)
{
	uint32_t address = cpu.PC - 4;
	uint32_t length = 1;
	char *endptr = NULL;
	unsigned long tmp;

	if (argc > 3) {
		printf("Invalid xxd command\n");
		return CLI_CONTINUE;
	}

	if (argc >= 2) {
		tmp = strtoul(argv[argc - 1], &endptr, 0);
		if (*endptr != '\0' || tmp == ULONG_MAX) {
			printf("Bad number \"%s\"\n", argv[argc - 1]);
			return CLI_CONTINUE;
		}
		length = tmp;
	}
	if (argc == 3) {
		tmp = strtoul(argv[1], &endptr, 0);
		if (*endptr != '\0' || tmp == ULONG_MAX) {
			printf("Bad number \"%s\"\n", argv[1]);
			return CLI_CONTINUE;
		}
		address = tmp;
	}

	do_xxd(address, length);
	return CLI_CONTINUE;
}

/* Returns negative for invalid numbers */
static int regname_to_number(const char *regname_number)
{
	int numlen, number;

	numlen = strlen(regname_number);
	if (numlen == 0 || numlen > 2)
		return -1;

	if (!isdigit(regname_number[0]))
		return -1;
	number = regname_number[0] - '0';
	if (numlen == 1)
		return number;

	if (!isdigit(regname_number[1]))
		return -1;
	number *= 10;
	number += regname_number[1] - '0';
	return number;
}

/* Returns a pointer to the register in memory, or NULL if invalid */
static void *regname_to_ptr(const char *regname, int *width_p)
{
	char *endptr = NULL;
	unsigned long tmp;
	uint32_t addr;
	int n;

	if (strcmp(regname, "PC") == 0) {
		backtrace_clear();
		*width_p = sizeof(cpu.PC);
		return &cpu.PC;
	}
	if (strcmp(regname, "PR") == 0) {
		backtrace_clear();
		*width_p = sizeof(cpu.PR);
		return &cpu.PR;
	}
	if (strcmp(regname, "EXPEVT") == 0) {
		*width_p = sizeof(cpu.EXPEVT);
		return &cpu.EXPEVT;
	}

	/* TODO: deal with memory mappings? */
	if (strncmp(regname, "w:", 2) == 0) {
		tmp = strtoul(&regname[2], &endptr, 0);
		if (*endptr != '\0' || tmp == ULONG_MAX) {
			printf("Bad address \"%s\"\n", &regname[2]);
			return NULL;
		}
		addr = tmp; /* TODO: check the range before these assignments? */
		addr = p1_p2_to_phys(addr);
		if (addr < MEMORY_OFF || addr > MEMORY_OFF + MEMORY_SIZE - sizeof(uint16_t)) {
			printf("Unsupported memory address \"%s\"\n", &regname[2]);
			return NULL;
		}
		*width_p = sizeof(uint16_t);
		return &memory[addr - MEMORY_OFF];
	}
	if (strncmp(regname, "l:", 2) == 0) {
		tmp = strtoul(&regname[2], &endptr, 0);
		if (*endptr != '\0' || tmp == ULONG_MAX) {
			printf("Bad address \"%s\"\n", &regname[2]);
			return NULL;
		}
		addr = tmp; /* TODO: check the range before these assignments? */
		addr = p1_p2_to_phys(addr);
		if (addr < MEMORY_OFF || addr > MEMORY_OFF + MEMORY_SIZE + sizeof(uint32_t)) {
			printf("Unsupported memory address \"%s\"\n", &regname[2]);
			return NULL;
		}
		*width_p = sizeof(uint32_t);
		return &memory[addr - MEMORY_OFF];
	}

	/* Assume BANK1 for now */
	if (regname[0] == 'R') {
		n = regname_to_number(&regname[1]);
		if (n >= 0) {
			if (n < 8) {
				*width_p = sizeof(cpu.R_BANK1[n]);
				return &cpu.R_BANK1[n];
			} else if (n < 16) {
				*width_p = sizeof(cpu.R[n]);
				return &cpu.R[n];
			}
		}
	}

	/*
	 * A port control register set by the debugger only affects future behaviour
	 * of I/O to the port register. Nothing happens right away.
	 */
	if (strcmp(regname, "PJCR") == 0) {
		*width_p = sizeof(uint16_t);
		return pfc_regs + (PFC_PJCR_OFF - PFC_REGS_OFF);
	}

	return NULL;
}

static int set_addr_to_value(void *addr, int width, const char *value)
{
	char *endptr = NULL;
	unsigned long tmp;

	tmp = strtoul(value, &endptr, 0);
	if (*endptr != '\0' || (tmp == ULONG_MAX && errno == ERANGE)) {
		/* TODO: always check errno, even when I don't care about -1? */
		printf("Bad number \"%s\"\n", value);
		return CLI_CONTINUE;
	}
	if (value[0] == '+' || value[0] == '-') {
		if (width == sizeof(uint16_t))
			*(uint16_t *)addr += tmp;
		else if (width == sizeof(uint32_t))
			*(uint32_t *)addr += tmp;
	} else {
		if (width == sizeof(uint16_t))
			*(uint16_t *)addr = tmp;
		else if (width == sizeof(uint32_t))
			*(uint32_t *)addr = tmp;
	}

	if (addr == &cpu.PC)
		backtrace_push(0, cpu.PC, false /* interrupt */);
	return CLI_CONTINUE;
}

/*
 * Only works with a few registers for now. The point is to run dll functions
 * so that I have something I can test.
 */
static int set_command_handler(int argc, const char **argv)
{
	void *reg = NULL;
	int reg_width;

	if (argc != 3) {
		printf("Invalid set command\n");
		return CLI_CONTINUE;
	}

	reg = regname_to_ptr(argv[1], &reg_width);
	if (!reg) {
		printf("Unknown register \"%s\"\n", argv[1]);
		return CLI_CONTINUE;
	}
	if (reg_width == 1) {
		printf("8-bit registers not supported for set\n");
		return CLI_CONTINUE;
	}
	return set_addr_to_value(reg, reg_width, argv[2]);
}

static enum pin_sense_mode button_to_mode(enum button button)
{
	int mode;

	/* TODO: maybe use the names from the manual for the bits? */
	switch (button) {
	case BUTTON_ONOFF:
		mode = (intc.ICR1 >> 2) & 0x0003;
		break;
	case BUTTON_QL1:
	case BUTTON_QL2:
	case BUTTON_QL3:
	case BUTTON_QL4:
		mode = (intc.ICR1 >> 8) & 0x0003;
		break;
	case BUTTON_PEN:
		mode = (intc.ICR1 >> 6) & 0x0003;
		break;
	case BUTTON_EXIT:
		return low + ((intc.ICR2 >> 12) & 0x0001);
	case BUTTON_RECORD:
		return low + ((intc.ICR2 >> 11) & 0x0001);
	case BUTTON_ENTER:
		return low + ((intc.ICR2 >> 10) & 0x0001);
	case BUTTON_DOWN:
		return low + ((intc.ICR2 >> 9) & 0x0001);
	case BUTTON_UP:
		return low + ((intc.ICR2 >> 8) & 0x0001);
	default:
		/* Bug */
		return 0;
	}

	if (mode < 3)
		return mode;
	/* Panic if 3, bug if it's strictly above 3... */
	panic("Reserved value for sense mode in ICR1\n");
	return 0;
}

static int input_qli_handler(int i, int argc, const char **argv)
{
	enum button btn;
	unsigned int state_flag;
	enum pin_sense_mode mode;
	bool pin_before, pin_after;

	if (argc != 1) {
		printf("Invalid QL%d input\n", i);
		return CLI_CONTINUE;
	}

	/* Not happy about any of this, share more code for all buttons (TODO) */
	btn = BUTTON_QL1 + (i - 1);
	state_flag = 1U << btn;

	/*
	 * I think the shared pin for this board actually goes high when a button
	 * is pushed, otherwise the handler at <0x80036D6C> would not be able to
	 * check the button pins in time. Not sure though... TODO
	 */
	pin_before = button_state & BUTTON_QLX_MASK;
	button_state ^= state_flag;
	pin_after = button_state & BUTTON_QLX_MASK;

	/*
	 * The interrupt may be triggered on low level, or on either edge. In the
	 * first case, it will remain set for as long as the button is pushed. In
	 * the second case, the interrupt handler is responsible for unsetting it.
	 */
	mode = button_to_mode(btn);
	if (!pin_before && pin_after) {
		if (mode == rising)
			intc.IRR0 |= IRR0_IRQ4R;
		if (mode == low)
			intc.IRR0 &= ~IRR0_IRQ4R;
	} else if (pin_before && !pin_after) {
		if (mode == falling || mode == low)
			intc.IRR0 |= IRR0_IRQ4R;
	}

	/*
	 * These four port pins are one for each button. The IRQ4 handler at
	 * <0x80036D6C> checks them in order, and the first one to be unset is the
	 * one that it handles. So you can't tell from that code what would happen
	 * if two were pushed at the same time, but I assume that both pins would be
	 * unset. TODO: see if some other handler actually supports multiple pushes
	 * and can confirm this.
	 */

	return CLI_CONTINUE;
}

static int input_ql1_handler(int argc, const char **argv)
{
	return input_qli_handler(1, argc, argv);
}

static int input_ql2_handler(int argc, const char **argv)
{
	return input_qli_handler(2, argc, argv);
}

static int input_ql3_handler(int argc, const char **argv)
{
	return input_qli_handler(3, argc, argv);
}

static int input_ql4_handler(int argc, const char **argv)
{
	return input_qli_handler(4, argc, argv);
}

static int input_onoff_handler(int argc, const char **argv)
{
	enum pin_sense_mode mode;

	if (argc != 1) {
		printf("Invalid onoff input\n");
		return CLI_CONTINUE;
	}

	/*
	 * The interrupt may be triggered on low level, or on either edge. In the
	 * first case, it will remain set for as long as the button is pushed. In
	 * the second case, the interrupt handler is responsible for unsetting it.
	 */
	mode = button_to_mode(BUTTON_ONOFF);
	if (button_state & BUTTON_ONOFF_PUSHED) {
		button_state &= ~BUTTON_ONOFF_PUSHED;
		if (mode == rising)
			intc.IRR0 |= IRR0_IRQ1R;
		if (mode == low)
			intc.IRR0 &= ~IRR0_IRQ1R;
	} else {
		button_state |= BUTTON_ONOFF_PUSHED;
		if (mode == falling || mode == low)
			intc.IRR0 |= IRR0_IRQ1R;
	}

	return CLI_CONTINUE;
}

/* The exit button uses the PINT12 interrupt pin */
static int input_exit_handler(int argc, const char **argv)
{
	if (argc != 1) {
		printf("Invalid exit input\n");
		return CLI_CONTINUE;
	}

	button_state ^= BUTTON_EXIT_PUSHED;
	pint_refresh();
	return CLI_CONTINUE;
}

/* The record button uses the PINT11 interrupt pin */
static int input_record_handler(int argc, const char **argv)
{
	if (argc != 1) {
		printf("Invalid record input\n");
		return CLI_CONTINUE;
	}

	button_state ^= BUTTON_RECORD_PUSHED;
	pint_refresh();
	return CLI_CONTINUE;
}

/* The enter button uses the PINT10 interrupt pin */
static int input_enter_handler(int argc, const char **argv)
{
	if (argc != 1) {
		printf("Invalid enter input\n");
		return CLI_CONTINUE;
	}

	button_state ^= BUTTON_ENTER_PUSHED;
	pint_refresh();
	return CLI_CONTINUE;
}

/* Pushing the wheel downwards uses the PINT9 interrupt pin */
static int input_down_handler(int argc, const char **argv)
{
	if (argc != 1) {
		printf("Invalid down input\n");
		return CLI_CONTINUE;
	}

	button_state ^= BUTTON_DOWN_PUSHED;
	pint_refresh();
	return CLI_CONTINUE;
}

/* Pushing the wheel upwards uses the PINT8 interrupt pin */
static int input_up_handler(int argc, const char **argv)
{
	if (argc != 1) {
		printf("Invalid up input\n");
		return CLI_CONTINUE;
	}

	button_state ^= BUTTON_UP_PUSHED;
	pint_refresh();
	return CLI_CONTINUE;
}

/* Returns 0 on success, -1 on failure */
static int touchscreen_read_coordinates(const char *xy)
{
	int *curr = NULL;
	int x = -1, y = -1;
	int i;

	curr = &x;

	/* 9 is just an arbitrary limit */
	for (i = 0; i < 9; ++i) {
		if (xy[i] == '\0')
			break;
		if (isdigit(xy[i])) {
			if (*curr == -1)
				*curr = 0;
			*curr *= 10;
			*curr += xy[i] - '0';
			continue;
		}
		if (curr == &x && xy[i] == ',') {
			curr = &y;
			continue;
		}
		break;
	}
	if (xy[i] != '\0' || x < 0 || y < 0)
		return -1;

	touchscreen.x = x;
	touchscreen.y = y;
	return 0;
}

static int input_pen_handler(int argc, const char **argv)
{
	const char *pos = NULL;
	enum pin_sense_mode mode;

	if (argc != 2) {
		printf("Invalid pen input: coordinates missing\n");
		return CLI_CONTINUE;
	}

	pos = argv[1];
	if (strcmp(pos, "up") == 0) {
		touchscreen.x = -1;
		touchscreen.y = -1;
	} else if (touchscreen_read_coordinates(pos)) {
		printf("Invalid pen input \"%s\"\n", pos);
		return CLI_CONTINUE;
	}

	/* The only mode in the selftests, but surely others will show up (TODO) */
	mode = button_to_mode(BUTTON_PEN);
	if (mode != rising) {
		panic("Unsupported sense mode for touchscreen interrupts (%u)\n", mode);
		return CLI_CONTINUE;
	}

	if (touchscreen.x == -1)
		return CLI_CONTINUE;
	intc.IRR0 |= IRR0_IRQ3R;
	return CLI_CONTINUE;
}

struct shell_command {
	const char *name;
	const char *usage;
	int (*handler)(int argc, const char **argv);
};

/* TODO: some way to list the input commands */
struct shell_command input_command_list[] = {
	{"onoff", "onoff", input_onoff_handler},
	{"QL1", "QL1", input_ql1_handler},
	{"QL2", "QL2", input_ql2_handler},
	{"QL3", "QL3", input_ql3_handler},
	{"QL4", "QL4", input_ql4_handler},
	{"exit", "exit", input_exit_handler},
	{"record", "record", input_record_handler},
	{"enter", "enter", input_enter_handler},
	{"down", "down", input_down_handler},
	{"up", "up", input_up_handler},
	/*
	 * The buttons self-test at <0x800396E0> also lists a supposed "Start"
	 * button that doesn't seem to have handler. I think we've covered all the
	 * physical buttons, so maybe it's just dead code.
	 */
	{"pen", "pen [x,y|up]", input_pen_handler},
};

static int input_command_handler(int argc, const char **argv)
{
	const char *command = NULL;
	int i, command_count;

	if (argc < 2) {
		printf("Invalid input command\n");
		return CLI_CONTINUE;
	}
	command = argv[1];

	/* TODO: reuse the code from the regular shell commands somehow */
	command_count = sizeof(input_command_list) / sizeof(input_command_list[0]);
	for (i = 0; i < command_count; ++i) {
		if (strcmp(command, input_command_list[i].name) == 0)
			return input_command_list[i].handler(argc - 1, argv + 1);
	}
	printf("Invalid input \"%s\"\n", command);
	return CLI_CONTINUE;
}

/*
 * Create a fake virtual memory mapping, to be able to run dll functions before
 * setting up the mmu. TODO: prevent overlapping mappings?
 */
static int map_command_handler(int argc, const char **argv)
{
	struct vm_mapping *map = NULL;
	uint32_t virt, phys, len;
	char *endptr = NULL;
	unsigned long tmp;

	if (argc != 4) {
		printf("Invalid map command\n");
		return CLI_CONTINUE;
	}

	tmp = strtoul(argv[1], &endptr, 0);
	if (*endptr != '\0' || tmp == ULONG_MAX) {
		printf("Bad number \"%s\"\n", argv[1]);
		return CLI_CONTINUE;
	}
	virt = tmp;

	tmp = strtoul(argv[2], &endptr, 0);
	if (*endptr != '\0' || tmp == ULONG_MAX) {
		printf("Bad number \"%s\"\n", argv[2]);
		return CLI_CONTINUE;
	}
	phys = tmp;

	tmp = strtoul(argv[3], &endptr, 0);
	if (*endptr != '\0' || tmp == ULONG_MAX) {
		printf("Bad number \"%s\"\n", argv[3]);
		return CLI_CONTINUE;
	}
	len = tmp;

	if (virt + len < virt || phys + len < phys) {
		printf("Mapping is too long\n");
		return CLI_CONTINUE;
	}

	if (vm_mappings.map_count == MAX_MAPPINGS) {
		printf("Mapping count limit reached\n");
		return CLI_CONTINUE;
	}
	map = &vm_mappings.maps[vm_mappings.map_count++];
	map->virt = virt;
	map->phys = phys;
	map->len = len;
	return CLI_CONTINUE;
}

static uint32_t crc32(uint8_t *input, long length);

static int crc_command_handler(int argc, const char **argv)
{
	const char *target = NULL;
	static uint8_t secbuf[CF_SECTOR_SZ];
	char *endptr = NULL;
	unsigned long secnum;
	size_t ret;

	if (argc != 3) {
		printf("Invalid crc command: target missing\n");
		return CLI_CONTINUE;
	}
	target = argv[1];
	if (strcmp(target, "card") != 0) {
		printf("Invalid crc target \"%s\"\n", target);
		return CLI_CONTINUE;
	}
	secnum = strtoul(argv[2], &endptr, 0);
	if (*endptr != '\0' || secnum == ULONG_MAX) {
		printf("Bad number \"%s\"\n", argv[2]);
		return CLI_CONTINUE;
	}

	if (!card_file) {
		printf("No CompactFlash card available\n");
		return CLI_CONTINUE;
	}
	if (secnum >= 1U << 28) {
		printf("Sector number too large for CompactFlash\n");
		return CLI_CONTINUE;
	}

	if (fseek(card_file, secnum << CF_SECTOR_SZ_SHIFT, SEEK_SET)) {
		printf("Failed fseek() on card file (%s)\n", strerror(errno));
		return CLI_CONTINUE;
	}
	ret = fread(secbuf, 1, sizeof(secbuf), card_file);
	if (ret != sizeof(secbuf)) {
		if (ferror(card_file))
			printf("Failed to read from card file\n");
		else
			printf("Out-of-bounds CompactFlash read (sector: %lu)\n", secnum);
		return CLI_CONTINUE;
	}

	printf("Card sector %lu CRC: %.8x\n", secnum, crc32(secbuf, sizeof(secbuf)));
	return CLI_CONTINUE;
}

static int print_command_handler(int argc, const char **argv)
{
	FILE *file = NULL;
	const char *path = NULL;
	size_t ret;

	if (argc == 2) {
		path = argv[1];
	} else {
		printf("Invalid print command: target file missing\n");
		return CLI_CONTINUE;
	}

	if (strcmp(path, "CRC") == 0) {
		printf("Display CRC: %.8x\n", crc32(display.fb, DISPLAY_FB_SIZE));
		return CLI_CONTINUE;
	}

	file = fopen(path, "wb");
	if (!file) {
		perror("print");
		return CLI_CONTINUE;
	}

	ret = fwrite(display.fb, 1, DISPLAY_FB_SIZE, file);
	if (ret != DISPLAY_FB_SIZE) {
		printf("print: write failed\n");
		fclose(file);
		return CLI_CONTINUE;
	}

	if (fclose(file)) {
		perror("print");
		return CLI_CONTINUE;
	}
	return CLI_CONTINUE;
}

static int patch_command_handler(int argc, const char **argv)
{
	struct patch *p = NULL;
	uint32_t addr;
	uint16_t insn;
	char *endptr = NULL;
	unsigned long tmp;
	int i;

	if (argc != 3) {
		printf("Invalid patch command\n");
		return CLI_CONTINUE;
	}

	tmp = strtoul(argv[1], &endptr, 0);
	if (*endptr != '\0' || tmp == ULONG_MAX) {
		printf("Bad number \"%s\"\n", argv[1]);
		return CLI_CONTINUE;
	}
	addr = tmp;
	if (addr & 1) {
		printf("Bad instruction address 0x%.8x: must be even\n", addr);
		return CLI_CONTINUE;
	}

	tmp = strtoul(argv[2], &endptr, 0);
	if (*endptr != '\0' || tmp == ULONG_MAX) {
		printf("Bad number \"%s\"\n", argv[2]);
		return CLI_CONTINUE;
	}
	insn = tmp;
	if (tmp != insn) {
		printf("Instruction is too long\n");
		return CLI_CONTINUE;
	}

	for (i = 0; i < patches.patch_count; ++i) {
		p = &patches.p[i];
		if (addr == p->addr) {
			p->insn = insn;
			return CLI_CONTINUE;
		}
	}
	if (patches.patch_count == MAX_PATCHES) {
		printf("Patch limit reached\n");
		return CLI_CONTINUE;
	}
	p = &patches.p[patches.patch_count++];
	p->addr = addr;
	p->insn = insn;
	return CLI_CONTINUE;
}

/* Parses a "\xHH" character. Returns 0 on success, -1 otherwise. */
static int hex_to_char(const char *hex, char *result)
{
	char c;
	int i;

	if (*hex++ != '\\')
		return -1;
	if (*hex++ != 'x')
		return -1;

	c = 0;
	for (i = 0; i < 2; ++i) {
		c <<= 4;
		if (!isxdigit(hex[i]))
			return -1;
		if (isdigit(hex[i]))
			c += hex[i] - '0';
		else if (islower(hex[i]))
			c += hex[i] - 'a' + 10;
		else
			c += hex[i] - 'A' + 10;
	}

	*result = c;
	return 0;
}

static void scif_receive_enqueue(char c)
{
	if (scif.rx_queue_cnt == SCIF_RX_QUEUE_SIZE) {
		printf("Character 0x%.2x dropped from serial input\n", c);
		return;
	}
	/* TODO: write tests for full buffer */
	scif.rx_queue[scif.rx_queue_end] = c;
	scif.rx_queue_end = (scif.rx_queue_end + 1) % SCIF_RX_QUEUE_SIZE;
	scif.rx_queue_cnt++;
}

/* Sends input to the serial console. "\xHH" format is supported. */
static int serialin_command_handler(int argc, const char **argv)
{
	int argidx;
	const char *curr_p;
	char hexchar;

	if (argc < 2) {
		printf("Invalid serialin command\n");
		return CLI_CONTINUE;
	}

	for (argidx = 1; argidx < argc; ++argidx) {
		if (argidx != 1)
			scif_receive_enqueue(' ');
		curr_p = argv[argidx];
		while (*curr_p) {
			if (hex_to_char(curr_p, &hexchar) == 0) {
				scif_receive_enqueue(hexchar);
				curr_p += 4;
			} else {
				scif_receive_enqueue(*curr_p);
				curr_p += 1;
			}
		}
	}
	return CLI_CONTINUE;
}

static int help_command_handler(int argc, const char **argv);

struct shell_command shell_command_list[] = {
	{"backtrace", "backtrace", backtrace_command_handler},
	{"break", "break address", break_command_handler},
	{"crc", "crc card sector", crc_command_handler},
	{"disas", "disas [[+|-]address [lenght]]", disas_command_handler},
	{"dump", "dump [cpu]", dump_command_handler},
	{"exit", "exit", exit_command_handler},
	{"help", "help [command]", help_command_handler},
	{"input", "input [button]", input_command_handler},
	{"map", "map virtual_address physical_address length", map_command_handler},
	{"patch", "patch address instruction", patch_command_handler},
	{"print", "print [output_file]", print_command_handler},
	{"run", "run", run_command_handler},
	{"serialin", "serialin input", serialin_command_handler},
	{"set", "set register_name [+|-]value", set_command_handler},
	{"step", "step [count]", step_command_handler},
	{"stop", "stop", stop_command_handler},
	{"xxd", "xxd [address] length", xxd_command_handler},
};

static int help_command_handler(int argc, const char **argv)
{
	int i, command_count;

	command_count = sizeof(shell_command_list) / sizeof(shell_command_list[0]);

	if (argc == 1) {
		for (i = 0; i < command_count; ++i)
			printf("%s -- %s\n", shell_command_list[i].name, shell_command_list[i].usage);
		return CLI_CONTINUE;
	}

	if (argc == 2) {
		for (i = 0; i < command_count; ++i) {
			if (strcmp(argv[1], shell_command_list[i].name) == 0) {
				printf("usage: %s\n", shell_command_list[i].usage);
				return CLI_CONTINUE;
			}
		}
		printf("Invalid command \"%s\"\n", argv[1]);
		return CLI_CONTINUE;
	}

	printf("Invalid help command\n");
	return CLI_CONTINUE;
}

#define MAX_ARGC	32

/* Returns CLI_CONTINUE if @line is empty */
static int assemble_argv(char *line, int *argc_p, char **argv)
{
	int argc = 0;

	while (argc < MAX_ARGC) {
		for (; isspace(*line); ++line)
			;
		if (*line == '\0' || *line == '#')
			break;
		argv[argc] = line;
		++argc;
		for (; *line != '\0' && !isspace(*line); ++line)
			;
		if (*line == '\0')
			break;
		*line = '\0';
		++line;
	}
	if (argc == 0)
		return 1;
	*argc_p = argc;
	return 0;
}

static int dispatch_command_line(char *line)
{
	int argc = 0;
	char *argv[MAX_ARGC] = {0};
	int i, command_count;

	if (assemble_argv(line, &argc, argv))
		return CLI_CONTINUE;

	command_count = sizeof(shell_command_list) / sizeof(shell_command_list[0]);
	for (i = 0; i < command_count; ++i) {
		if (strcmp(argv[0], shell_command_list[i].name) == 0)
			return shell_command_list[i].handler(argc, (const char **)argv);
	}
	printf("Invalid command \"%s\"\n", argv[0]);
	return CLI_CONTINUE;
}

static void prompt_loop(void)
{
	FILE *infile = NULL;
	static char line[4096] = {0};
	int ret = CLI_CONTINUE;

	while (ret == CLI_CONTINUE) {
		dump_all_monitors();
		if (kbinterrupted) {
			kbinterrupted = false;
			become_interactive();
		}
		infile = script_file ? script_file : stdin;
		if (interactive) {
			printf("debug> ");
			fflush(stdout);
		}
		if (!fgets(line, sizeof(line), infile)) {
			if (kbinterrupted) {
				puts("");
				continue;
			}
			if (feof(infile))
				exit(0);
			fprintf(stderr, "Failed to read from input\n");
			exit(1);
		}
		ret = dispatch_command_line(line);
	}
}

static int emulate(void)
{
	reset();

	if (interactive || script_file) {
		prompt_loop();
		return 0;
	}
	return run(-1 /* steps */);
}

/*
 * This is apparently called CRC-32/ISO-HDLC. It's intended to match a utility
 * from my system which, according to the man page, "is supplied with the
 * Archive::Zip module for Perl." The implementation is extremely naïve but I
 * don't really need any extra performance.
 *
 * We use long for the length because that's the return type of ftell().
 */
static uint32_t crc32(uint8_t *input, long length)
{
	uint32_t poly, curr;
	uint8_t shifted_bit;
	long i, j;

	poly = 0xEDB88320U;
	curr = 0xFFFFFFFFU;

	for (i = 0; i < length; ++i) {
		curr ^= input[i];
		for (j = 0; j < 8; ++j) {
			shifted_bit = curr & 1;
			curr >>= 1;
			if (shifted_bit)
				curr ^= poly;
		}
	}
	return ~curr;
}
