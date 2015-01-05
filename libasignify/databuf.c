/* Copyright (c) 2015, Vsevolod Stakhov
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *       * Redistributions of source code must retain the above copyright
 *         notice, this list of conditions and the following disclaimer.
 *       * Redistributions in binary form must reproduce the above copyright
 *         notice, this list of conditions and the following disclaimer in the
 *         documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED ''AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL AUTHOR BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <ctype.h>

#ifdef HAVE_MLOCK
#include <sys/mman.h>
#endif

#include "asignify.h"
#include "asignify_internal.h"
#include "blake2.h"
#include "tweetnacl.h"

enum asignify_privkey_field {
	PRIVKEY_FIELD_STRING,
	PRIVKEY_FIELD_UINT,
	PRIVKEY_FIELD_HEX
};

struct asignify_privkey_parser {
	const char *field_name;
	enum asignify_privkey_field field_type;
	long struct_offset;
	unsigned int required_len;
};

/*
 * Keep sorted by field name
 */
const static struct asignify_privkey_parser parser_fields[] = {
	{
		.field_name = "checksum",
		.field_type = PRIVKEY_FIELD_HEX,
		.struct_offset = STRUCT_OFFSET(struct asignify_private_key, checksum),
		.required_len = BLAKE2B_OUTBYTES
	},
	{
		.field_name = "data",
		.field_type = PRIVKEY_FIELD_HEX,
		.struct_offset = STRUCT_OFFSET(struct asignify_private_key, encrypted_blob),
		.required_len = crypto_sign_SECRETKEYBYTES
	},
	{
		.field_name = "id",
		.field_type = PRIVKEY_FIELD_HEX,
		.struct_offset = STRUCT_OFFSET(struct asignify_private_key, id),
		.required_len = KEY_ID_LEN
	},
	{
		.field_name = "kdf",
		.field_type = PRIVKEY_FIELD_STRING,
		.struct_offset = STRUCT_OFFSET(struct asignify_private_key, pbkdf_alg),
		.required_len = 0
	},
	{
		.field_name = "rounds",
		.field_type = PRIVKEY_FIELD_UINT,
		.struct_offset = STRUCT_OFFSET(struct asignify_private_key, rounds)
	},
	{
		.field_name = "salt",
		.field_type = PRIVKEY_FIELD_HEX,
		.struct_offset = STRUCT_OFFSET(struct asignify_private_key, salt),
		.required_len = SALT_LEN
	}
};

void
asignify_public_data_free(struct asignify_public_data *d)
{
	if (d) {
		free(d->data);
		free(d->id);
		d->data = NULL;
		d->id = NULL;
		free(d);
	}
}

void
asignify_alloc_public_data_fields(struct asignify_public_data *pk)
{
	pk->data = xmalloc(pk->data_len);
	pk->id = xmalloc(pk->id_len);
}

/*
 * Native format is:
 * <PUBKEY_MAGIC>:<version>:<id>:<pkey>
 */
struct asignify_public_data*
asignify_public_data_load(const char *buf, size_t buflen, const char *magic,
	size_t magiclen, unsigned int ver_min, unsigned int ver_max,
	unsigned int id_len, unsigned int data_len)
{
	char *errstr;
	const char *p = buf;
	unsigned int version;
	size_t remain = buflen, blen;
	struct asignify_public_data *res = NULL;

	if (buflen <= magiclen || memcmp (buf, magic, magiclen) != 0) {
		return (NULL);
	}

	p += magiclen - 1;
	remain -= magiclen - 1;

	version = strtoul(p, &errstr, 10);
	if (errstr == NULL || *errstr != ':'
			|| version < ver_min || version > ver_max) {
		return (NULL);
	}

	res = xmalloc(sizeof(*res));
	res->version = 1;
	res->data_len = id_len;
	res->id_len = data_len;
	asignify_alloc_public_data_fields(res);

	/* Read ID */
	blen = b64_pton_stop(p, res->id, res->id_len, ":");
	if (blen != res->id_len || (p = strchr(p, ':')) == NULL) {
		asignify_public_data_free(res);
		return (NULL);
	}

	p ++;

	/* Read data */
	blen = b64_pton_stop(p, res->data, res->data_len, "");
	if (blen != res->data_len) {
		asignify_public_data_free(res);
		return (NULL);
	}

	return (res);
}

struct field_search_key {
	const char *begin;
	size_t len;
};

static int
asignify_parser_fields_cmp(const void *k, const void *st)
{
	const struct asignify_privkey_parser *p =
					(const struct asignify_privkey_parser *)st;
	struct field_search_key *key = (struct field_search_key *)k;

	return (strncmp(key->begin, p->field_name, key->len));
}

static void
asignify_privkey_cleanup(struct asignify_private_key *privk)
{
	if (privk != NULL) {
		free(privk->checksum);
		if (privk->encrypted_blob) {
			explicit_memzero(privk->encrypted_blob, crypto_sign_SECRETKEYBYTES);
		}
		free(privk->encrypted_blob);
		free(privk->id);
		free(privk->pbkdf_alg);
		free(privk->salt);
		explicit_memzero(privk, sizeof(*privk));
	}
}

static bool
asignify_private_data_parse_value(const char *val, size_t len,
	const struct asignify_privkey_parser *parser,
	struct asignify_private_key *privk)
{
	unsigned char **desth;
	char **dests;
	unsigned int *destui;

	switch(parser->field_type) {
	case PRIVKEY_FIELD_STRING:
		dests = STRUCT_MEMBER_PTR(char *, privk, parser->struct_offset);
		*dests = xmalloc(len + 1);
		memcpy(*dests, val, len);
		**dests = '\0';
		break;
	case PRIVKEY_FIELD_HEX:
		if (len / 2 != parser->required_len) {
			return (false);
		}

		desth = STRUCT_MEMBER_PTR(unsigned char *, privk, parser->struct_offset);
		*desth = xmalloc(len / 2);
		if (hex2bin(*desth, len / 2, val, len, NULL, NULL) == -1) {
			free(*desth);
			*desth = NULL;
			return (false);
		}
		break;
	case PRIVKEY_FIELD_UINT:
		destui = STRUCT_MEMBER_PTR(unsigned int, privk, parser->struct_offset);
		errno = 0;
		*destui = strtoul(val, NULL, 10);
		if (errno != 0) {
			return (false);
		}
		break;
	}

	return (true);
}

static bool
asignify_private_data_parse_line(const char *buf, size_t buflen,
	struct asignify_private_key *privk)
{
	const char *p, *end, *c;
	enum {
		PARSE_NAME = 0,
		PARSE_SEMICOLON,
		PARSE_VALUE,
		PARSE_SPACES,
		PARSE_ERROR
	} state = 0, next_state = 0;
	const struct asignify_privkey_parser *parser = NULL;
	struct field_search_key k;

	p = buf;
	end = buf + buflen;
	c = buf;

	while (p < end) {
		switch (state) {
		case PARSE_NAME:
			if (*p == ':') {
				if (p - c > 0) {
					k.begin = c;
					k.len = p - c;
					parser = bsearch(&k, parser_fields,
						sizeof(parser_fields) / sizeof(parser_fields[0]),
						sizeof(parser_fields[0]), asignify_parser_fields_cmp);

					if (parser == NULL) {
						state = PARSE_ERROR;
					}
					else {
						state = PARSE_SEMICOLON;
					}
				}
				else {
					state = PARSE_ERROR;
				}
			}
			else if (!isgraph(*p)) {
				state = PARSE_ERROR;
			}
			else {
				p ++;
			}
			break;
		case PARSE_SEMICOLON:
			if (*p == ':') {
				p ++;
				state = PARSE_SPACES;
				next_state = PARSE_VALUE;
			}
			else {
				state = PARSE_ERROR;
			}
			break;
		case PARSE_VALUE:
			if (parser == NULL) {
				state = PARSE_ERROR;
			}
			else if (parser->field_type == PRIVKEY_FIELD_UINT && !isdigit(*p)) {
				state = PARSE_ERROR;
			}
			else if (*p == '\n') {
				if (!asignify_private_data_parse_value(c, p - c, parser, privk)) {
					state = PARSE_ERROR;
				}
				else {
					state = PARSE_SPACES;
					next_state = PARSE_NAME;
				}
			}
			else {
				p ++;
			}
			break;
		case PARSE_SPACES:
			if (isspace(*p)) {
				p ++;
			}
			else {
				c = p;
				state = next_state;
				parser = NULL;
			}
			break;
		case PARSE_ERROR:
			return (false);
		}
	}

	return (state == PARSE_SPACES);
}

static bool
asignify_private_key_is_sane(struct asignify_private_key *privk)
{
	if (privk->pbkdf_alg && strcmp(privk->pbkdf_alg, PBKDF_ALG) == 0) {
		if (privk->rounds >= PBKDF_MINROUNDS) {
			if (privk->salt) {
				if (privk->version == 1) {
					if (privk->id != NULL && privk->encrypted_blob != NULL &&
									privk->checksum != NULL) {
						return (true);
					}
				}
			}
		}
	}
	else {
		/* Unencrypted key */
		if (privk->version == 1) {
			if (privk->id != NULL && privk->encrypted_blob != NULL) {
				return (true);
			}
		}
	}

	return (false);
}

struct asignify_private_data*
asignify_private_data_unpack_key(struct asignify_private_key *privk,
	asignify_password_cb password_cb, void *d)
{
	unsigned char canary[10];
	char password[1024];
	struct asignify_private_data *priv;
	unsigned char xorkey[crypto_sign_SECRETKEYBYTES];
	unsigned char res_checksum[BLAKE2B_OUTBYTES];
	int r;

	priv = xmalloc(sizeof(*priv));

	if (privk->pbkdf_alg) {
		/* We need to derive key */
		if (password_cb == NULL) {
			free(priv);
			asignify_privkey_cleanup(privk);
			return (NULL);
		}

		/* Some buffer overflow protection */
		randombytes(canary, sizeof(canary));
		memcpy(password + sizeof(password) - sizeof(canary), canary,
				sizeof(canary));
		r = password_cb(password, sizeof(password) - sizeof(canary), d);
		if (r <= 0 || r > sizeof(password) - sizeof(canary) ||
			memcmp(password + sizeof(password) - sizeof(canary), canary, sizeof(canary)) != 0) {
			free(priv);
			explicit_memzero(password, sizeof(password));
			asignify_privkey_cleanup(privk);
			return (NULL);
		}

		if (pkcs5_pbkdf2(password, r, privk->salt, SALT_LEN, xorkey, sizeof(xorkey),
			privk->rounds) == -1) {
			free(priv);
			explicit_memzero(password, sizeof(password));
			asignify_privkey_cleanup(privk);
			return (NULL);
		}
		explicit_memzero(password, sizeof(password));

		for (r = 0; r < sizeof(xorkey); r ++) {
			privk->encrypted_blob[r] ^= xorkey[r];
		}

		explicit_memzero(xorkey, sizeof(xorkey));
		blake2b(res_checksum, privk->encrypted_blob, NULL, BLAKE2B_OUTBYTES,
			sizeof(xorkey), 0);

		if (memcmp(res_checksum, privk->checksum, sizeof(res_checksum)) != 0) {
			explicit_memzero(privk->encrypted_blob, crypto_sign_SECRETKEYBYTES);
			asignify_privkey_cleanup(privk);
			free(priv);
			return (NULL);
		}
		priv->data = xmalloc(crypto_sign_SECRETKEYBYTES);
		priv->data_len = crypto_sign_SECRETKEYBYTES;
		memcpy(priv->data, privk->encrypted_blob, crypto_sign_SECRETKEYBYTES);
		explicit_memzero(privk->encrypted_blob, crypto_sign_SECRETKEYBYTES);
		priv->id = xmalloc(KEY_ID_LEN);
		priv->id_len = KEY_ID_LEN;
		memcpy(priv->id, privk->id, KEY_ID_LEN);
	}
	else {
		priv->data = xmalloc(crypto_sign_SECRETKEYBYTES);
		priv->data_len = crypto_sign_SECRETKEYBYTES;
		memcpy(priv->data, privk->encrypted_blob, crypto_sign_SECRETKEYBYTES);
		explicit_memzero(privk->encrypted_blob, crypto_sign_SECRETKEYBYTES);
		priv->id = xmalloc(KEY_ID_LEN);
		priv->id_len = KEY_ID_LEN;
		memcpy(priv->id, privk->id, KEY_ID_LEN);
	}

	asignify_privkey_cleanup(privk);

	return (priv);
}

struct asignify_private_data*
asignify_private_data_load(FILE *f, asignify_password_cb password_cb, void *d)
{
	char *buf = NULL;
	size_t buflen = 0;
	struct asignify_private_key privk;
	bool first = true;

	memset(&privk, 0, sizeof(privk));

	while (getline(&buf, &buflen, f) != -1) {
		if (first) {
			/* Check magic */
			if (memcmp(buf, PRIVKEY_MAGIC, sizeof(PRIVKEY_MAGIC) - 1) == 0) {
				first = false;
			}
			else {
				return (NULL);
			}
		}
		else {
			if (!asignify_private_data_parse_line(buf, buflen, &privk)) {
				asignify_privkey_cleanup(&privk);
				return (NULL);
			}
		}
	}

	if (!asignify_private_key_is_sane(&privk)) {
		asignify_privkey_cleanup(&privk);
		return (NULL);
	}

	return (asignify_private_data_unpack_key(&privk, password_cb, d));
}

void
asignify_private_data_free(struct asignify_private_data *d)
{
	if (d != NULL) {
		free(d->id);
		d->id = NULL;
		explicit_memzero(d->data, d->data_len);
#ifdef HAVE_MLOCK
		munlock(d->data, d->data_len);
#endif
		free(d->data);
		free(d);
	}
}