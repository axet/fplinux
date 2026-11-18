/* SPDX-License-Identifier: GPL-2.0-only */
/* Host consumer of the LVGL-independent ambient engine. */
#include "fplinux-ui-ambient.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

struct capture {
	unsigned int applies;
	struct fplinux_ambient_frame frame;
};

static void capture_apply(void *context,
			  const struct fplinux_ambient_frame *frame)
{
	struct capture *capture = context;

	++capture->applies;
	capture->frame = *frame;
}

static void print_rgb(const struct fplinux_ambient_rgb *rgb)
{
	printf("{\"r\":%u,\"g\":%u,\"b\":%u}", (unsigned int)rgb->r,
	       (unsigned int)rgb->g, (unsigned int)rgb->b);
}

static void print_zone(const struct fplinux_ambient_frame *frame,
		       enum fplinux_ambient_zone zone)
{
	printf("{\"colour\":");
	print_rgb(&frame->zone[zone]);
	printf(",\"intensity\":%u}", (unsigned int)frame->intensity[zone]);
}

int main(void)
{
	struct fplinux_ambient_sink sink = { .apply = capture_apply };
	struct capture capture = { 0 };
	struct fplinux_ambient *ambient;
	const struct fplinux_ambient_frame *frame;
	bool invalid_scene;
	bool invalid_zone;
	bool invalid_effect;

	sink.context = &capture;
	ambient = fplinux_ambient_create();
	if (!ambient)
		return 2;
	fplinux_ambient_set_sink(ambient, &sink);
	fplinux_ambient_set_scene(ambient, FPLINUX_AMBIENT_IDLE);
	fplinux_ambient_tick(ambient, 0);
	fplinux_ambient_tick(ambient, 400);
	frame = fplinux_ambient_frame(ambient);
	printf("{\"idle\":");
	print_zone(frame, FPLINUX_AMBIENT_TOP);
	fplinux_ambient_tick(ambient, 2000);
	printf(",\"idle_late_intensity\":%u",
	       (unsigned int)fplinux_ambient_frame(ambient)
		       ->intensity[FPLINUX_AMBIENT_TOP]);

	fplinux_ambient_trigger(ambient, FPLINUX_AMBIENT_NOTIFICATION, 2000);
	fplinux_ambient_tick(ambient, 2400);
	fplinux_ambient_tick(ambient, 2800);
	printf(",\"notification\":");
	print_zone(fplinux_ambient_frame(ambient), FPLINUX_AMBIENT_TOP);

	fplinux_ambient_trigger(ambient, FPLINUX_AMBIENT_LOW_BATTERY, 2000);
	fplinux_ambient_tick(ambient, 3200);
	fplinux_ambient_tick(ambient, 3600);
	printf(",\"low_battery\":");
	print_zone(fplinux_ambient_frame(ambient), FPLINUX_AMBIENT_TOP);

	fplinux_ambient_trigger(ambient, FPLINUX_AMBIENT_NOTIFICATION, 500);
	fplinux_ambient_tick(ambient, 4000);
	printf(",\"lower_ignored\":");
	print_zone(fplinux_ambient_frame(ambient), FPLINUX_AMBIENT_TOP);

	fplinux_ambient_tick(ambient, 4800);
	fplinux_ambient_tick(ambient, 5200);
	printf(",\"expired\":");
	print_zone(fplinux_ambient_frame(ambient), FPLINUX_AMBIENT_TOP);

	fplinux_ambient_set_zone(ambient, FPLINUX_AMBIENT_TOP,
				 (struct fplinux_ambient_rgb){ 1, 2, 3 });
	fplinux_ambient_tick(ambient, 5600);
	printf(",\"override\":");
	print_zone(fplinux_ambient_frame(ambient), FPLINUX_AMBIENT_TOP);
	fplinux_ambient_clear_zones(ambient);
	fplinux_ambient_tick(ambient, 5700);

	fplinux_ambient_set_enabled(ambient, false);
	fplinux_ambient_tick(ambient, 5800);
	printf(",\"disabled\":");
	print_zone(fplinux_ambient_frame(ambient), FPLINUX_AMBIENT_TOP);
	printf(",\"disabled_opacity\":%u",
	       (unsigned int)fplinux_ambient_frame(ambient)->opacity);

	invalid_scene = !fplinux_ambient_set_scene(
		ambient, (enum fplinux_ambient_scene) - 1);
	invalid_zone = !fplinux_ambient_set_zone(
		ambient, (enum fplinux_ambient_zone) - 1,
		(struct fplinux_ambient_rgb){ 0, 0, 0 });
	invalid_effect = !fplinux_ambient_set_effect(
		ambient, FPLINUX_AMBIENT_STATIC, 50);
	printf(",\"invalid\":[%s,%s,%s]", invalid_scene ? "true" : "false",
	       invalid_zone ? "true" : "false",
	       invalid_effect ? "true" : "false");
	printf(",\"applies\":%u,\"capture_applies\":%u}\n", capture.applies,
	       capture.applies);
	fplinux_ambient_destroy(ambient);
	return 0;
}
