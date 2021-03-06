/*
 * 2012+ Copyright (c) Evgeniy Polyakov <zbr@ioremap.net>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/mman.h>
#include <sys/wait.h>

#include <errno.h>
#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "elliptics/packet.h"
#include "elliptics/interface.h"

#include "backends.h"
#include "common.h"

#include <smack/smack.h>

#if SMACK_KEY_SIZE != DNET_ID_SIZE
#error "SMACK_KEY_SIZE does not match DNET_ID_SIZE"
#endif

#ifndef __unused
#define __unused	__attribute__ ((unused))
#endif

struct smack_backend
{
	int			sync;

	struct eblob_log	log;
	struct eblob_backend	*meta;

	struct smack_init_ctl	ictl;
	struct smack_ctl	*smack;
};

static inline void smack_setup_idx(struct index *idx, unsigned char *id)
{
	memcpy(idx->id, id, SMACK_KEY_SIZE);
	idx->data_size = 0;
}

static int smack_backend_lookup_raw(struct smack_backend *s, struct index *idx, void *state, struct dnet_cmd *cmd)
{
	int err, fd;
	char *path;

	err = smack_lookup(s->smack, idx, &path);
	if (err < 0)
		goto err_out_exit;

	fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		err = -errno;
		dnet_backend_log(DNET_LOG_ERROR, "%s: SMACK: %s: lookup-open: size: %llu: %s %d.\n",
				dnet_dump_id_str(idx->id), path,
				(unsigned long long)idx->data_size,
				strerror(-err), err);
		goto err_out_free;
	}

	err = dnet_send_file_info(state, cmd, fd, 0, idx->data_size);
	if (err)
		goto err_out_close;

	dnet_backend_log(DNET_LOG_INFO, "%s: SMACK: %s: lookup: size: %llu.\n",
			dnet_dump_id(&cmd->id), path, (unsigned long long)idx->data_size);

err_out_close:
	close(fd);
err_out_free:
	free(path);
err_out_exit:
	return err;
}

static int smack_backend_lookup(struct smack_backend *s, void *state, struct dnet_cmd *cmd)
{
	struct index idx;

	smack_setup_idx(&idx, cmd->id.id);
	return smack_backend_lookup_raw(s, &idx, state, cmd);
}

static int smack_backend_write(struct smack_backend *s, void *state, struct dnet_cmd *cmd, void *data)
{
	struct dnet_node *n = dnet_get_node_from_state(state);
	int err;
	struct index idx;
	struct dnet_io_attr *io = data;
	struct dnet_file_info *info;
	struct dnet_addr_attr *a;

	dnet_convert_io_attr(io);
	
	data += sizeof(struct dnet_io_attr);

	smack_setup_idx(&idx, io->id);
	idx.data_size = io->size;

	err = smack_write(s->smack, &idx, data);
	if (err < 0)
		goto err_out_exit;
#if 0
	err = smack_backend_lookup_raw(s, &idx, state, cmd);
	if (err)
		goto err_out_exit;
#else
	a = malloc(sizeof(struct dnet_addr_attr) + sizeof(struct dnet_file_info));
	if (!a) {
		err = -ENOMEM;
		goto err_out_exit;
	}
	info = (struct dnet_file_info *)(a + 1);

	dnet_fill_addr_attr(n, a);
	dnet_convert_addr_attr(a);

	memset(info, 0, sizeof(struct dnet_file_info));
	dnet_convert_file_info(info);

	err = dnet_send_reply(state, cmd, a, sizeof(struct dnet_addr_attr) + sizeof(struct dnet_file_info), 0);
#endif
	dnet_backend_log(DNET_LOG_INFO, "%s: SMACK: : WRITE: Ok: offset: %llu, size: %llu.\n",
			dnet_dump_id(&cmd->id), (unsigned long long)io->offset, (unsigned long long)io->size);

err_out_exit:
	return err;
}

static int smack_backend_read(struct smack_backend *s, void *state, struct dnet_cmd *cmd, void *iodata)
{
	struct dnet_io_attr *io = iodata;
	char *data;
	struct index idx;
	int err;

	dnet_convert_io_attr(io);

	smack_setup_idx(&idx, io->id);
	idx.data_size = io->size;

	err = smack_read(s->smack, &idx, &data);
	if (err < 0)
		goto err_out_exit;

	io->size = idx.data_size;
	err = dnet_send_read_data(state, cmd, io, data, -1, io->offset, 0);
	if (err)
		goto err_out_free;

err_out_free:
	free(data);
err_out_exit:
	return err;
}

static int smack_backend_remove(struct smack_backend *s, void *state __unused, struct dnet_cmd *cmd, void *data __unused)
{
	struct index idx;

	smack_setup_idx(&idx, cmd->id.id);
	return smack_remove(s->smack, &idx);
}

static int smack_backend_bulk_read(struct smack_backend *s, void *state, struct dnet_cmd *cmd, void *data)
{
	int err = -1, ret;
	struct dnet_io_attr *io = data;
	struct dnet_io_attr *ios = io+1;
	uint64_t count = 0;
	uint64_t i;

	dnet_convert_io_attr(io);
	count = io->size / sizeof(struct dnet_io_attr);

	for (i = 0; i < count; i++) {
		ret = smack_backend_read(s, state, cmd, &ios[i]);
		if (!ret)
			err = 0;
		else if (err == -1)
			err = ret;
	}

	return err;
}

static int smack_backend_checksum(struct dnet_node *n, void *priv, struct dnet_id *id, void *csum, int *csize) {
	char *data;
	struct index idx;
	int err;

	smack_setup_idx(&idx, id->id);
	idx.data_size = 0;

	err = smack_read(((struct smack_backend*)priv)->smack, &idx, &data);
	if (err < 0)
		goto err_out_exit;

	err = dnet_checksum_data(n, csum, csize, data, idx.data_size);

	free(data);
err_out_exit:
	return err;
}

static int smack_backend_command_handler(void *state, void *priv, struct dnet_cmd *cmd, void *data)
{
	int err;
	struct smack_backend *s = priv;

	switch (cmd->cmd) {
		case DNET_CMD_LOOKUP:
			err = smack_backend_lookup(s, state, cmd);
			break;
		case DNET_CMD_WRITE:
			err = smack_backend_write(s, state, cmd, data);
			break;
		case DNET_CMD_READ:
			err = smack_backend_read(s, state, cmd, data);
			break;
		case DNET_CMD_STAT:
			err = backend_stat(state, s->ictl.path, cmd);
			break;
		case DNET_CMD_DEL:
			err = smack_backend_remove(s, state, cmd, data);
			break;
		case DNET_CMD_BULK_READ:
			err = smack_backend_bulk_read(s, state, cmd, data);
			break;
		case DNET_CMD_READ_RANGE:
			err = -ENOTSUP;
			break;
		default:
			err = -EINVAL;
			break;
	}

	return err;
}

static int dnet_smack_set_cache_size(struct dnet_config_backend *b, char *key __unused, char *value)
{
	struct smack_backend *s = b->data;

	s->ictl.max_cache_size = atoi(value);
	return 0;
}

static int dnet_smack_set_bloom_size(struct dnet_config_backend *b, char *key __unused, char *value)
{
	struct smack_backend *s = b->data;

	s->ictl.bloom_size = atoi(value);
	return 0;
}

static int dnet_smack_set_blob_num(struct dnet_config_backend *b, char *key __unused, char *value)
{
	struct smack_backend *s = b->data;

	s->ictl.max_blob_num = atoi(value);
	return 0;
}

static int dnet_smack_set_cache_thread_num(struct dnet_config_backend *b, char *key __unused, char *value)
{
	struct smack_backend *s = b->data;

	s->ictl.cache_thread_num = atoi(value);
	return 0;
}

static int dnet_smack_set_sync(struct dnet_config_backend *b, char *key __unused, char *value)
{
	struct smack_backend *s = b->data;

	s->sync = atoi(value);
	return 0;
}

static int dnet_smack_set_type(struct dnet_config_backend *b, char *key __unused, char *value)
{
	struct smack_backend *s = b->data;

	if (s->ictl.type)
		free(s->ictl.type);

	s->ictl.type = strdup(value);
	return 0;
}

static int dnet_smack_set_log(struct dnet_config_backend *b, char *key __unused, char *value)
{
	struct smack_backend *s = b->data;
	char *tmp;

	tmp = strdup(value);
	if (!tmp)
		return -ENOMEM;

	if (s->ictl.log)
		free(s->ictl.log);
	s->ictl.log = tmp;
	return 0;
}

static int dnet_smack_set_root(struct dnet_config_backend *b, char *key __unused, char *root)
{
	struct smack_backend *s = b->data;
	int err;

	err = backend_storage_size(b, root);
	if (err)
		goto err_out_exit;

	s->ictl.path = strdup(root);
	if (!s->ictl.path) {
		err = -ENOMEM;
		goto err_out_exit;
	}

	return 0;

err_out_exit:
	return err;
}

static int smack_backend_send(void *state, void *priv, struct dnet_id *id)
{
	struct dnet_node *n = dnet_get_node_from_state(state);
	struct smack_backend *s = priv;
	struct index idx;
	char *result = NULL;
	char *data;
	int err;

	smack_setup_idx(&idx, id->id);
	err = smack_read(s->smack, &idx, &data);
	if (err)
		goto err_out_exit;

	struct dnet_io_control ctl;

	memset(&ctl, 0, sizeof(ctl));

	ctl.fd = -1;

	ctl.data = data;

	memcpy(&ctl.id, id, sizeof(struct dnet_id));

	ctl.io.offset = 0;
	ctl.io.size = idx.data_size;
	ctl.io.type = 0;
	ctl.io.flags = 0;

	struct dnet_session *sess = dnet_session_create(n);
	dnet_session_set_groups(sess, (int *)&id->group_id, 1);

	err = dnet_write_data_wait(sess, &ctl, (void **)&result);
	if (err < 0)
		goto err_out_free;
	free(result);
	err = 0;

err_out_free:
	free(data);
err_out_exit:
	return err;
}

int smack_backend_storage_stat(void *priv, struct dnet_stat *st)
{
	int err;
	struct smack_backend *s = priv;

	memset(st, 0, sizeof(struct dnet_stat));

	err = backend_stat_low_level(s->ictl.path ? s->ictl.path : ".", st);
	if (err)
		return err;

	return 0;
}

static void dnet_smack_db_cleanup(struct smack_backend *s)
{
	eblob_cleanup(s->meta);
}

static int dnet_smack_db_init(struct smack_backend *s, struct dnet_config *c, const char *path)
{
	static char meta_path[300];
	struct eblob_config ecfg;
	int err = 0;

	snprintf(meta_path, sizeof(meta_path), "%s/meta", path);

	memset(&ecfg, 0, sizeof(ecfg));
	ecfg.file = meta_path;
	ecfg.sync = 300;
	ecfg.blob_flags = EBLOB_RESERVE_10_PERCENTS | EBLOB_TRY_OVERWRITE | EBLOB_NO_FOOTER;
	ecfg.defrag_percentage = 25;
	ecfg.defrag_timeout = 3600;
	ecfg.log = (struct eblob_log *)c->log;

	s->meta = eblob_init(&ecfg);
	if (!s->meta) {
		err = -EINVAL;
		dnet_backend_log(DNET_LOG_ERROR, "Failed to initialize metadata eblob\n");
	}

	return err;
}

static void smack_backend_cleanup(void *priv)
{
	struct smack_backend *s = priv;

	smack_cleanup(s->smack);
	dnet_smack_db_cleanup(s);
	free(s->ictl.path);
}

static ssize_t dnet_smack_db_read(void *priv, struct dnet_raw_id *id, void **datap)
{
	struct smack_backend *s = priv;
	return dnet_db_read_raw(s->meta, id, datap);
}

static int dnet_smack_db_write(void *priv, struct dnet_raw_id *id, void *data, size_t size)
{
	struct smack_backend *s = priv;
	return dnet_db_write_raw(s->meta, id, data, size);
}

static int dnet_smack_db_remove(void *priv, struct dnet_raw_id *id, int real_del)
{
	struct smack_backend *s = priv;
	return dnet_db_remove_raw(s->meta, id, real_del);
}

static int dnet_smack_db_iterate(struct dnet_iterate_ctl *ctl)
{
	struct smack_backend *s = ctl->iterate_private;
	return dnet_db_iterate(s->meta, ctl);
}

static long long smack_total_elements(void *priv)
{
	struct smack_backend *s = priv;

	return smack_total_num(s->smack);
}

static int dnet_smack_config_init(struct dnet_config_backend *b, struct dnet_config *c)
{
	struct smack_backend *s = b->data;
	char *hpath;
	int err, hlen;

	hlen = strlen(s->ictl.path) + 128; /* 128 is for '/history' suffix */
	hpath = alloca(hlen);
	if (!hpath) {
		err = -ENOMEM;
		goto err_out_exit;
	}

	snprintf(hpath, hlen, "%s/history", s->ictl.path);

	c->cb = &b->cb;

	b->cb.command_private = s;

	b->cb.command_handler = smack_backend_command_handler;
	b->cb.send = smack_backend_send;

	c->storage_size = b->storage_size;
	c->storage_free = b->storage_free;

	b->cb.storage_stat = smack_backend_storage_stat;
	b->cb.backend_cleanup = smack_backend_cleanup;
	b->cb.checksum = smack_backend_checksum;

	b->cb.meta_read = dnet_smack_db_read;
	b->cb.meta_write = dnet_smack_db_write;
	b->cb.meta_remove = dnet_smack_db_remove;
	b->cb.meta_total_elements = smack_total_elements;
	b->cb.meta_iterate = dnet_smack_db_iterate;

	mkdir(hpath, 0755);
	err = dnet_smack_db_init(s, c, hpath);
	if (err)
		goto err_out_exit;

	s->ictl.log_level = c->log->log_level;
	s->smack = smack_init(&s->ictl, &err);
	if (!s->smack)
		goto err_out_cleanup;

	return 0;

err_out_cleanup:
	dnet_smack_db_cleanup(s);
err_out_exit:
	return err;
}

static void dnet_smack_config_cleanup(struct dnet_config_backend *b)
{
	struct smack_backend *s = b->data;

	smack_backend_cleanup(s);
}

static struct dnet_config_entry dnet_cfg_entries_smacksystem[] = {
	{"type", dnet_smack_set_type},
	{"log", dnet_smack_set_log},
	{"sync", dnet_smack_set_sync},
	{"root", dnet_smack_set_root},
	{"cache_size", dnet_smack_set_cache_size},
	{"bloom_size", dnet_smack_set_bloom_size},
	{"blob_num", dnet_smack_set_blob_num},
	{"cache_thread_num", dnet_smack_set_cache_thread_num},
};

static struct dnet_config_backend dnet_smack_backend = {
	.name			= "smack",
	.ent			= dnet_cfg_entries_smacksystem,
	.num			= ARRAY_SIZE(dnet_cfg_entries_smacksystem),
	.size			= sizeof(struct smack_backend),
	.init			= dnet_smack_config_init,
	.cleanup		= dnet_smack_config_cleanup,
};

int dnet_smack_backend_init(void)
{
	return dnet_backend_register(&dnet_smack_backend);
}

void dnet_smack_backend_exit(void)
{
	/* cleanup routing will be called explicitly through backend->cleanup() callback */
}
