#include "display.h"
#include "board.h"
#include "esp_rom_sys.h"

// GN1650: I2C-style framing with no address. MSB first. DIO and CLK are
// open-drain with 5.1k pull-ups to +3V3_SW, so "1" means release the line.
#define HALF_US 5

typedef struct {
    gpio_num_t dio, clk;
    uint8_t segs[4];
    uint8_t ctrl;
    bool valid;
    uint32_t naks;
} disp_t;

static disp_t s_disp[2] = {
    {.dio = PIN_DISP_DIO1, .clk = PIN_DISP_CLK1},
    {.dio = PIN_DISP_DIO2, .clk = PIN_DISP_CLK2},
};

static inline void dly(void) { esp_rom_delay_us(HALF_US); }

static void bus_start(disp_t *d)
{
    gpio_set_level(d->dio, 1);
    gpio_set_level(d->clk, 1);
    dly();
    gpio_set_level(d->dio, 0);
    dly();
    gpio_set_level(d->clk, 0);
    dly();
}

static void bus_stop(disp_t *d)
{
    gpio_set_level(d->dio, 0);
    dly();
    gpio_set_level(d->clk, 1);
    dly();
    gpio_set_level(d->dio, 1);
    dly();
}

static bool bus_byte(disp_t *d, uint8_t b)
{
    for (int i = 7; i >= 0; i--) {
        gpio_set_level(d->dio, (b >> i) & 1);
        dly();
        gpio_set_level(d->clk, 1);
        dly();
        gpio_set_level(d->clk, 0);
        dly();
    }
    gpio_set_level(d->dio, 1);  // release for ACK
    dly();
    gpio_set_level(d->clk, 1);
    dly();
    bool ack = gpio_get_level(d->dio) == 0;
    gpio_set_level(d->clk, 0);
    dly();
    return ack;
}

static void bus_cmd(disp_t *d, uint8_t a, uint8_t b)
{
    bus_start(d);
    bool ok = bus_byte(d, a);
    ok &= bus_byte(d, b);
    bus_stop(d);
    if (!ok) d->naks++;
}

void display_init(void)
{
    for (int i = 0; i < 2; i++) {
        disp_t *d = &s_disp[i];
        gpio_config_t io = {
            .pin_bit_mask = (1ULL << d->dio) | (1ULL << d->clk),
            .mode = GPIO_MODE_INPUT_OUTPUT_OD,
            .pull_up_en = GPIO_PULLUP_DISABLE,  // external 5.1k
            .pull_down_en = GPIO_PULLDOWN_DISABLE,
            .intr_type = GPIO_INTR_DISABLE,
        };
        gpio_config(&io);
        gpio_set_level(d->dio, 1);
        gpio_set_level(d->clk, 1);
        d->valid = false;
    }
}

static uint8_t make_ctrl(int brightness)
{
    if (brightness <= 0) return 0x00;  // display off
    if (brightness > 8) brightness = 8;
    // B6..B4 brightness: 001 = 1/8 ... 111 = 7/8, 000 = 8/8. B3 = 0: 8-segment mode (colon works). B0 = on.
    return (uint8_t)(((brightness & 7) << 4) | 0x01);
}

void display_set(int idx, const uint8_t segs[4], int brightness)
{
    disp_t *d = &s_disp[idx];
    uint8_t ctrl = make_ctrl(brightness);
    if (!d->valid || ctrl != d->ctrl) {
        bus_cmd(d, 0x48, ctrl);
        d->ctrl = ctrl;
    }
    for (int i = 0; i < 4; i++) {
        if (!d->valid || segs[i] != d->segs[i]) {
            bus_cmd(d, 0x68 + 2 * i, segs[i]);
            d->segs[i] = segs[i];
        }
    }
    d->valid = true;
}

void display_force_refresh(void)
{
    s_disp[0].valid = false;
    s_disp[1].valid = false;
}

uint32_t display_nak_count(int idx) { return s_disp[idx].naks; }

static const uint8_t k_digits[10] = {0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x6F};

uint8_t display_digit(int d) { return (d >= 0 && d <= 9) ? k_digits[d] : 0; }

// The usual 7-segment letters (segments: 0x01 a top, 0x02 b, 0x04 c, 0x08 d bottom, 0x10 e, 0x20 f, 0x40 g
// middle). Upper and lower case share a glyph unless both shapes read well (C c, H h, O o, U u, I i).
// Some can't be told from others: K looks like an H with a top bar, M is an upside-down U, W is three
// bars, X is H, Z is 2. Anything else is blank.
uint8_t display_char(char c)
{
    if (c >= '0' && c <= '9') return k_digits[c - '0'];
    switch (c) {
    case 'A': case 'a': return 0x77;
    case 'B': case 'b': return 0x7C;
    case 'C': return 0x39;
    case 'c': return 0x58;
    case 'D': case 'd': return 0x5E;
    case 'E': case 'e': return 0x79;
    case 'F': case 'f': return 0x71;
    case 'G': return 0x3D;
    case 'g': return 0x6F;
    case 'H': return 0x76;
    case 'h': return 0x74;
    case 'I': return 0x30;
    case 'i': return 0x10;
    case 'J': case 'j': return 0x1E;
    case 'K': case 'k': return 0x75;
    case 'L': case 'l': return 0x38;
    case 'M': case 'm': return 0x37;
    case 'N': case 'n': return 0x54;
    case 'O': return 0x3F;
    case 'o': return 0x5C;
    case 'P': case 'p': return 0x73;
    case 'Q': case 'q': return 0x67;
    case 'R': case 'r': return 0x50;
    case 'S': case 's': return 0x6D;
    case 'T': case 't': return 0x78;
    case 'U': case 'V': return 0x3E;
    case 'u': case 'v': return 0x1C;
    case 'W': case 'w': return 0x2A;
    case 'X': case 'x': return 0x76;
    case 'Y': case 'y': return 0x6E;
    case 'Z': case 'z': return 0x5B;
    case '-': return 0x40;
    case '_': return 0x08;
    case '=': return 0x48;
    default: return 0x00;
    }
}

void display_text(uint8_t segs[4], const char *s)
{
    for (int i = 0; i < 4; i++) segs[i] = (s && *s) ? display_char(*s++) : 0;
}

void display_number(uint8_t segs[4], int n)
{
    if (n < 0) n = 0;
    if (n > 9999) n = 9999;
    for (int i = 3; i >= 0; i--) {
        segs[i] = (n > 0 || i == 3) ? k_digits[n % 10] : 0;
        n /= 10;
    }
}

void display_colon(uint8_t segs[4], bool on)
{
    if (on) {
        segs[2] |= SEG_COLON;
        segs[3] |= SEG_COLON;
    } else {
        segs[2] &= ~SEG_COLON;
        segs[3] &= ~SEG_COLON;
    }
}
