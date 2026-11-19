/* SPDX-License-Identifier: LGPL-2.1-or-later */
#define _GNU_SOURCE
#include "fplinux-contacts.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

const char *fplinux_contacts_dir(const char *dir)
{
	return (dir && dir[0]) ? dir : FPLINUX_CONTACTS_DEFAULT_DIR;
}

bool fplinux_contacts_ensure_dir(const char *dir)
{
	struct stat st;

	dir = fplinux_contacts_dir(dir);
	if (mkdir(dir, 0755) == 0)
		return true;
	if (errno != EEXIST)
		return false;
	return stat(dir, &st) == 0 && S_ISDIR(st.st_mode);
}

void fplinux_contact_list_init(struct fplinux_contact_list *list)
{
	list->items = NULL;
	list->count = 0;
	list->capacity = 0;
}

void fplinux_contact_list_free(struct fplinux_contact_list *list)
{
	free(list->items);
	list->items = NULL;
	list->count = 0;
	list->capacity = 0;
}

static bool list_grow(struct fplinux_contact_list *list)
{
	size_t cap = list->capacity ? list->capacity * 2 : 8;
	struct fplinux_contact *items =
		realloc(list->items, cap * sizeof(*items));

	if (!items)
		return false;
	list->items = items;
	list->capacity = cap;
	return true;
}

static void trim_line(char *s)
{
	size_t n = strlen(s);

	while (n > 0 && (s[n - 1] == '\r' || s[n - 1] == '\n' ||
			 s[n - 1] == ' ' || s[n - 1] == '\t'))
		s[--n] = '\0';
}

static void copy_field(char *dst, size_t dst_size, const char *src)
{
	size_t n;

	if (!dst_size)
		return;
	if (!src)
		src = "";
	n = strlen(src);
	if (n >= dst_size)
		n = dst_size - 1;
	memcpy(dst, src, n);
	dst[n] = '\0';
}

static void unfold_vcard(char *text)
{
	char *r = text;
	char *w = text;

	while (*r) {
		if (r[0] == '\r' && r[1] == '\n' &&
		    (r[2] == ' ' || r[2] == '\t')) {
			r += 3;
			continue;
		}
		if (r[0] == '\n' && (r[1] == ' ' || r[1] == '\t')) {
			r += 2;
			continue;
		}
		*w++ = *r++;
	}
	*w = '\0';
}

static void decode_value(const char *in, char *out, size_t out_size)
{
	size_t i = 0;

	if (!out_size)
		return;
	while (*in && i + 1 < out_size) {
		if (in[0] == '\\' && in[1]) {
			if (in[1] == 'n' || in[1] == 'N') {
				out[i++] = '\n';
				in += 2;
			} else if (in[1] == '\\' || in[1] == ';' ||
				   in[1] == ',') {
				out[i++] = in[1];
				in += 2;
			} else {
				out[i++] = in[1];
				in += 2;
			}
		} else {
			out[i++] = *in++;
		}
	}
	out[i] = '\0';
}

static void encode_value(const char *in, char *out, size_t out_size)
{
	size_t i = 0;

	if (!out_size)
		return;
	while (*in && i + 1 < out_size) {
		unsigned char c = (unsigned char)*in++;

		if (c == '\\') {
			if (i + 2 >= out_size)
				break;
			out[i++] = '\\';
			out[i++] = '\\';
		} else if (c == ';') {
			if (i + 2 >= out_size)
				break;
			out[i++] = '\\';
			out[i++] = ';';
		} else if (c == ',') {
			if (i + 2 >= out_size)
				break;
			out[i++] = '\\';
			out[i++] = ',';
		} else if (c == '\n') {
			if (i + 2 >= out_size)
				break;
			out[i++] = '\\';
			out[i++] = 'n';
		} else if (c == '\r') {
			/* drop */
		} else {
			out[i++] = (char)c;
		}
	}
	out[i] = '\0';
}

static bool prop_name_is(const char *line, const char *name)
{
	size_t n = strlen(name);

	if (strncasecmp(line, name, n) != 0)
		return false;
	return line[n] == ':' || line[n] == ';' || line[n] == '\0';
}

static const char *prop_value(const char *line)
{
	const char *colon = strchr(line, ':');

	return colon ? colon + 1 : NULL;
}

static void extract_tel_type(const char *line, char *type, size_t type_size)
{
	const char *params_end = strchr(line, ':');
	const char *p = strchr(line, ';');
	const char *type_kw;

	if (type_size)
		type[0] = '\0';
	if (!p || (params_end && p > params_end))
		return;
	type_kw = "TYPE=";
	while (p && (!params_end || p < params_end)) {
		p++;
		if (strncasecmp(p, type_kw, 5) == 0) {
			const char *v = p + 5;
			size_t i = 0;

			while (v[i] && v[i] != ';' && v[i] != ':' &&
			       v[i] != ',' && i + 1 < type_size) {
				type[i] = (char)toupper((unsigned char)v[i]);
				i++;
			}
			if (type_size)
				type[i < type_size ? i : type_size - 1] = '\0';
			return;
		}
		/* bare type token like TEL;CELL: */
		{
			const char *v = p;
			size_t i = 0;

			while (v[i] && v[i] != ';' && v[i] != ':' &&
			       v[i] != ',' && v[i] != '=')
				i++;
			if (i > 0 && v[i] != '=' && i + 1 < type_size) {
				size_t k = 0;

				for (k = 0; k < i && k + 1 < type_size; k++)
					type[k] = (char)toupper(
						(unsigned char)v[k]);
				type[k] = '\0';
				/* ignore VALUE= etc */
				if (strcmp(type, "VOICE") == 0 ||
				    strcmp(type, "PREF") == 0) {
					type[0] = '\0';
					p = strchr(v, ';');
					continue;
				}
				return;
			}
		}
		p = strchr(p, ';');
	}
}

bool fplinux_contact_parse(const char *text, const char *path,
			   struct fplinux_contact *contact)
{
	char *copy;
	char *line;
	char *save = NULL;
	char decoded[FPLINUX_CONTACTS_NAME_BYTES];
	bool in_card = false;

	memset(contact, 0, sizeof(*contact));
	if (path)
		copy_field(contact->path, sizeof(contact->path), path);
	if (!text)
		return false;
	copy = strdup(text);
	if (!copy)
		return false;
	unfold_vcard(copy);
	for (line = strtok_r(copy, "\n", &save); line;
	     line = strtok_r(NULL, "\n", &save)) {
		const char *value;

		trim_line(line);
		if (strcasecmp(line, "BEGIN:VCARD") == 0) {
			in_card = true;
			continue;
		}
		if (strcasecmp(line, "END:VCARD") == 0)
			break;
		if (!in_card)
			continue;
		value = prop_value(line);
		if (!value)
			continue;
		if (prop_name_is(line, "FN")) {
			decode_value(value, decoded, sizeof(decoded));
			copy_field(contact->fn, sizeof(contact->fn), decoded);
		} else if (prop_name_is(line, "N") && !contact->fn[0]) {
			/* N:family;given;additional;prefix;suffix */
			char tmp[FPLINUX_CONTACTS_NAME_BYTES];
			char *parts[5] = { 0 };
			int i = 0;
			char *p = tmp;
			char *toksave = NULL;
			char *t;

			decode_value(value, tmp, sizeof(tmp));
			for (t = strtok_r(p, ";", &toksave); t && i < 5;
			     t = strtok_r(NULL, ";", &toksave))
				parts[i++] = t;
			/* given + family as fallback */
			decoded[0] = '\0';
			if (parts[1] && parts[1][0]) {
				copy_field(decoded, sizeof(decoded), parts[1]);
				if (parts[0] && parts[0][0]) {
					size_t l = strlen(decoded);

					if (l + 1 < sizeof(decoded)) {
						decoded[l++] = ' ';
						copy_field(decoded + l,
							   sizeof(decoded) - l,
							   parts[0]);
					}
				}
			} else if (parts[0] && parts[0][0]) {
				copy_field(decoded, sizeof(decoded), parts[0]);
			}
			if (decoded[0])
				copy_field(contact->fn, sizeof(contact->fn),
					   decoded);
		} else if (prop_name_is(line, "UID")) {
			decode_value(value, decoded, sizeof(decoded));
			copy_field(contact->uid, sizeof(contact->uid), decoded);
		} else if (prop_name_is(line, "TEL")) {
			if (contact->tel_count < FPLINUX_CONTACTS_TEL_MAX) {
				struct fplinux_contact_tel *tel =
					&contact->tel[contact->tel_count];

				decode_value(value, decoded, sizeof(decoded));
				/* store as-is (may include spaces) */
				copy_field(tel->value, sizeof(tel->value),
					   decoded);
				extract_tel_type(line, tel->type,
						 sizeof(tel->type));
				if (tel->value[0])
					contact->tel_count++;
			}
		}
	}
	free(copy);
	if (!contact->fn[0] && contact->tel_count > 0)
		copy_field(contact->fn, sizeof(contact->fn),
			   contact->tel[0].value);
	return contact->fn[0] != '\0' || contact->uid[0] != '\0' ||
	       contact->tel_count > 0;
}

bool fplinux_contact_load_file(const char *path,
			       struct fplinux_contact *contact)
{
	FILE *f;
	long size;
	char *buf;
	size_t n;
	bool ok;

	f = fopen(path, "rb");
	if (!f)
		return false;
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return false;
	}
	size = ftell(f);
	if (size < 0 || size > 1024 * 1024) {
		fclose(f);
		if (size > 1024 * 1024)
			errno = EFBIG;
		return false;
	}
	errno = 0;
	rewind(f);
	if (errno) {
		fclose(f);
		return false;
	}
	buf = malloc((size_t)size + 1);
	if (!buf) {
		fclose(f);
		return false;
	}
	n = fread(buf, 1, (size_t)size, f);
	fclose(f);
	buf[n] = '\0';
	ok = fplinux_contact_parse(buf, path, contact);
	free(buf);
	if (ok && !contact->path[0])
		copy_field(contact->path, sizeof(contact->path), path);
	return ok;
}

size_t fplinux_contact_format_vcard(const struct fplinux_contact *contact,
				    char *out, size_t out_size)
{
	char enc_name[FPLINUX_CONTACTS_NAME_BYTES * 2];
	char enc_uid[FPLINUX_CONTACTS_UID_BYTES * 2];
	int n = 0;
	size_t i;

	if (!contact || !out || out_size == 0)
		return (size_t)-1;
	encode_value(contact->fn, enc_name, sizeof(enc_name));
	encode_value(contact->uid, enc_uid, sizeof(enc_uid));
	n = snprintf(out, out_size, "BEGIN:VCARD\r\nVERSION:3.0\r\n");
	if (n < 0 || (size_t)n >= out_size)
		return (size_t)-1;
	if (contact->uid[0]) {
		int m = snprintf(out + n, out_size - (size_t)n, "UID:%s\r\n",
				 enc_uid);

		if (m < 0 || (size_t)m >= out_size - (size_t)n)
			return (size_t)-1;
		n += m;
	}
	{
		int m = snprintf(out + n, out_size - (size_t)n, "FN:%s\r\n",
				 enc_name[0] ? enc_name : "Unnamed");

		if (m < 0 || (size_t)m >= out_size - (size_t)n)
			return (size_t)-1;
		n += m;
	}
	/* simple N: use FN as given name fallback */
	{
		int m = snprintf(out + n, out_size - (size_t)n, "N:;%s;;;\r\n",
				 enc_name[0] ? enc_name : "Unnamed");

		if (m < 0 || (size_t)m >= out_size - (size_t)n)
			return (size_t)-1;
		n += m;
	}
	for (i = 0; i < contact->tel_count; i++) {
		char enc_tel[FPLINUX_CONTACTS_NUMBER_BYTES * 2];
		int m;

		encode_value(contact->tel[i].value, enc_tel, sizeof(enc_tel));
		if (contact->tel[i].type[0])
			m = snprintf(out + n, out_size - (size_t)n,
				     "TEL;TYPE=%s:%s\r\n", contact->tel[i].type,
				     enc_tel);
		else
			m = snprintf(out + n, out_size - (size_t)n,
				     "TEL:%s\r\n", enc_tel);
		if (m < 0 || (size_t)m >= out_size - (size_t)n)
			return (size_t)-1;
		n += m;
	}
	{
		int m = snprintf(out + n, out_size - (size_t)n,
				 "END:VCARD\r\n");

		if (m < 0 || (size_t)m >= out_size - (size_t)n)
			return (size_t)-1;
		n += m;
	}
	return (size_t)n;
}

static bool write_file_atomic(const char *path, const char *data, size_t len)
{
	char tmp[PATH_MAX];
	int fd = -1;
	FILE *f;
	size_t n;
	int ret;

	ret = snprintf(tmp, sizeof(tmp), "%s.tmp", path);
	if (ret < 0 || (size_t)ret >= sizeof(tmp)) {
		errno = ENAMETOOLONG;
		return false;
	}
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return false;
	f = fdopen(fd, "wb");
	if (!f) {
		close(fd);
		unlink(tmp);
		return false;
	}
	n = fwrite(data, 1, len, f);
	if (n != len || fflush(f) != 0) {
		fclose(f);
		unlink(tmp);
		return false;
	}
	if (fsync(fileno(f)) != 0) {
		/* best effort on some fs, but try */
	}
	if (fclose(f) != 0) {
		unlink(tmp);
		return false;
	}
	if (rename(tmp, path) != 0) {
		unlink(tmp);
		return false;
	}
	return true;
}

void fplinux_contacts_generate_uid(char *out, size_t out_size)
{
	static unsigned counter = 0;
	unsigned r = (unsigned)time(NULL);

	r ^= (unsigned)getpid() << 8;
	r ^= ++counter * 2654435761u;
	snprintf(out, out_size, "c%08x-%04x", r, counter & 0xffff);
}

static bool uid_is_safe(const char *uid)
{
	if (!uid || !uid[0])
		return false;
	if (strcmp(uid, ".") == 0 || strcmp(uid, "..") == 0)
		return false;
	for (const char *p = uid; *p; p++) {
		if (*p == '/' || *p == '\\')
			return false;
	}
	return true;
}

bool fplinux_contact_save(const char *dir, struct fplinux_contact *contact)
{
	char path[PATH_MAX];
	char buf[4096];
	size_t len;
	int ret;

	dir = fplinux_contacts_dir(dir);
	if (!fplinux_contacts_ensure_dir(dir))
		return false;
	if (!contact->uid[0])
		fplinux_contacts_generate_uid(contact->uid,
					      sizeof(contact->uid));
	if (!uid_is_safe(contact->uid)) {
		errno = EINVAL;
		return false;
	}
	ret = snprintf(path, sizeof(path), "%s/%s.vcf", dir, contact->uid);
	if (ret < 0 || (size_t)ret >= sizeof(path)) {
		errno = ENAMETOOLONG;
		return false;
	}
	len = fplinux_contact_format_vcard(contact, buf, sizeof(buf));
	if (len == (size_t)-1) {
		errno = EOVERFLOW;
		return false;
	}
	if (!write_file_atomic(path, buf, len))
		return false;
	copy_field(contact->path, sizeof(contact->path), path);
	return true;
}

bool fplinux_contact_delete(struct fplinux_contact *contact)
{
	if (!contact || !contact->path[0])
		return false;
	return unlink(contact->path) == 0 || errno == ENOENT;
}

static int compare_contacts(const void *a, const void *b)
{
	const struct fplinux_contact *ca = a;
	const struct fplinux_contact *cb = b;
	int r = strcasecmp(ca->fn, cb->fn);

	if (r != 0)
		return r;
	return strcmp(ca->uid, cb->uid);
}

bool fplinux_contact_list_load(const char *dir,
			       struct fplinux_contact_list *list)
{
	DIR *d;
	struct dirent *ent;
	char path[PATH_MAX];

	dir = fplinux_contacts_dir(dir);
	fplinux_contact_list_free(list);
	fplinux_contact_list_init(list);
	d = opendir(dir);
	if (!d) {
		if (errno == ENOENT)
			return true;
		return false;
	}
	while ((ent = readdir(d)) != NULL) {
		struct fplinux_contact c;
		size_t name_len = strlen(ent->d_name);
		int ret;

		if (name_len < 4 ||
		    strcasecmp(ent->d_name + name_len - 4, ".vcf") != 0)
			continue;
		ret = snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
		if (ret < 0 || (size_t)ret >= sizeof(path))
			continue;
		if (!fplinux_contact_load_file(path, &c))
			continue;
		if (!c.uid[0]) {
			/* derive uid from filename */
			char uid[FPLINUX_CONTACTS_UID_BYTES];
			size_t u = name_len - 4;

			if (u >= sizeof(uid))
				u = sizeof(uid) - 1;
			memcpy(uid, ent->d_name, u);
			uid[u] = '\0';
			copy_field(c.uid, sizeof(c.uid), uid);
		}
		if (list->count == list->capacity && !list_grow(list))
			continue;
		list->items[list->count++] = c;
	}
	closedir(d);
	if (list->count > 1)
		qsort(list->items, list->count, sizeof(list->items),
		      compare_contacts);
	return true;
}

const struct fplinux_contact *
fplinux_contact_list_find_uid(const struct fplinux_contact_list *list,
			      const char *uid)
{
	size_t i;

	if (!list || !uid)
		return NULL;
	for (i = 0; i < list->count; i++) {
		if (strcmp(list->items[i].uid, uid) == 0)
			return &list->items[i];
	}
	return NULL;
}

void fplinux_contacts_normalize_number(const char *number, char *out,
				       size_t out_size)
{
	size_t j = 0;
	bool leading = true;

	if (!out_size)
		return;
	if (!number) {
		out[0] = '\0';
		return;
	}
	while (*number && j + 1 < out_size) {
		unsigned char c = (unsigned char)*number++;

		if (leading && c == '+') {
			out[j++] = '+';
			leading = false;
			continue;
		}
		if (isdigit(c)) {
			out[j++] = (char)c;
			leading = false;
		} else if (c == '*' || c == '#' || c == 'p' || c == 'w' ||
			   c == 'P' || c == 'W') {
			/* keep dial chars? normalize for matching: skip */
		}
	}
	out[j] = '\0';
}

static size_t digit_count(const char *s)
{
	size_t n = 0;

	if (!s)
		return 0;
	if (*s == '+')
		s++;
	while (*s) {
		if (isdigit((unsigned char)*s))
			n++;
		s++;
	}
	return n;
}

bool fplinux_contacts_number_match(const char *a, const char *b)
{
	char na[FPLINUX_CONTACTS_NUMBER_BYTES];
	char nb[FPLINUX_CONTACTS_NUMBER_BYTES];
	size_t la, lb;

	fplinux_contacts_normalize_number(a, na, sizeof(na));
	fplinux_contacts_normalize_number(b, nb, sizeof(nb));
	if (!na[0] || !nb[0])
		return false;
	if (strcmp(na, nb) == 0)
		return true;
	/* compare by suffix of digits (ignore leading + and country prefix differences) */
	{
		const char *da = na[0] == '+' ? na + 1 : na;
		const char *db = nb[0] == '+' ? nb + 1 : nb;

		la = strlen(da);
		lb = strlen(db);
		if (la >= 7 && lb >= 7) {
			size_t m = la < lb ? la : lb;

			if (m > 11)
				m = 11; /* up to national+prefix range */
			if (memcmp(da + la - m, db + lb - m, m) == 0)
				return true;
		}
	}
	/* fallback: digit-only equality ignoring leading + */
	if (digit_count(na) >= 7 && digit_count(na) == digit_count(nb)) {
		const char *pa = na, *pb = nb;

		if (*pa == '+')
			pa++;
		if (*pb == '+')
			pb++;
		if (strcmp(pa, pb) == 0)
			return true;
	}
	return false;
}

const struct fplinux_contact *
fplinux_contact_list_lookup_number(const struct fplinux_contact_list *list,
				   const char *number)
{
	size_t i, j;

	if (!list || !number)
		return NULL;
	for (i = 0; i < list->count; i++) {
		for (j = 0; j < list->items[i].tel_count; j++) {
			if (fplinux_contacts_number_match(
				    list->items[i].tel[j].value, number))
				return &list->items[i];
		}
	}
	return NULL;
}
