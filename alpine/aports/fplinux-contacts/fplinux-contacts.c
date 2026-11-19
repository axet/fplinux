/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "fplinux-contacts.h"
#include "fplinux-cli.h"

#include <fplinux/fplinux-gettext.h>
#include <fplinux/fplinux-ui.h>
#include <fplinux/fplinux-ui-menu.h>
#include <fplinux/fplinux-ui-screen.h>
#include <fplinux/fplinux-ui-text.h>

#include <errno.h>
#include <libintl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define APP_ID "org.fplinux.contacts"
#define _(text) gettext(text)
#define NC_(context, text) context "\004" text

enum command {
	CMD_BACK = 1,
	CMD_NEW,
	CMD_REFRESH,
	CMD_EDIT,
	CMD_DELETE,
	CMD_SAVE,
	CMD_CONFIRM_DELETE,
	CMD_CANCEL_DELETE,
};

enum screen_kind {
	SCREEN_LIST,
	SCREEN_VIEW,
	SCREEN_EDIT,
};

struct contacts_app {
	struct fplinux_ui *ui;
	struct fplinux_contact_list list;
	/* UID of contact shown/edited, empty for new contact. */
	char current_uid[FPLINUX_CONTACTS_UID_BYTES];
	struct fplinux_ui_screen *list_screen;
	struct fplinux_ui_screen *view_screen;
	struct fplinux_ui_screen *edit_screen;
	struct fplinux_ui_screen *delete_alert;
	/* form item indices for edit */
	size_t edit_name_item;
	size_t edit_tel_item;
	bool edit_is_new;
};

/* Helper to add a command with optional icon. */
static void screen_add_cmd(struct contacts_app *app,
			   struct fplinux_ui_screen *screen, int id,
			   enum fplinux_ui_command_type type, const char *label,
			   const char *icon_name, int priority,
			   bool keep_visible)
{
	struct fplinux_ui_command cmd = {
		.id = id,
		.type = type,
		.label = label,
		.priority = priority,
		.keep_visible = keep_visible,
	};

	if (icon_name)
		cmd.icon = fplinux_ui_glyph(app->ui, icon_name,
					    FPLINUX_UI_GLYPH_ROW);
	fplinux_ui_screen_add_command(screen, &cmd);
}

static void set_app_data(struct contacts_app *app,
			 struct fplinux_ui_screen *screen)
{
	fplinux_ui_screen_set_data(screen, app);
}

static void load_contacts(struct contacts_app *app)
{
	if (!fplinux_contact_list_load(NULL, &app->list)) {
		/* keep list as-is/empty on error; show nothing special here */
	}
}

static const struct fplinux_contact *current_contact(struct contacts_app *app)
{
	return fplinux_contact_list_find_uid(&app->list, app->current_uid);
}

static void show_error(struct contacts_app *app, const char *title,
		       const char *text)
{
	struct fplinux_ui_screen *alert = fplinux_ui_alert_new(
		app->ui, FPLINUX_UI_ALERT_ERROR, title, text, 0);

	if (alert) {
		set_app_data(app, alert);
		fplinux_ui_push(app->ui, alert);
	}
}

static void show_list(struct contacts_app *app);
static void show_view(struct contacts_app *app, const char *uid);
static void show_edit(struct contacts_app *app, const char *uid);

static void view_command(struct fplinux_ui *ui,
			 struct fplinux_ui_screen *screen, int id,
			 void *context);
static void edit_command(struct fplinux_ui *ui,
			 struct fplinux_ui_screen *screen, int id,
			 void *context);
static void delete_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *screen, int id,
			   void *context);

static void populate_list_screen(struct contacts_app *app)
{
	size_t i;

	if (!app->list_screen)
		return;
	fplinux_ui_list_delete_all(app->list_screen);
	for (i = 0; i < app->list.count; i++) {
		const struct fplinux_contact *c = &app->list.items[i];
		const char *detail = c->tel_count > 0 ? c->tel[0].value : NULL;

		fplinux_ui_list_append(app->list_screen,
				       c->fn[0] ? c->fn : _("Unnamed"), detail);
	}
}

static void list_command(struct fplinux_ui *ui,
			 struct fplinux_ui_screen *screen, int id,
			 void *context)
{
	struct contacts_app *app = context;
	size_t index;

	(void)screen;
	if (id == CMD_BACK) {
		fplinux_ui_go_back(ui);
		return;
	}
	if (id == CMD_NEW) {
		show_edit(app, NULL);
		return;
	}
	if (id == CMD_REFRESH) {
		load_contacts(app);
		populate_list_screen(app);
		return;
	}
	if (id == FPLINUX_UI_SELECT_COMMAND) {
		index = fplinux_ui_list_get_focused(app->list_screen);
		if (index < app->list.count)
			show_view(app, app->list.items[index].uid);
	}
}

static void show_list(struct contacts_app *app)
{
	if (app->list_screen && fplinux_ui_current(app->ui) == app->list_screen)
		return;
	if (!app->list_screen) {
		app->list_screen = fplinux_ui_list_new(
			app->ui, _("Contacts"), FPLINUX_UI_LIST_IMPLICIT);
		if (!app->list_screen) {
			perror("fplinux-contacts: list");
			return;
		}
		set_app_data(app, app->list_screen);
		screen_add_cmd(app, app->list_screen, CMD_NEW,
			       FPLINUX_UI_COMMAND_SCREEN, _("New"), "add", -1,
			       false);
		screen_add_cmd(app, app->list_screen, CMD_REFRESH,
			       FPLINUX_UI_COMMAND_SCREEN, _("Refresh"),
			       "refresh", 5, false);
		screen_add_cmd(app, app->list_screen, CMD_BACK,
			       FPLINUX_UI_COMMAND_BACK, NULL, "back", 10,
			       false);
		fplinux_ui_screen_set_listener(app->list_screen, list_command,
					       NULL, app);
		fplinux_ui_push(app->ui, app->list_screen);
	} else {
		/* ensure it's on stack/current; simple approach: replace or navigate */
	}
	load_contacts(app);
	populate_list_screen(app);
}

static void show_view(struct contacts_app *app, const char *uid)
{
	const struct fplinux_contact *c;
	size_t i;

	if (uid)
		strncpy(app->current_uid, uid, sizeof(app->current_uid) - 1);
	app->current_uid[sizeof(app->current_uid) - 1] = '\0';
	c = current_contact(app);
	if (!c) {
		load_contacts(app);
		c = current_contact(app);
	}
	if (!c)
		return;

	/* recreate view screen each time for simplicity */
	app->view_screen =
		fplinux_ui_form_new(app->ui, c->fn[0] ? c->fn : _("Contact"));
	if (!app->view_screen)
		return;
	set_app_data(app, app->view_screen);
	fplinux_ui_form_append_string(app->view_screen, _("Name"),
				      c->fn[0] ? c->fn : "");
	if (c->tel_count == 0) {
		fplinux_ui_form_append_string(app->view_screen, _("Phone"), "");
	} else {
		for (i = 0; i < c->tel_count; i++) {
			const char *label = c->tel[i].type[0] ? c->tel[i].type :
								_("Phone");

			fplinux_ui_form_append_string(app->view_screen, label,
						      c->tel[i].value);
		}
	}
	screen_add_cmd(app, app->view_screen, CMD_EDIT,
		       FPLINUX_UI_COMMAND_SCREEN, _("Edit"), "edit", 0, false);
	screen_add_cmd(app, app->view_screen, CMD_DELETE,
		       FPLINUX_UI_COMMAND_SCREEN, _("Delete"), "delete", 1,
		       false);
	screen_add_cmd(app, app->view_screen, CMD_BACK, FPLINUX_UI_COMMAND_BACK,
		       NULL, "back", 10, false);
	fplinux_ui_screen_set_listener(app->view_screen, view_command, NULL,
				       app);
	fplinux_ui_push(app->ui, app->view_screen);
}

static void ask_delete(struct contacts_app *app)
{
	const struct fplinux_contact *c = current_contact(app);
	char msg[512];

	if (!c)
		return;
	snprintf(msg, sizeof(msg), _("Delete contact \"%s\"?"),
		 c->fn[0] ? c->fn : _("Unnamed"));
	app->delete_alert = fplinux_ui_alert_new(app->ui,
						 FPLINUX_UI_ALERT_CONFIRMATION,
						 _("Delete contact"), msg, 0);
	if (!app->delete_alert)
		return;
	set_app_data(app, app->delete_alert);
	{
		struct fplinux_ui_command ok = {
			.id = CMD_CONFIRM_DELETE,
			.type = FPLINUX_UI_COMMAND_OK,
			.label = _("Delete"),
			.icon = fplinux_ui_glyph(app->ui, "delete",
						 FPLINUX_UI_GLYPH_ROW),
		};
		struct fplinux_ui_command cancel = {
			.id = CMD_CANCEL_DELETE,
			.type = FPLINUX_UI_COMMAND_CANCEL,
			.label = _("Cancel"),
			.icon = fplinux_ui_glyph(app->ui, "cancel",
						 FPLINUX_UI_GLYPH_ROW),
		};

		fplinux_ui_screen_add_command(app->delete_alert, &ok);
		fplinux_ui_screen_add_command(app->delete_alert, &cancel);
	}
	fplinux_ui_screen_set_listener(app->delete_alert, delete_command, NULL,
				       app);
	fplinux_ui_push(app->ui, app->delete_alert);
}

static void view_command(struct fplinux_ui *ui,
			 struct fplinux_ui_screen *screen, int id,
			 void *context)
{
	struct contacts_app *app = context;

	(void)screen;
	if (id == CMD_BACK) {
		fplinux_ui_go_back(ui);
		return;
	}
	if (id == CMD_EDIT) {
		show_edit(app, app->current_uid);
		return;
	}
	if (id == CMD_DELETE) {
		ask_delete(app);
	}
}

static void delete_command(struct fplinux_ui *ui,
			   struct fplinux_ui_screen *screen, int id,
			   void *context)
{
	struct contacts_app *app = context;
	struct fplinux_contact *target = NULL;
	size_t i;

	(void)ui;
	(void)screen;
	app->delete_alert = NULL;
	if (id != CMD_CONFIRM_DELETE)
		return;
	/* find mutable copy in list */
	for (i = 0; i < app->list.count; i++) {
		if (strcmp(app->list.items[i].uid, app->current_uid) == 0) {
			target = &app->list.items[i];
			break;
		}
	}
	if (!target)
		return;
	if (!fplinux_contact_delete(target)) {
		show_error(app, _("Delete failed"),
			   _("Could not delete the contact file."));
		return;
	}
	/* pop view and refresh list */
	load_contacts(app);
	populate_list_screen(app);
	/* go back from view screen to list: view is current parent after alert removed */
	fplinux_ui_pop(app->ui);
}

static void show_edit(struct contacts_app *app, const char *uid)
{
	const struct fplinux_contact *c = NULL;
	const char *name = "";
	const char *tel = "";

	if (!app)
		return;
	app->edit_is_new = (uid == NULL || uid[0] == '\0');
	if (!app->edit_is_new) {
		strncpy(app->current_uid, uid, sizeof(app->current_uid) - 1);
		app->current_uid[sizeof(app->current_uid) - 1] = '\0';
		load_contacts(app);
		c = current_contact(app);
		if (c) {
			name = c->fn;
			if (c->tel_count > 0)
				tel = c->tel[0].value;
		}
	} else {
		app->current_uid[0] = '\0';
	}

	app->edit_screen = fplinux_ui_form_new(
		app->ui,
		app->edit_is_new ? _("New contact") : _("Edit contact"));
	if (!app->edit_screen)
		return;
	set_app_data(app, app->edit_screen);
	app->edit_name_item = fplinux_ui_form_append_text_field(
		app->edit_screen, _("Name"), name, 128, FPLINUX_UI_TEXT_ANY);
	app->edit_tel_item = fplinux_ui_form_append_text_field(
		app->edit_screen, _("Phone"), tel, 48, FPLINUX_UI_TEXT_PHONE);
	fplinux_ui_form_set_focus(app->edit_screen, app->edit_name_item);

	screen_add_cmd(app, app->edit_screen, CMD_SAVE, FPLINUX_UI_COMMAND_OK,
		       _("Save"), "save", 0, true);
	screen_add_cmd(app, app->edit_screen, CMD_BACK,
		       FPLINUX_UI_COMMAND_CANCEL, _("Cancel"), "cancel", 1,
		       true);
	fplinux_ui_screen_set_listener(app->edit_screen, edit_command, NULL,
				       app);
	fplinux_ui_push(app->ui, app->edit_screen);
}

static bool save_edit(struct contacts_app *app)
{
	const char *name;
	const char *tel;
	struct fplinux_contact contact;
	const struct fplinux_contact *existing = NULL;

	if (!app->edit_screen)
		return false;
	name = fplinux_ui_form_get_text(app->edit_screen, app->edit_name_item);
	tel = fplinux_ui_form_get_text(app->edit_screen, app->edit_tel_item);
	if (!name)
		name = "";
	if (!tel)
		tel = "";

	/* require at least name or phone */
	if (name[0] == '\0' && tel[0] == '\0') {
		show_error(app, _("Cannot save"),
			   _("Enter a name or phone number."));
		return false;
	}

	memset(&contact, 0, sizeof(contact));
	if (!app->edit_is_new) {
		existing = current_contact(app);
		if (existing)
			contact = *existing; /* copy */
	}
	if (!app->edit_is_new && app->current_uid[0])
		strncpy(contact.uid, app->current_uid, sizeof(contact.uid) - 1);
	else if (!contact.uid[0])
		contact.uid[0] = '\0'; /* generated on save */

	strncpy(contact.fn, name[0] ? name : tel, sizeof(contact.fn) - 1);
	contact.fn[sizeof(contact.fn) - 1] = '\0';
	contact.tel_count = 0;
	if (tel[0] != '\0') {
		strncpy(contact.tel[0].value, tel,
			sizeof(contact.tel[0].value) - 1);
		contact.tel[0].type[0] = '\0';
		contact.tel_count = 1;
	}
	/* preserve extra numbers if editing? keep simple: replace with first */

	if (!fplinux_contact_save(NULL, &contact)) {
		char msg[256];

		snprintf(msg, sizeof(msg),
			 _("Could not save to %s. Is the SD card mounted?"),
			 fplinux_contacts_dir(NULL));
		show_error(app, _("Save failed"), msg);
		return false;
	}
	strncpy(app->current_uid, contact.uid, sizeof(app->current_uid) - 1);
	app->current_uid[sizeof(app->current_uid) - 1] = '\0';
	load_contacts(app);
	populate_list_screen(app);
	return true;
}

static void edit_command(struct fplinux_ui *ui,
			 struct fplinux_ui_screen *screen, int id,
			 void *context)
{
	struct contacts_app *app = context;

	(void)screen;
	if (id == CMD_BACK) {
		fplinux_ui_go_back(ui);
		return;
	}
	if (id == CMD_SAVE) {
		if (save_edit(app)) {
			/* close edit; if came from view, refresh view by popping edit and view and reopening view */
			fplinux_ui_pop(ui); /* pop edit */
			if (!app->edit_is_new) {
				/* if previous screen was view, pop it and reopen */
				fplinux_ui_pop(ui);
				show_view(app, app->current_uid);
			}
		}
	}
}

static void app_event(struct fplinux_ui *ui,
		      const struct fplinux_ui_event *event, void *context)
{
	struct contacts_app *app = context;

	(void)ui;
	(void)app;
	if (event->type == FPLINUX_UI_EVENT_ACTIVATION &&
	    event->activation == FPLINUX_UI_ACTIVATION_OPEN) {
		/* refresh when opened */
	}
	/* CLOSE/VISIBILITY not specially handled */
}

int main(int argc, char **argv)
{
	struct fplinux_cli cli = {
		.program = "fplinux-contacts",
		.description = "Manage contacts stored as vCard files",
	};
	struct contacts_app app = { 0 };
	int result;

	result = fplinux_cli_parse(&cli, argc, argv);
	if (result != FPLINUX_CLI_READY)
		return result;

	app.ui = fplinux_ui_open(APP_ID, app_event, &app);
	if (!app.ui) {
		perror("fplinux-contacts: open UI");
		return 1;
	}
	fplinux_contact_list_init(&app.list);
	load_contacts(&app);
	show_list(&app);

	result = fplinux_ui_run(app.ui);
	fplinux_ui_close(app.ui);
	fplinux_contact_list_free(&app.list);
	return result < 0 ? 1 : 0;
}
