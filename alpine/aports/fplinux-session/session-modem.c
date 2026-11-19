/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "session-modem.h"

#include "fplinux-utf8.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define MODEM_ATTRIBUTE_BYTES 64

/* Reads one sysfs attribute, allowing only its final newline. */
static bool read_attribute(int directory_fd, const char *name, char *text,
			   size_t capacity)
{
	char extra;
	ssize_t count;
	size_t length = 0;
	int fd;

	if (capacity < 2)
		return false;
	fd = openat(directory_fd, name, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		return false;
	while (length + 1 < capacity) {
		count = read(fd, text + length, capacity - 1 - length);
		if (count < 0 && errno == EINTR)
			continue;
		if (count < 0) {
			close(fd);
			return false;
		}
		if (!count)
			break;
		length += (size_t)count;
	}
	do {
		count = read(fd, &extra, 1);
	} while (count < 0 && errno == EINTR);
	close(fd);
	if (count)
		return false;
	text[length] = '\0';
	if (length && text[length - 1] == '\n')
		text[--length] = '\0';
	for (size_t i = 0; i < length; ++i)
		if ((unsigned char)text[i] < 0x20 || text[i] == 0x7f)
			return false;
	return true;
}

static bool parse_signal(const char *text, int *signal)
{
	char *end;
	long long value;

	if (!text[0] || (!isdigit((unsigned char)text[0]) && text[0] != '-' &&
			 text[0] != '+'))
		return false;
	errno = 0;
	value = strtoll(text, &end, 10);
	if (errno || end == text || *end || value < -1 || value > 5)
		return false;
	*signal = (int)value;
	return true;
}

static bool operator_valid(const char *text)
{
	const unsigned char *cursor = (const unsigned char *)text;
	const unsigned char *end = cursor + strlen(text);
	uint32_t scalar;

	if (strlen(text) >=
	    sizeof(((struct fplinux_ui_session_modem *)0)->operator_name))
		return false;
	while (cursor < end) {
		if (!fplinux_utf8_next(&cursor, end, &scalar))
			return false;
		if (scalar < 0x20 || (scalar >= 0x7f && scalar <= 0x9f))
			return false;
	}
	return true;
}

bool fplinux_session_modem_read(const char *directory,
				struct fplinux_ui_session_modem *state)
{
	char text[MODEM_ATTRIBUTE_BYTES];
	DIR *modem;
	int directory_fd;
	bool valid = true;

	state->signal = -1;
	state->operator_name[0] = '\0';
	if (!directory)
		return false;
	modem = opendir(directory);
	if (!modem)
		return false;
	directory_fd = dirfd(modem);
	if (directory_fd < 0) {
		closedir(modem);
		return false;
	}
	if (!read_attribute(directory_fd, "signal_level", text, sizeof(text)) ||
	    !parse_signal(text, &state->signal))
		valid = false;
	if (!read_attribute(directory_fd, "operator_name", text,
			    sizeof(text)) ||
	    !operator_valid(text))
		valid = false;
	closedir(modem);
	if (!valid) {
		state->signal = -1;
		state->operator_name[0] = '\0';
		return false;
	}
	strcpy(state->operator_name, text);
	return true;
}
