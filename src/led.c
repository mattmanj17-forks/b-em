#include "b-em.h"
#include "config.h"
#include "main.h"
#include "led.h"
#include "video_render.h"
#include <allegro5/allegro_font.h>
#include <allegro5/allegro_primitives.h>

#define LED_BOX_WIDTH (64)
#define LED_REGION_HEIGHT (12)

int led_ticks = 0;
int last_led_update_at = -10000;

ALLEGRO_BITMAP *led_bitmap;

static ALLEGRO_FONT *font;
static ALLEGRO_COLOR black;

typedef struct {
    const char *label;
    bool transient;
    int index;
    bool state;
    int turn_off_at;
    char cfgcol[8];
    ALLEGRO_COLOR colour;
} led_details_t;

static led_details_t led_details[LED_MAX] = {
    { /* LED_CASSETTE_MOTOR */ "cassette\nmotor", true,  0, false, 0, "cass"    },
    { /* LED_CAPS_LOCK      */ "caps\nlock",      false, 1, false, 0, "capslk"  },
    { /* LED_SHIFT_LOCK     */ "shift\nlock",     false, 2, false, 0, "shiftlk" },
    { /* LED_DRIVE_0        */ "drive 0",         true,  3, false, 0, "drive0"  },
    { /* LED_DRIVE_1        */ "drive 1",         true,  4, false, 0, "drive1"  },
    { /* LED_HARD_DISK_0    */ "hard\ndisc 0",    true,  5, false, 0, "hd0"     },
    { /* LED_HARD_DISK_1    */ "hard\ndisc 1",    true,  6, false, 0, "hd1"     },
    { /* LED_HARD_DISK_2    */ "hard\ndisc 2",    true,  7, false, 0, "hd2"     },
    { /* LED_HARD_DISK_3    */ "hard\ndisc 3",    true,  8, false, 0, "hd3"     },
    { /* LED_VDFS           */ "VDFS",            true,  9, false, 0, "vdfs"    }
};

static void draw_led(const led_details_t *led_details, bool b)
{
    const int x1 = led_details->index * LED_BOX_WIDTH;
    const int y1 = 0;
    const int led_width = 16;
    const int led_height = 4;
    const int led_x1 = x1 + (LED_BOX_WIDTH - led_width) / 2;
    const int led_y1 = y1 + (LED_REGION_HEIGHT - led_height) / 2;
    assert(led_bitmap);
    al_set_target_bitmap(led_bitmap);
    al_draw_filled_rectangle(led_x1, led_y1, led_x1 + led_width, led_y1 + led_height, b ? led_details->colour : black);
}

static void draw_led_full(const led_details_t *led_details, bool b, ALLEGRO_COLOR bgcol)
{
    const ALLEGRO_COLOR label_colour = al_map_rgb(128, 128, 128);
    const int text_region_height = LED_BOX_HEIGHT - LED_REGION_HEIGHT;
    const int x1 = led_details->index * LED_BOX_WIDTH;
    const int y1 = 0;
    const char *label = led_details->label;
    assert(led_bitmap);
    al_set_target_bitmap(led_bitmap);
    al_draw_filled_rectangle(x1, y1, x1 + LED_BOX_WIDTH - 1, y1 + LED_BOX_HEIGHT - 1, bgcol);
    draw_led(led_details, b);
    const char *label_newline = strchr(label, '\n');
    if (!label_newline) {
        const int text_height = al_get_font_ascent(font);
        const int text_y1 = y1 + LED_REGION_HEIGHT + (text_region_height - text_height) / 2;
        al_draw_text(font, label_colour, x1 + LED_BOX_WIDTH / 2, text_y1, ALLEGRO_ALIGN_CENTRE, label);
    }
    else {
        char *label1 = malloc(label_newline - label + 1);
        memcpy(label1, label, label_newline - label);
        label1[label_newline - label] = '\0';
        const char *label2 = label_newline + 1;
        const int text_height = al_get_font_line_height(font);
        const int line_space = 2;
        const int text_y1 = y1 + LED_REGION_HEIGHT + (text_region_height - 2 * text_height - line_space) / 2;
        al_draw_text(font, label_colour, x1 + LED_BOX_WIDTH / 2, text_y1, ALLEGRO_ALIGN_CENTRE, label1);
        al_draw_text(font, label_colour, x1 + LED_BOX_WIDTH / 2, text_y1 + text_height + line_space, ALLEGRO_ALIGN_CENTRE, label2);
    }
}

void led_init(void)
{
    const int led_count = sizeof(led_details) / sizeof(led_details[0]);
    al_init_primitives_addon();
    al_init_font_addon();
    if ((led_bitmap = al_create_bitmap(led_count * LED_BOX_WIDTH, LED_BOX_HEIGHT))) {
        al_set_target_bitmap(led_bitmap);
        al_clear_to_color(al_map_rgb(0, 0, 64));
        if ((font = al_create_builtin_font())) {
            ALLEGRO_COLOR dcol = get_config_colour("leds", "default", al_map_rgb(255, 0, 0));
            ALLEGRO_COLOR bgcol = get_config_colour("leds", "bgcol", al_map_rgb(64,64,64));
            black = al_map_rgb(0, 0, 0);
            for (int i = 0; i < led_count; i++) {
                led_details[i].colour = get_config_colour("leds", led_details[i].cfgcol, dcol);
                draw_led_full(&led_details[i], false, bgcol);
                led_details[i].state = false;
            }
            return;
        }
    }
    vid_ledlocation = LED_LOC_UNDEFINED;
}

void led_close(void)
{
    if (led_bitmap)
        al_destroy_bitmap(led_bitmap);
    if (font)
        al_destroy_font(font);
}

void led_update(led_name_t led_name, bool b, int ticks)
{
    if (vid_ledlocation > LED_LOC_NONE && led_name < LED_MAX) {
        if (b != led_details[led_name].state) {
            draw_led(&led_details[led_name], b);
            last_led_update_at = framesrun;
            led_details[led_name].state = b;
        }
        if (!b || (ticks == 0))
            led_details[led_name].turn_off_at = 0;
        else {
            led_details[led_name].turn_off_at = framesrun + ticks;
            if ((led_ticks == 0) || (ticks < led_ticks))
                led_ticks = ticks;
        }
    }
}

void led_timer_fired(void)
{
    if (vid_ledlocation > LED_LOC_NONE) {
        for (int i = 0; i < sizeof(led_details)/sizeof(led_details[0]); i++) {
            if (led_details[i].turn_off_at != 0) {
                if (framesrun >= led_details[i].turn_off_at) {
                    led_update(i, false, 0);
                    if (led_details[i].state != false) {
                        last_led_update_at = framesrun;
                        led_details[i].state = false;
                        draw_led(&led_details[i], false);
                    }
                    led_details[i].turn_off_at = 0;
                }
                else {
                    int ticks = led_details[i].turn_off_at - framesrun;
                    assert(ticks > 0);
                    if ((led_ticks == 0) || (ticks < led_ticks))
                        led_ticks = ticks;
                }
            }
        }
    }
}

bool led_any_transient_led_on(void)
{
    for (int i = 0; i < sizeof(led_details)/sizeof(led_details[0]); i++)
        if (led_details[i].transient && led_details[i].state)
            return true;
    return false;
}
