/* SPDX-License-Identifier: LGPL-2.1-or-later */
#ifndef FPLINUX_CONTACTS_H
#define FPLINUX_CONTACTS_H

#include <stdbool.h>
#include <stddef.h>

/*
 * Shared contacts storage and lookup.
 *
 * Contacts are normal vCard (.vcf) files in one directory so they are easy to
 * edit and sync. The primary location is /mnt/card/vcards. Each file stores one
 * contact (BEGIN:VCARD ... END:VCARD) with a stable UID used as the contact id
 * and, by default, as the file name (<uid>.vcf).
 */

#define FPLINUX_CONTACTS_DEFAULT_DIR "/mnt/card/vcards"
#define FPLINUX_CONTACTS_NAME_BYTES 256
#define FPLINUX_CONTACTS_NUMBER_BYTES 64
#define FPLINUX_CONTACTS_UID_BYTES 64
#define FPLINUX_CONTACTS_PATH_BYTES 512
#define FPLINUX_CONTACTS_TEL_MAX 4

struct fplinux_contact_tel {
	char value[FPLINUX_CONTACTS_NUMBER_BYTES];
	/* Optional type label such as CELL, HOME, WORK; may be empty. */
	char type[32];
};

struct fplinux_contact {
	char uid[FPLINUX_CONTACTS_UID_BYTES];
	char fn[FPLINUX_CONTACTS_NAME_BYTES];
	char path[FPLINUX_CONTACTS_PATH_BYTES];
	struct fplinux_contact_tel tel[FPLINUX_CONTACTS_TEL_MAX];
	size_t tel_count;
};

/* A snapshot of contacts loaded from a directory. */
struct fplinux_contact_list {
	struct fplinux_contact *items;
	size_t count;
	size_t capacity;
};

/* Returns dir, or FPLINUX_CONTACTS_DEFAULT_DIR when dir is NULL or empty. */
const char *fplinux_contacts_dir(const char *dir);
/* Creates the directory (mode 0755) if it does not exist. */
bool fplinux_contacts_ensure_dir(const char *dir);

/* Initializes an empty list. */
void fplinux_contact_list_init(struct fplinux_contact_list *list);
/* Frees all items and resets the list. */
void fplinux_contact_list_free(struct fplinux_contact_list *list);
/*
 * Loads all *.vcf contacts from dir (or the default directory), sorted by
 * display name then UID. Invalid files are skipped. Returns false on a fatal
 * directory/read error (errno set); an empty or missing directory yields an
 * empty list and returns true if the directory does not exist.
 */
bool fplinux_contact_list_load(const char *dir,
			       struct fplinux_contact_list *list);

/* Finds a contact by UID in a loaded list. Returns NULL if not found. */
const struct fplinux_contact *
fplinux_contact_list_find_uid(const struct fplinux_contact_list *list,
			      const char *uid);

/*
 * Normalizes a phone number for comparison: keeps a leading '+' and digits,
 * removing spaces, punctuation and formatting characters. out must have room
 * for at least FPLINUX_CONTACTS_NUMBER_BYTES bytes.
 */
void fplinux_contacts_normalize_number(const char *number, char *out,
				       size_t out_size);
/*
 * Returns true when two numbers match: equal normalized strings, or one
 * normalized tail matches the other when either has at least 7 digits
 * (tolerates missing/extra country or trunk prefix).
 */
bool fplinux_contacts_number_match(const char *a, const char *b);
/*
 * Looks up the first contact whose any TEL matches number. Returns NULL if
 * not found. When multiple match, the first in list order (sorted by name) is
 * returned.
 */
const struct fplinux_contact *
fplinux_contact_list_lookup_number(const struct fplinux_contact_list *list,
				   const char *number);

/*
 * Parses one vCard from text into contact. path may be NULL or the source
 * file path. Returns false if no usable contact is found.
 */
bool fplinux_contact_parse(const char *text, const char *path,
			   struct fplinux_contact *contact);
/* Reads and parses a .vcf file. Returns false on read/parse error. */
bool fplinux_contact_load_file(const char *path,
			       struct fplinux_contact *contact);

/*
 * Formats contact as vCard 3.0 text into out (NUL-terminated). Returns the
 * number of bytes written excluding NUL, or (size_t)-1 if out_size is too
 * small.
 */
size_t fplinux_contact_format_vcard(const struct fplinux_contact *contact,
				    char *out, size_t out_size);

/*
 * Saves contact to dir as <uid>.vcf (atomically via temp+rename). Creates the
 * directory if needed. Updates contact->path on success.
 */
bool fplinux_contact_save(const char *dir, struct fplinux_contact *contact);
/* Deletes the contact file at contact->path. */
bool fplinux_contact_delete(struct fplinux_contact *contact);

/* Generates a new random/unique-enough UID string into out. */
void fplinux_contacts_generate_uid(char *out, size_t out_size);

#endif
