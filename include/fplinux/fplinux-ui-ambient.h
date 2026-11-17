/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_UI_AMBIENT_H
#define FPLINUX_UI_AMBIENT_H

#include <stdbool.h>
#include <stdint.h>

struct fplinux_ui;

/*
 * Application-controlled ambient accents. This is a UI effect on the LVGL
 * top layer, not a hardware light or a backlight setting. The engine keeps
 * the scene, transitions and timing; the runtime draws the four zones as
 * thin strips at the display edges. Apps use scenes instead of setting
 * colours directly so that later visual changes stay shared.
 *
 * The runtime renderer is off by default. An application opting in calls
 * fplinux_ui_set_ambient_enabled() before using the shared engine; until
 * then fplinux_ui_ambient() returns NULL. The standalone engine functions
 * remain usable without a UI.
 */

enum fplinux_ambient_zone {
	FPLINUX_AMBIENT_TOP,
	FPLINUX_AMBIENT_RIGHT,
	FPLINUX_AMBIENT_BOTTOM,
	FPLINUX_AMBIENT_LEFT,
	FPLINUX_AMBIENT_ZONE_COUNT,
};

/*
 * Semantic scenes, ordered by their own priority. A transient trigger with a
 * higher priority replaces a lower one; a lower one is ignored until the
 * active trigger expires.
 */
enum fplinux_ambient_scene {
	FPLINUX_AMBIENT_IDLE,
	FPLINUX_AMBIENT_ACTIVE,
	FPLINUX_AMBIENT_CHARGING,
	FPLINUX_AMBIENT_NAVIGATION,
	FPLINUX_AMBIENT_NOTIFICATION,
	FPLINUX_AMBIENT_LOW_BATTERY,
	FPLINUX_AMBIENT_CALL,
	FPLINUX_AMBIENT_ALERT,
	FPLINUX_AMBIENT_INCOMING_CALL,
	FPLINUX_AMBIENT_SCENE_COUNT,
};

enum fplinux_ambient_effect {
	FPLINUX_AMBIENT_STATIC,
	FPLINUX_AMBIENT_BREATHE,
	FPLINUX_AMBIENT_PULSE,
	FPLINUX_AMBIENT_SWEEP,
	FPLINUX_AMBIENT_EFFECT_COUNT,
};

struct fplinux_ambient_rgb {
	uint8_t r, g, b;
};

/*
 * One computed frame. zone[] is the scene colour after the scene transition;
 * intensity[] is the effect's 0..255 level for that zone. A renderer may use
 * it for opacity. opacity is the renderer's base opacity. The frame is
 * borrowed and changes on the next tick.
 */
struct fplinux_ambient_frame {
	struct fplinux_ambient_rgb zone[FPLINUX_AMBIENT_ZONE_COUNT];
	uint8_t intensity[FPLINUX_AMBIENT_ZONE_COUNT];
	uint8_t opacity;
};

/*
 * Output abstraction. The runtime installs an LVGL sink; tests and future
 * hardware backends can install another one. apply() is called only when the
 * computed frame changes, with the same context given to set_sink().
 */
struct fplinux_ambient_sink {
	void (*apply)(void *context, const struct fplinux_ambient_frame *frame);
	void *context;
};

struct fplinux_ambient;

/* NULL with errno ENOMEM on allocation failure. */
struct fplinux_ambient *fplinux_ambient_create(void);
void fplinux_ambient_destroy(struct fplinux_ambient *ambient);

/* Replaces the current sink; NULL stops output. */
void fplinux_ambient_set_sink(struct fplinux_ambient *ambient,
			      const struct fplinux_ambient_sink *sink);
/* Disabled output is a black frame until enabled again. */
void fplinux_ambient_set_enabled(struct fplinux_ambient *ambient, bool enabled);

/* Sets the persistent scene. False with errno EINVAL for an unknown scene. */
bool fplinux_ambient_set_scene(struct fplinux_ambient *ambient,
			       enum fplinux_ambient_scene scene);
/*
 * Shows a scene for duration_ms and then returns to the persistent one.
 * Pass 0 to end the current trigger immediately. A trigger of equal or
 * higher priority replaces the active one; a lower one is ignored while it
 * still has time left. False with errno EINVAL for an unknown scene.
 */
bool fplinux_ambient_trigger(struct fplinux_ambient *ambient,
			     enum fplinux_ambient_scene scene,
			     uint32_t duration_ms);
/*
 * Overrides one zone's scene colour until clear_zones(). False with errno
 * EINVAL for an unknown zone.
 */
bool fplinux_ambient_set_zone(struct fplinux_ambient *ambient,
			      enum fplinux_ambient_zone zone,
			      struct fplinux_ambient_rgb rgb);
void fplinux_ambient_clear_zones(struct fplinux_ambient *ambient);
/*
 * Replaces the scene's animation for future and active scenes. period_ms
 * below 100 or an unknown effect fails with EINVAL.
 */
bool fplinux_ambient_set_effect(struct fplinux_ambient *ambient,
				enum fplinux_ambient_effect effect,
				uint32_t period_ms);

/*
 * Recomputes the frame for a monotonic CLOCK_MONOTONIC time in milliseconds
 * and calls the sink when it changed. The runtime calls this from its LVGL
 * timer; applications do not need a second timer.
 */
void fplinux_ambient_tick(struct fplinux_ambient *ambient, uint64_t now_ms);
/* Borrowed latest frame, valid until the next tick or destroy. */
const struct fplinux_ambient_frame *
fplinux_ambient_frame(const struct fplinux_ambient *ambient);
/*
 * Creates the runtime's shared engine and LVGL top-layer renderer on the
 * first enable. Later calls pause or resume that renderer without losing
 * scene state. Returns false with errno EINVAL for a NULL ui, or ENOMEM if
 * the renderer cannot be created.
 */
bool fplinux_ui_set_ambient_enabled(struct fplinux_ui *ui, bool enabled);
/* Borrowed engine owned by the runtime, or NULL while ambient is disabled. */
struct fplinux_ambient *fplinux_ui_ambient(struct fplinux_ui *ui);

#endif
