/**
 *		Tempesta DB
 *
 * This is the entry point: initialization functions and public interfaces.
 *
 * Copyright (C) 2014 NatSys Lab. (info@natsys-lab.com).
 * Copyright (C) 2015-2024 Tempesta Technologies, Inc.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59
 * Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */
#include <linux/module.h>
#include <linux/slab.h>

#include "file.h"
#include "htrie.h"
#include "table.h"
#include "tdb_if.h"

#define TDB_VERSION	"0.2.0"

MODULE_AUTHOR("Tempesta Technologies");
MODULE_DESCRIPTION("Tempesta DB");
MODULE_VERSION(TDB_VERSION);
MODULE_LICENSE("GPL");

/**
 * Create TDB entry and copy @len contiguous bytes from @data to the entry.
 *
 * Returns complete entry, such entry can't be modified or filled with data
 * w/o locking.
 *
 * The user must call tdb_rec_put() when finish with the record.
 */
TdbRec *
tdb_entry_create(TDB *db, unsigned long key, void *data, size_t *len)
{
	TdbRec *r;

	BUG_ON(!data);
	r = tdb_htrie_insert(db->hdr, key, data, NULL, NULL, len, true);
	if (!r)
		TDB_ERR("Cannot create db entry for %.*s, key=%#lx\n",
			(int)*len, (char *)data, key);

	return r;
}
EXPORT_SYMBOL(tdb_entry_create);

/**
 * Create TDB entry to store @len bytes, if there is entry that equals to new
 * one it will be removed, new record will be placed to its place.
 *
 * @eq_cb and @eq_data are used to compare records.
 *
 * NOTE: Returns incomplete record. When modifying of current record is finished
 * need to mark record as complete using tdb_entry_mark_complete(). Incomplete
 * records are invisible for lookup and remove.
 *
 * The user must call tdb_rec_put() when finish with the record.
 *
 * TODO #515 function must holds a lock upon return.
 */
TdbRec *
tdb_entry_alloc_unique(TDB *db, unsigned long key, size_t *len,
		       tdb_eq_cb_t *eq_cb, void *eq_data)
{
	TdbRec *r;

	/* Use tdb_entry_create() for small records, they always complete.*/
	BUG_ON(*len < TDB_HTRIE_MINDREC);
	r = tdb_htrie_insert(db->hdr, key, NULL, eq_cb, eq_data, len, false);
	if (!r)
		TDB_ERR("Cannot allocate db entry for key=%#lx\n", key);

	return r;
}
EXPORT_SYMBOL(tdb_entry_alloc_unique);

/**
 * Create TDB entry to store @len bytes.
 *
 * NOTE: Returns incomplete entry. When modifying of current entry is finished
 * need to mark entry as complete using tdb_entry_mark_complete(). Incomplete
 * entries are invisible for lookup and remove.
 *
 * The user must call tdb_rec_put() when finish with the record.
 *
 * TODO #515 function must holds a lock upon return.
 */
TdbRec *
tdb_entry_alloc(TDB *db, unsigned long key, size_t *len)
{
	TdbRec *r;

	/* Use tdb_entry_create() for small records, they always complete.*/
	BUG_ON(*len < TDB_HTRIE_MINDREC);
	r = tdb_htrie_insert(db->hdr, key, NULL, NULL, NULL, len, false);
	if (!r)
		TDB_ERR("Cannot allocate db entry for key=%#lx\n", key);

	return r;
}
EXPORT_SYMBOL(tdb_entry_alloc);

/*
 * Return true if @rec is complete.
 */
bool
tdb_entry_is_complete(void *rec)
{
	return tdb_rec_is_complete(rec);
}
EXPORT_SYMBOL(tdb_entry_is_complete);

/*
 * Mark TDB record as complete. Incomplete records are invisible for lookup
 * and remove. Small records are always complete.
 */
void
tdb_entry_mark_complete(void *rec)
{
	tdb_rec_mark_complete(rec);
}
EXPORT_SYMBOL(tdb_entry_mark_complete);

/**
 * @return pointer to free area of size at least @size bytes or allocate
 * a new record and link it with the current one.
 *
 * TODO update @size to actually allocated space.
 */
TdbVRec *
tdb_entry_add(TDB *db, TdbVRec *r, size_t size)
{
	return tdb_htrie_extend_rec(db->hdr, r, size);
}
EXPORT_SYMBOL(tdb_entry_add);

/**
 * Remove TDB entries by @key using @eq_cb for comparing entry with @data.
 *
 * @force - Force delete incomplete record.
 */
void
tdb_entry_remove(TDB *db, unsigned long key, tdb_eq_cb_t *eq_cb, void *data,
		 bool force)
{
	tdb_htrie_remove(db->hdr, key, eq_cb, data, force);
}
EXPORT_SYMBOL(tdb_entry_remove);

/**
 * Check available room in @trec and allocate new record if it's not enough.
 * Chop tail of @trec if we allocated more space, but can't use the tail
 * w/o data fragmentation.
 */
void *
tdb_entry_get_room(TDB *db, TdbVRec **r, char *curr_ptr, size_t tail_len,
		   size_t tot_size)
{
	if (likely((*r)->data + (*r)->len - curr_ptr >= tail_len))
		return curr_ptr;

	(*r)->len = curr_ptr - (*r)->data;

	*r = tdb_htrie_extend_rec(db->hdr, *r, tot_size);
	return *r ? (*r)->data : NULL;
}
EXPORT_SYMBOL(tdb_entry_get_room);

/**
 * Lookup and get a record.
 * Since we don't copy returned records, we have to refcount the record
 * the user must call tdb_rec_put() when finish with the record.
 *
 * @return pointer to record with incremented reference counter if the record is
 * found and NULL otherwise.
 */
TdbIter
tdb_rec_get(TDB *db, unsigned long key)
{
	TdbIter iter = { NULL };

	iter.bckt = tdb_htrie_lookup(db->hdr, key);
	if (!iter.bckt)
		goto out;

	iter.rec = tdb_htrie_bscan_for_rec(db->hdr, (TdbBucket **)&iter.bckt,
					   key);

out:
	return iter;
}
EXPORT_SYMBOL(tdb_rec_get);

/**
 * Get next record from full key collision chain.
 */
void
tdb_rec_next(TDB *db, TdbIter *iter)
{
	BUG_ON(!iter->bckt);

	iter->rec = tdb_htrie_next_rec(db->hdr, iter->rec,
				       (TdbBucket **)&iter->bckt,
				       iter->rec->key);
}
EXPORT_SYMBOL(tdb_rec_next);

/* Decrements reference counter. */
void
tdb_rec_put(TDB *db, void *rec)
{
	tdb_htrie_put_rec(db->hdr, (TdbRec *)rec);
}
EXPORT_SYMBOL(tdb_rec_put);

/* Increments reference counter. */
void
tdb_rec_keep(void *rec)
{
	tdb_htrie_get_rec((TdbRec *)rec);
}
EXPORT_SYMBOL(tdb_rec_keep);

int
tdb_info(char *buf, size_t len)
{
	int n;

	n = snprintf(buf, len,
		     "\nTempesta DB version: %s\n"
		     "Open tables: ",
		     TDB_VERSION);
	if (n <= 0)
		return n;

	n += tdb_tbl_print_all(buf + n, len - n);

	buf[n - 1] = '\n';

	return n;
}

/**
 * Search for already opened handler for the database or allocate a new one.
 *
 * The path to table must end with table name (not more than TDB_TBLNAME_LEN
 * characters in long) followed by TDB_SUFFIX.
 */
static TDB *
tdb_get_db(const char *path, int node)
{
	int full_len, len;
	char *slash;
	char tbl_nname[TDB_TBLNAME_LEN + 2] = {};
	TDB *db;

	BUG_ON(node > 9);

	full_len = strlen(path);
	if (strncmp(path + full_len - sizeof(TDB_SUFFIX) + 1,
		    TDB_SUFFIX, sizeof(TDB_SUFFIX) - 1))
	{
		TDB_ERR("Bad table suffix for %s\n", path);
		return NULL;
	}
	slash = strrchr(path, '/');
	if (!slash) {
		TDB_ERR("Please specify absolute path to %s\n", path);
		return NULL;
	}
	len = full_len - (slash - path) - sizeof(TDB_SUFFIX);
	BUG_ON(len < 0);
	/* We'll need to fit <name><numa_id>.tdb */
	if (len + sizeof(TDB_SUFFIX) >= TDB_TBLNAME_LEN) {
		TDB_ERR("Too long table name %s (%d instead of %lu)\n",
			path, len, TDB_TBLNAME_LEN - sizeof(TDB_SUFFIX) - 1);
		return NULL;
	}

	snprintf(tbl_nname, TDB_TBLNAME_LEN, "%.*s%d", len, slash + 1, node);
	db = tdb_tbl_lookup(tbl_nname, len + 1);
	if (db)
		return db;

	db = kzalloc(sizeof(TDB), GFP_KERNEL);
	if (!db) {
		TDB_ERR("Cannot allocate new db handler\n");
		return NULL;
	}

	snprintf(db->path, TDB_PATH_LEN, "%.*s%d.tdb",
		 (int)(full_len - sizeof(TDB_SUFFIX) + 1), path, node);
	snprintf(db->tbl_name, TDB_TBLNAME_LEN, "%.*s%d.tdb",
		 len, slash + 1, node);

	return tdb_get(db);
}

/**
 * Lookup and get a record if the record is found or create TDB entry to store
 * @len bytes. If record exist then since we don't copy returned records,
 * we have to refcount the record the user must call tdb_rec_put()
 * when finish with the record.
 *
 * @return pointer to record with incremented reference counter if the record is
 * found and create TDB entry with incremented refcounter otherwise.
 *
 * TODO #515 rework the function in lock-free way.
 * TODO #515 TDB must be extended to support small records with constant memory
 * address.
 */
TdbRec *
tdb_rec_get_alloc(TDB *db, unsigned long key, TdbGetAllocCtx *ctx)
{
	TdbIter iter;
	TdbRec *r;

	spin_lock(&db->ga_lock);

	ctx->is_new = false;
	iter = tdb_rec_get(db, key);
	while (!TDB_ITER_BAD(iter)) {
		if (ctx->eq_rec(iter.rec,  ctx->ctx)) {
			spin_unlock(&db->ga_lock);
			return iter.rec;
		}
		tdb_rec_next(db, &iter);
	}

	if (ctx->precreate_rec && ctx->precreate_rec(ctx->ctx)) {
		spin_unlock(&db->ga_lock);
		return NULL;
	}
	ctx->is_new = true;
	r = tdb_entry_alloc(db, key, &ctx->len);
	if (!r) {
		spin_unlock(&db->ga_lock);
		return r;
	}
	ctx->init_rec(r, ctx->ctx);
	tdb_entry_mark_complete(r);

	spin_unlock(&db->ga_lock);

	return r;
}
EXPORT_SYMBOL(tdb_rec_get_alloc);

int
tdb_entry_walk(TDB *db, int (*fn)(void *))
{
	return tdb_htrie_walk(db->hdr, fn);
}
EXPORT_SYMBOL(tdb_entry_walk);

/**
 * Open database file and @return its descriptor.
 * If the database is already opened, then returns the handler.
 *
 * The function must not be called from softirq!
 */
TDB *
tdb_open(const char *path, size_t fsize, unsigned int rec_size, int node)
{
	TDB *db;

	if ((fsize & ~TDB_EXT_MASK) || fsize < TDB_EXT_SZ) {
		TDB_ERR("Bad table size: %lu\n", fsize);
		return NULL;
	}

	db = tdb_get_db(path, node);
	if (!db)
		return NULL;

	db->node = node;

	if (tdb_file_open(db, fsize)) {
		TDB_ERR("Cannot open db for %s\n", path);
		goto err;
	}

	db->hdr = tdb_htrie_init(db->hdr, db->filp->f_inode->i_size, rec_size);
	if (!db->hdr) {
		TDB_ERR("Cannot initialize db header\n");
		goto err_init;
	}

	tdb_tbl_enumerate(db);
	spin_lock_init(&db->ga_lock);

	TDB_LOG("Opened table %s: size=%lu rec_size=%u base=%p\n",
		db->path, fsize, rec_size, db->hdr);

	return db;
err_init:
	tdb_file_close(db);
err:
	tdb_put(db);
	return NULL;
}
EXPORT_SYMBOL(tdb_open);

static void
__do_close_table(TDB *db)
{
	/* Unmapping can be done from process context. */
	tdb_file_close(db);

	tdb_htrie_exit(db->hdr);

	TDB_LOG("Close table '%s'\n", db->tbl_name);

	kfree(db);
}

void
tdb_close(TDB *db)
{
	if (!db)
		return;

	if (!atomic_dec_and_test(&db->count))
		return;

	tdb_tbl_forget(db);

	__do_close_table(db);
}
EXPORT_SYMBOL(tdb_close);

static int __init
tdb_init(void)
{
	int r;

	TDB_LOG("Start Tempesta DB\n");

	r = tdb_init_mappings();
	if (r)
		return r;

	r = tdb_if_init();
	if (r)
		return r;

	return 0;
}

static void __exit
tdb_exit(void)
{
	TDB_LOG("Shutdown Tempesta DB\n");

	tdb_if_exit();

	/*
	 * There are no database users, so roughly close all abandoned
	 * tables w/o reference checking and so on.
	 */
	tdb_tbl_foreach(__do_close_table);
}

module_init(tdb_init);
module_exit(tdb_exit);
