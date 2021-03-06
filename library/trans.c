/*
 * 2008+ Copyright (c) Evgeniy Polyakov <zbr@ioremap.net>
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

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "elliptics.h"
#include "elliptics/packet.h"
#include "elliptics/interface.h"

static inline int dnet_trans_cmp(uint64_t old, uint64_t new)
{
	if (old > new)
		return 1;
	if (old < new)
		return -1;
	return 0;
}

struct dnet_trans *dnet_trans_search(struct rb_root *root, uint64_t trans)
{
	struct rb_node *n = root->rb_node;
	struct dnet_trans *t = NULL;
	int cmp = 1;

	while (n) {
		t = rb_entry(n, struct dnet_trans, trans_entry);

		cmp = dnet_trans_cmp(t->trans, trans);
		if (cmp < 0)
			n = n->rb_left;
		else if (cmp > 0)
			n = n->rb_right;
		else
			return dnet_trans_get(t);
	}

	return NULL;
}

int dnet_trans_insert_nolock(struct rb_root *root, struct dnet_trans *a)
{
	struct rb_node **n = &root->rb_node, *parent = NULL;
	struct dnet_trans *t;
	int cmp;

	while (*n) {
		parent = *n;

		t = rb_entry(parent, struct dnet_trans, trans_entry);

		cmp = dnet_trans_cmp(t->trans, a->trans);
		if (cmp < 0)
			n = &parent->rb_left;
		else if (cmp > 0)
			n = &parent->rb_right;
		else
			return -EEXIST;
	}

	if (a->st && a->st->n)
		dnet_log(a->st->n, DNET_LOG_NOTICE, "%s: added transaction: %llu -> %s.\n",
			dnet_dump_id(&a->cmd.id), (unsigned long long)a->trans,
			dnet_server_convert_dnet_addr(&a->st->addr));

	rb_link_node(&a->trans_entry, parent, n);
	rb_insert_color(&a->trans_entry, root);
	return 0;
}

void dnet_trans_remove_nolock(struct rb_root *root, struct dnet_trans *t)
{
	if (!t->trans_entry.rb_parent_color) {
		if (t->st && t->st->n)
			dnet_log(t->st->n, DNET_LOG_ERROR, "%s: trying to remove standalone transaction %llu.\n",
				dnet_dump_id(&t->cmd.id), (unsigned long long)t->trans);
		return;
	}

	if (t) {
		rb_erase(&t->trans_entry, root);
		t->trans_entry.rb_parent_color = 0;
	}
}

void dnet_trans_remove(struct dnet_trans *t)
{
	struct dnet_net_state *st = t->st;

	pthread_mutex_lock(&st->trans_lock);
	dnet_trans_remove_nolock(&st->trans_root, t);
	list_del_init(&t->trans_list_entry);
	pthread_mutex_unlock(&st->trans_lock);
}

struct dnet_trans *dnet_trans_alloc(struct dnet_node *n __unused, uint64_t size)
{
	struct dnet_trans *t;

	t = malloc(sizeof(struct dnet_trans) + size);
	if (!t)
		goto err_out_exit;

	memset(t, 0, sizeof(struct dnet_trans) + size);

	atomic_init(&t->refcnt, 1);
	INIT_LIST_HEAD(&t->trans_list_entry);

	gettimeofday(&t->start, NULL);

	return t;

err_out_exit:
	return NULL;
}

void dnet_trans_destroy(struct dnet_trans *t)
{
	struct dnet_net_state *st = NULL;
	struct timeval tv;
	long diff;

	if (!t)
		return;

	gettimeofday(&tv, NULL);
	diff = 1000000 * (tv.tv_sec - t->start.tv_sec) + (tv.tv_usec - t->start.tv_usec);

	if (t->st && t->st->n) {
		st = t->st;

		pthread_mutex_lock(&st->trans_lock);
		list_del_init(&t->trans_list_entry);
		pthread_mutex_unlock(&st->trans_lock);

		if (t->trans_entry.rb_parent_color)
			dnet_trans_remove(t);
	} else if (!list_empty(&t->trans_list_entry)) {
		assert(0);
	}

	if (t->complete) {
		t->cmd.flags |= DNET_FLAGS_DESTROY;
		t->complete(t->st, &t->cmd, t->priv);
	}

	if (st && (t->cmd.status == 0) &&
			((t->command == DNET_CMD_READ) || (t->command == DNET_CMD_LOOKUP))) {

		if (diff < st->median_read_time && st->weight < DNET_STATE_MAX_WEIGHT)
			st->weight *= 1.1;
		else if (diff > st->median_read_time && st->weight > 1)
			st->weight *= 0.8;

		st->median_read_time = (st->median_read_time + diff) / 2;
	}

	if (st && st->n && t->command != 0) {
		char str[64];
		struct tm tm;

		localtime_r((time_t *)&t->start.tv_sec, &tm);
		strftime(str, sizeof(str), "%F %R:%S", &tm);

		dnet_log(st->n, DNET_LOG_INFO, "%s: destruction %s trans: %llu, reply: %d, st: %s, weight: %f, mrt: %ld, time: %ld, started: %s.%06lu, cached status: %d.\n",
			dnet_dump_id(&t->cmd.id),
			dnet_cmd_string(t->command),
			(unsigned long long)(t->trans & ~DNET_TRANS_REPLY),
			!!(t->trans & ~DNET_TRANS_REPLY),
			dnet_state_dump_addr(t->st),
			st->weight, st->median_read_time, diff,
			str, t->start.tv_usec,
			t->cmd.status);
	}


	dnet_state_put(t->st);
	dnet_state_put(t->orig);

	free(t);
}

int dnet_trans_alloc_send_state(struct dnet_net_state *st, struct dnet_trans_control *ctl)
{
	struct dnet_io_req req;
	struct dnet_node *n = st->n;
	struct dnet_cmd *cmd;
	struct dnet_trans *t;
	int err;

	t = dnet_trans_alloc(n, sizeof(struct dnet_cmd) + ctl->size);
	if (!t) {
		err = -ENOMEM;
		if (ctl->complete)
			ctl->complete(NULL, NULL, ctl->priv);
		goto err_out_exit;
	}

	t->complete = ctl->complete;
	t->priv = ctl->priv;

	cmd = (struct dnet_cmd *)(t + 1);

	memcpy(&cmd->id, &ctl->id, sizeof(struct dnet_id));
	cmd->flags = ctl->cflags;
	cmd->size = ctl->size;

	memcpy(&t->cmd, cmd, sizeof(struct dnet_cmd));

	cmd->cmd = t->command = ctl->cmd;

	if (ctl->size && ctl->data)
		memcpy(cmd + 1, ctl->data, ctl->size);

	cmd->trans = t->rcv_trans = t->trans = atomic_inc(&n->trans);

	dnet_convert_cmd(cmd);

	t->st = dnet_state_get(st);

	memset(&req, 0, sizeof(req));
	req.st = st;
	req.header = cmd;
	req.hsize = sizeof(struct dnet_cmd) + ctl->size;

	dnet_log(n, DNET_LOG_INFO, "%s: alloc/send %s trans: %llu -> %s %f.\n",
			dnet_dump_id(&cmd->id),
			dnet_cmd_string(ctl->cmd),
			(unsigned long long)t->trans,
			dnet_server_convert_dnet_addr(&t->st->addr), t->st->weight);

	err = dnet_trans_send(t, &req);
	if (err)
		goto err_out_put;

	return 0;

err_out_put:
	dnet_trans_put(t);
err_out_exit:
	return err;
}

int dnet_trans_alloc_send(struct dnet_session *s, struct dnet_trans_control *ctl)
{
	struct dnet_node *n = s->node;
	struct dnet_net_state *st;
	int err;

	st = dnet_state_get_first(n, &ctl->id);
	if (!st) {
		err = -ENOENT;
		if (ctl->complete)
			ctl->complete(NULL, NULL, ctl->priv);
		goto err_out_exit;
	}

	err = dnet_trans_alloc_send_state(st, ctl);
	dnet_state_put(st);

err_out_exit:
	return err;
}

static void dnet_trans_check_stall(struct dnet_net_state *st)
{
	struct dnet_trans *t;
	struct timeval tv;
	int trans_timeout = 0;

	gettimeofday(&tv, NULL);

	pthread_mutex_lock(&st->trans_lock);
	list_for_each_entry(t, &st->trans_list, trans_list_entry) {
		if (t->time.tv_sec >= tv.tv_sec)
			break;

		dnet_log(st->n, DNET_LOG_ERROR, "%s: trans: %llu TIMEOUT\n", dnet_state_dump_addr(st), (unsigned long long)t->trans);
		trans_timeout++;
	}
	pthread_mutex_unlock(&st->trans_lock);

	if (trans_timeout) {
		st->stall++;

		if (st->weight >= 2)
			st->weight /= 2;

		dnet_log(st->n, DNET_LOG_ERROR, "%s: TIMEOUT: transactions: %d, stall counter: %d, weight: %f\n",
				dnet_state_dump_addr(st), trans_timeout, st->stall, st->weight);
		if (st->stall >= st->n->stall_count) {
			shutdown(st->read_s, 2);
			shutdown(st->write_s, 2);

			dnet_state_remove_nolock(st);
		} else {
			dnet_schedule_recv(st);
			dnet_schedule_send(st);
		}
	} else {
		st->stall = 0;

		if (st->weight < DNET_STATE_MAX_WEIGHT)
			st->weight *= 1.2;

		if (st->stall) {
			dnet_log(st->n, DNET_LOG_INFO, "%s: reseting state stall counter: weight: %f\n",
					dnet_state_dump_addr(st), st->weight);
		}
	}
}

static void dnet_check_all_states(struct dnet_node *n)
{
	struct dnet_net_state *st, *tmp;
	struct dnet_group *g, *gtmp;

	pthread_mutex_lock(&n->state_lock);
	list_for_each_entry_safe(g, gtmp, &n->group_list, group_entry) {
		list_for_each_entry_safe(st, tmp, &g->state_list, state_entry) {
			dnet_trans_check_stall(st);
		}
	}
	pthread_mutex_unlock(&n->state_lock);
}

static int dnet_check_route_table(struct dnet_node *n)
{
	int rnd = rand();
	struct dnet_id id;
	int groups[128];
	int group_num = 0, i;
	struct dnet_net_state *st;
	struct dnet_group *g;

	pthread_mutex_lock(&n->state_lock);
	list_for_each_entry(g, &n->group_list, group_entry) {
		groups[group_num++] = g->group_id;

		if (group_num > (int)ARRAY_SIZE(groups))
			break;
	}
	pthread_mutex_unlock(&n->state_lock);

	for (i = 0; i < group_num; ++i) {
		id.group_id = groups[i];
		memcpy(id.id, &rnd, sizeof(rnd));

		st = dnet_state_get_first(n, &id);
		if (st) {
			dnet_recv_route_list(st);
			dnet_state_put(st);
		}
	}

	return 0;
}

static void *dnet_check_process(void *data)
{
	struct dnet_node *n = data;
	long i, timeout, wait_for_stall;
	struct timeval tv1, tv2;
	int checks = 0, route_table_checks = 3;

	dnet_set_name("check");

	if (!n->check_timeout)
		n->check_timeout = 10;

	dnet_log(n, DNET_LOG_INFO, "Started checking thread. Timeout: %lu seconds.\n",
			n->check_timeout);

	while (!n->need_exit) {
		gettimeofday(&tv1, NULL);
		dnet_try_reconnect(n);
		if (++checks == route_table_checks) {
			checks = 0;
			dnet_check_route_table(n);
		}

		dnet_discovery(n);
		gettimeofday(&tv2, NULL);

		timeout = n->check_timeout - (tv2.tv_sec - tv1.tv_sec);
		wait_for_stall = n->wait_ts.tv_sec;

		for (i=0; i<timeout; ++i) {
			if (n->need_exit)
				break;

			if (--wait_for_stall == 0) {
				wait_for_stall = n->wait_ts.tv_sec;
				dnet_check_all_states(n);
			}
			sleep(1);
		}
	}

	return NULL;
}

int dnet_check_thread_start(struct dnet_node *n)
{
	int err;

	err = pthread_create(&n->check_tid, NULL, dnet_check_process, n);
	if (err) {
		dnet_log(n, DNET_LOG_ERROR, "Failed to start tree checking thread: err: %d.\n",
				err);
		return -err;
	}

	return 0;
}

void dnet_check_thread_stop(struct dnet_node *n)
{
	pthread_join(n->check_tid, NULL);
	dnet_log(n, DNET_LOG_NOTICE, "Checking thread stopped.\n");
}
