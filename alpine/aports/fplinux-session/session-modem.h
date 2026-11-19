/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef FPLINUX_SESSION_MODEM_H
#define FPLINUX_SESSION_MODEM_H

#include "fplinux-ui-session.h"

/* Cellular status has no uevent; poll the producer's class device. */
#define FPLINUX_SESSION_MODEM_POLL_MS 1000U

/*
 * Reads signal_level and operator_name from the modem status directory. On any
 * missing, malformed or oversized value the state stays unknown: signal -1 and
 * an empty operator name. Returns whether both attributes were valid.
 */
bool fplinux_session_modem_read(const char *directory,
				struct fplinux_ui_session_modem *state);

#endif
