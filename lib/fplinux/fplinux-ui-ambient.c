/* SPDX-License-Identifier: LGPL-2.1-or-later */
#include "fplinux-ui-ambient.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#define AMBIENT_TRANSITION_MS 350U
#define AMBIENT_MIN_PERIOD_MS 100U
#define AMBIENT_BASE_OPACITY 220U

struct scene_spec {
	enum fplinux_ambient_effect effect;
	uint32_t period_ms;
	uint8_t priority;
	struct fplinux_ambient_rgb zone[FPLINUX_AMBIENT_ZONE_COUNT];
};

/*
 * Cool blue and warm amber are the two shared accents. Scenes only name
 * semantic colours and motion; the renderer decides how they look on the
 * display. Values are 8-bit RGB so the engine stays independent of LVGL.
 */
static const struct scene_spec scenes[FPLINUX_AMBIENT_SCENE_COUNT] = {
	[FPLINUX_AMBIENT_IDLE] = {
		.effect = FPLINUX_AMBIENT_BREATHE,
		.period_ms = 4000,
		.priority = 0,
		.zone = {
			{ 0x18, 0x5a, 0x8c }, { 0x18, 0x5a, 0x8c },
			{ 0x18, 0x5a, 0x8c }, { 0x18, 0x5a, 0x8c },
		},
	},
	[FPLINUX_AMBIENT_ACTIVE] = {
		.effect = FPLINUX_AMBIENT_STATIC,
		.period_ms = 1000,
		.priority = 1,
		.zone = {
			{ 0x00, 0x70, 0xc0 }, { 0x00, 0x70, 0xc0 },
			{ 0xd8, 0x70, 0x18 }, { 0xd8, 0x70, 0x18 },
		},
	},
	[FPLINUX_AMBIENT_CHARGING] = {
		.effect = FPLINUX_AMBIENT_BREATHE,
		.period_ms = 3000,
		.priority = 2,
		.zone = {
			{ 0x2e, 0xa8, 0x64 }, { 0x2e, 0xa8, 0x64 },
			{ 0x2e, 0xa8, 0x64 }, { 0x2e, 0xa8, 0x64 },
		},
	},
	[FPLINUX_AMBIENT_NAVIGATION] = {
		.effect = FPLINUX_AMBIENT_SWEEP,
		.period_ms = 2400,
		.priority = 3,
		.zone = {
			{ 0x8c, 0xd4, 0xf0 }, { 0x8c, 0xd4, 0xf0 },
			{ 0x8c, 0xd4, 0xf0 }, { 0x8c, 0xd4, 0xf0 },
		},
	},
	[FPLINUX_AMBIENT_NOTIFICATION] = {
		.effect = FPLINUX_AMBIENT_PULSE,
		.period_ms = 1200,
		.priority = 4,
		.zone = {
			{ 0x36, 0xb6, 0xc8 }, { 0x36, 0xb6, 0xc8 },
			{ 0x36, 0xb6, 0xc8 }, { 0x36, 0xb6, 0xc8 },
		},
	},
	[FPLINUX_AMBIENT_LOW_BATTERY] = {
		.effect = FPLINUX_AMBIENT_PULSE,
		.period_ms = 1000,
		.priority = 5,
		.zone = {
			{ 0xe8, 0x98, 0x1c }, { 0xe8, 0x98, 0x1c },
			{ 0xe8, 0x98, 0x1c }, { 0xe8, 0x98, 0x1c },
		},
	},
	[FPLINUX_AMBIENT_CALL] = {
		.effect = FPLINUX_AMBIENT_BREATHE,
		.period_ms = 5000,
		.priority = 6,
		.zone = {
			{ 0xb8, 0xe0, 0xf0 }, { 0x18, 0x78, 0xa8 },
			{ 0xb8, 0xe0, 0xf0 }, { 0x18, 0x78, 0xa8 },
		},
	},
	[FPLINUX_AMBIENT_ALERT] = {
		.effect = FPLINUX_AMBIENT_PULSE,
		.period_ms = 700,
		.priority = 7,
		.zone = {
			{ 0xd0, 0x30, 0x30 }, { 0xd0, 0x30, 0x30 },
			{ 0xd0, 0x30, 0x30 }, { 0xd0, 0x30, 0x30 },
		},
	},
	[FPLINUX_AMBIENT_INCOMING_CALL] = {
		.effect = FPLINUX_AMBIENT_PULSE,
		.period_ms = 900,
		.priority = 8,
		.zone = {
			{ 0xe8, 0x78, 0x20 }, { 0x28, 0x88, 0xd8 },
			{ 0xe8, 0x78, 0x20 }, { 0x28, 0x88, 0xd8 },
		},
	},
};

struct fplinux_ambient {
	struct fplinux_ambient_sink sink;
	struct fplinux_ambient_frame frame;
	struct fplinux_ambient_frame last;
	struct fplinux_ambient_rgb from[FPLINUX_AMBIENT_ZONE_COUNT];
	struct fplinux_ambient_rgb override[FPLINUX_AMBIENT_ZONE_COUNT];
	bool overridden[FPLINUX_AMBIENT_ZONE_COUNT];
	enum fplinux_ambient_scene base_scene;
	enum fplinux_ambient_scene scene;
	enum fplinux_ambient_scene trigger;
	enum fplinux_ambient_effect effect;
	uint32_t period_ms;
	uint64_t now_ms;
	uint64_t trigger_until_ms;
	uint64_t transition_start_ms;
	bool enabled;
	bool effect_override;
};

static uint8_t mix(uint8_t from, uint8_t to, uint32_t elapsed, uint32_t total)
{
	uint32_t value;

	if (elapsed >= total)
		return to;
	value = ((uint32_t)from * (total - elapsed) + (uint32_t)to * elapsed +
		 total / 2) /
		total;
	return (uint8_t)value;
}

static uint8_t triangle(uint32_t phase, uint32_t period)
{
	uint32_t half = period / 2;

	if (half == 0)
		return UINT8_MAX;
	if (phase < half)
		return (uint8_t)(80 + (175U * phase) / half);
	return (uint8_t)(255 - (175U * (phase - half)) / half);
}

static uint8_t effect_intensity(const struct fplinux_ambient *ambient,
				unsigned int zone, uint32_t phase)
{
	uint32_t period = ambient->period_ms;
	uint32_t pulse;

	if (period == 0)
		return UINT8_MAX;
	switch (ambient->effect) {
	case FPLINUX_AMBIENT_BREATHE:
		return triangle(phase, period);
	case FPLINUX_AMBIENT_PULSE:
		pulse = period / 6;
		if (pulse == 0)
			pulse = 1;
		return phase < pulse ? UINT8_MAX : 110;
	case FPLINUX_AMBIENT_SWEEP:
		return (phase * FPLINUX_AMBIENT_ZONE_COUNT) / period == zone ?
			       UINT8_MAX :
			       115;
	case FPLINUX_AMBIENT_STATIC:
	default:
		return UINT8_MAX;
	}
}

static bool frame_differs(const struct fplinux_ambient_frame *a,
			  const struct fplinux_ambient_frame *b)
{
	for (unsigned int zone = 0; zone < FPLINUX_AMBIENT_ZONE_COUNT; ++zone) {
		if (a->zone[zone].r != b->zone[zone].r ||
		    a->zone[zone].g != b->zone[zone].g ||
		    a->zone[zone].b != b->zone[zone].b ||
		    a->intensity[zone] != b->intensity[zone])
			return true;
	}
	return a->opacity != b->opacity;
}

struct fplinux_ambient *fplinux_ambient_create(void)
{
	struct fplinux_ambient *ambient = calloc(1, sizeof(*ambient));

	if (!ambient)
		return NULL;
	ambient->enabled = true;
	ambient->base_scene = FPLINUX_AMBIENT_IDLE;
	ambient->scene = FPLINUX_AMBIENT_IDLE;
	ambient->effect = scenes[ambient->scene].effect;
	ambient->period_ms = scenes[ambient->scene].period_ms;
	return ambient;
}

void fplinux_ambient_destroy(struct fplinux_ambient *ambient)
{
	free(ambient);
}

void fplinux_ambient_set_sink(struct fplinux_ambient *ambient,
			      const struct fplinux_ambient_sink *sink)
{
	if (!ambient)
		return;
	if (sink)
		ambient->sink = *sink;
	else
		ambient->sink = (struct fplinux_ambient_sink){ 0 };
}

void fplinux_ambient_set_enabled(struct fplinux_ambient *ambient, bool enabled)
{
	if (ambient)
		ambient->enabled = enabled;
}

bool fplinux_ambient_set_scene(struct fplinux_ambient *ambient,
			       enum fplinux_ambient_scene scene)
{
	if (!ambient || scene < 0 || scene >= FPLINUX_AMBIENT_SCENE_COUNT) {
		errno = EINVAL;
		return false;
	}
	ambient->base_scene = scene;
	return true;
}

bool fplinux_ambient_trigger(struct fplinux_ambient *ambient,
			     enum fplinux_ambient_scene scene,
			     uint32_t duration_ms)
{
	if (!ambient || scene < 0 || scene >= FPLINUX_AMBIENT_SCENE_COUNT) {
		errno = EINVAL;
		return false;
	}
	if (!duration_ms) {
		ambient->trigger = FPLINUX_AMBIENT_SCENE_COUNT;
		ambient->trigger_until_ms = 0;
		return true;
	}
	if (ambient->trigger < FPLINUX_AMBIENT_SCENE_COUNT &&
	    ambient->now_ms < ambient->trigger_until_ms &&
	    scenes[scene].priority < scenes[ambient->trigger].priority)
		return true;
	ambient->trigger = scene;
	ambient->trigger_until_ms = ambient->now_ms + duration_ms;
	return true;
}

bool fplinux_ambient_set_zone(struct fplinux_ambient *ambient,
			      enum fplinux_ambient_zone zone,
			      struct fplinux_ambient_rgb rgb)
{
	if (!ambient || zone < 0 || zone >= FPLINUX_AMBIENT_ZONE_COUNT) {
		errno = EINVAL;
		return false;
	}
	ambient->override[zone] = rgb;
	ambient->overridden[zone] = true;
	return true;
}

void fplinux_ambient_clear_zones(struct fplinux_ambient *ambient)
{
	if (!ambient)
		return;
	memset(ambient->overridden, 0, sizeof(ambient->overridden));
	memset(ambient->override, 0, sizeof(ambient->override));
}

bool fplinux_ambient_set_effect(struct fplinux_ambient *ambient,
				enum fplinux_ambient_effect effect,
				uint32_t period_ms)
{
	if (!ambient || effect < 0 || effect >= FPLINUX_AMBIENT_EFFECT_COUNT ||
	    period_ms < AMBIENT_MIN_PERIOD_MS) {
		errno = EINVAL;
		return false;
	}
	ambient->effect = effect;
	ambient->period_ms = period_ms;
	ambient->effect_override = true;
	return true;
}

void fplinux_ambient_tick(struct fplinux_ambient *ambient, uint64_t now_ms)
{
	enum fplinux_ambient_scene active;
	const struct scene_spec *spec;
	uint32_t phase;
	uint32_t elapsed;

	if (!ambient)
		return;
	ambient->now_ms = now_ms;
	if (!ambient->enabled) {
		memset(&ambient->frame, 0, sizeof(ambient->frame));
		if (frame_differs(&ambient->frame, &ambient->last) &&
		    ambient->sink.apply) {
			ambient->last = ambient->frame;
			ambient->sink.apply(ambient->sink.context,
					    &ambient->frame);
		}
		return;
	}
	if (ambient->trigger < FPLINUX_AMBIENT_SCENE_COUNT &&
	    now_ms >= ambient->trigger_until_ms) {
		ambient->trigger = FPLINUX_AMBIENT_SCENE_COUNT;
		ambient->trigger_until_ms = 0;
	}
	active = ambient->trigger < FPLINUX_AMBIENT_SCENE_COUNT ?
			 ambient->trigger :
			 ambient->base_scene;
	spec = &scenes[active];
	if (active != ambient->scene) {
		for (unsigned int zone = 0; zone < FPLINUX_AMBIENT_ZONE_COUNT;
		     ++zone)
			ambient->from[zone] = ambient->frame.zone[zone];
		ambient->scene = active;
		if (!ambient->effect_override) {
			ambient->effect = spec->effect;
			ambient->period_ms = spec->period_ms;
		}
		ambient->transition_start_ms = now_ms;
	}
	elapsed = now_ms >= ambient->transition_start_ms ?
			  (uint32_t)(now_ms - ambient->transition_start_ms) :
			  0;
	if (elapsed > AMBIENT_TRANSITION_MS)
		elapsed = AMBIENT_TRANSITION_MS;
	phase = (uint32_t)(now_ms % ambient->period_ms);
	ambient->frame.opacity = AMBIENT_BASE_OPACITY;
	for (unsigned int zone = 0; zone < FPLINUX_AMBIENT_ZONE_COUNT; ++zone) {
		struct fplinux_ambient_rgb target;

		target = ambient->overridden[zone] ? ambient->override[zone] :
						     spec->zone[zone];
		ambient->frame.zone[zone].r =
			mix(ambient->from[zone].r, target.r, elapsed,
			    AMBIENT_TRANSITION_MS);
		ambient->frame.zone[zone].g =
			mix(ambient->from[zone].g, target.g, elapsed,
			    AMBIENT_TRANSITION_MS);
		ambient->frame.zone[zone].b =
			mix(ambient->from[zone].b, target.b, elapsed,
			    AMBIENT_TRANSITION_MS);
		ambient->frame.intensity[zone] =
			effect_intensity(ambient, zone, phase);
	}
	if (frame_differs(&ambient->frame, &ambient->last) &&
	    ambient->sink.apply) {
		ambient->last = ambient->frame;
		ambient->sink.apply(ambient->sink.context, &ambient->frame);
	}
}

const struct fplinux_ambient_frame *
fplinux_ambient_frame(const struct fplinux_ambient *ambient)
{
	return ambient ? &ambient->frame : NULL;
}
