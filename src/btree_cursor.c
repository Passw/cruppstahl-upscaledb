/**
 * Copyright (C) 2005-2007 Christoph Rupp (chris@crupp.de).
 * All rights reserved. See file LICENSE for licence and copyright
 * information.
 *
 * btree cursors - implementation
 *
 */

#include <string.h>
#include "btree_cursor.h"
#include "btree.h"
#include "mem.h"
#include "util.h"
#include "error.h"
#include "blob.h"

static ham_status_t
my_set_to_nil(ham_bt_cursor_t *c)
{
    /*
     * uncoupled cursor: free the cached pointer
     */
    if (bt_cursor_get_flags(c)&BT_CURSOR_FLAG_UNCOUPLED) {
        ham_key_t *key=bt_cursor_get_uncoupled_key(c);
        if (key->data)
            ham_mem_free(key->data);
        ham_mem_free(key);
        bt_cursor_set_uncoupled_key(c, 0);
        bt_cursor_set_flags(c,
                bt_cursor_get_flags(c)&(~BT_CURSOR_FLAG_UNCOUPLED));
    }
    /*
     * coupled cursor: uncouple, remove from page
     */
    else if (bt_cursor_get_flags(c)&BT_CURSOR_FLAG_COUPLED) {
        page_remove_cursor(bt_cursor_get_coupled_page(c),
                (ham_cursor_t *)c);
        bt_cursor_set_flags(c,
                bt_cursor_get_flags(c)&(~BT_CURSOR_FLAG_COUPLED));
    }

    return (0);
}

static ham_status_t
my_move_first(ham_btree_t *be, ham_bt_cursor_t *c, ham_u32_t flags)
{
    ham_status_t st;
    ham_page_t *page;
    btree_node_t *node;
    ham_db_t *db=cursor_get_db(c);

    /*
     * get a NIL cursor
     */
    st=my_set_to_nil(c);
    if (st)
        return (st);

    db_set_error(db, 0);

    /*
     * get the root page
     */
    if (!btree_get_rootpage(be))
        return (db_set_error(db, HAM_KEY_NOT_FOUND));
    page=db_fetch_page(db, btree_get_rootpage(be), flags);
    if (!page)
        return (db_get_error(db));

    /*
     * while we've not reached the leaf: pick the smallest element
     * and traverse down
     */
    while (1) {

        node=ham_page_get_btree_node(page);
        /* check for an empty root page */
        if (btree_node_get_count(node)==0)
            return (db_set_error(db, HAM_KEY_NOT_FOUND));
        /* leave the loop when we've reached the leaf page */
        if (btree_node_is_leaf(node))
            break;

        page=db_fetch_page(db, btree_node_get_ptr_left(node), 0);
        if (!page) {
            if (!db_get_error(db))
                db_set_error(db, HAM_KEY_NOT_FOUND);
            return (db_get_error(db));
        }
    }

    /*
     * couple this cursor to the smallest key in this page
     */
    page_add_cursor(page, (ham_cursor_t *)c);
    bt_cursor_set_coupled_page(c, page);
    bt_cursor_set_coupled_index(c, 0);
    bt_cursor_set_flags(c,
            bt_cursor_get_flags(c)|BT_CURSOR_FLAG_COUPLED);

    return (0);
}

static ham_status_t
my_move_last(ham_btree_t *be, ham_bt_cursor_t *c, ham_u32_t flags)
{
    ham_status_t st;
    ham_page_t *page;
    btree_node_t *node;
    ham_db_t *db=cursor_get_db(c);

    /*
     * get a NIL cursor
     */
    st=my_set_to_nil(c);
    if (st)
        return (st);

    db_set_error(db, 0);

    /*
     * get the root page
     */
    if (!btree_get_rootpage(be))
        return (db_set_error(db, HAM_KEY_NOT_FOUND));
    page=db_fetch_page(db, btree_get_rootpage(be), flags);
    if (!page)
        return (db_get_error(db));

    /*
     * while we've not reached the leaf: pick the largest element
     * and traverse down
     */
    while (1) {
        key_t *key;

        node=ham_page_get_btree_node(page);
        /* check for an empty root page */
        if (btree_node_get_count(node)==0)
            return (db_set_error(db, HAM_KEY_NOT_FOUND));
        /* leave the loop when we've reached a leaf page */
        if (btree_node_is_leaf(node))
            break;

        key=btree_node_get_key(db, node, btree_node_get_count(node)-1);

        page=db_fetch_page(db, key_get_ptr(key), 0);
        if (!page) {
            if (!db_get_error(db))
                db_set_error(db, HAM_KEY_NOT_FOUND);
            return (db_get_error(db));
        }
    }

    /*
     * couple this cursor to the largest key in this page
     */
    page_add_cursor(page, (ham_cursor_t *)c);
    bt_cursor_set_coupled_page(c, page);
    bt_cursor_set_coupled_index(c, btree_node_get_count(node)-1);
    bt_cursor_set_flags(c,
            bt_cursor_get_flags(c)|BT_CURSOR_FLAG_COUPLED);

    return (0);
}

static ham_status_t
my_move_next(ham_btree_t *be, ham_bt_cursor_t *c, ham_u32_t flags)
{
    ham_status_t st;
    ham_page_t *page;
    btree_node_t *node;
    ham_db_t *db=cursor_get_db(c);

    /*
     * uncoupled cursor: couple it
     */
    if (bt_cursor_get_flags(c)&BT_CURSOR_FLAG_UNCOUPLED) {
        st=bt_cursor_couple(c);
        if (st)
            return (st);
    }
    else if (!(bt_cursor_get_flags(c)&BT_CURSOR_FLAG_COUPLED))
        return (HAM_CURSOR_IS_NIL);

    db_set_error(db, 0);

    page=bt_cursor_get_coupled_page(c);
    node=ham_page_get_btree_node(page);

    /*
     * if the index+1 is till in the coupled page, just increment the
     * index
     */
    if (bt_cursor_get_coupled_index(c)+1<btree_node_get_count(node)) {
        bt_cursor_set_coupled_index(c, bt_cursor_get_coupled_index(c)+1);
        return (0);
    }

    /*
     * otherwise uncouple the cursor and load the right sibling page
     */
    page_remove_cursor(page, (ham_cursor_t *)c);
    bt_cursor_set_flags(c, bt_cursor_get_flags(c)&(~BT_CURSOR_FLAG_COUPLED));

    if (!btree_node_get_right(node))
        return (HAM_CURSOR_IS_NIL);

    page=db_fetch_page(db, btree_node_get_right(node), flags);
    if (!page)
        return (db_get_error(db));

    /*
     * couple this cursor to the smallest key in this page
     */
    page_add_cursor(page, (ham_cursor_t *)c);
    bt_cursor_set_coupled_page(c, page);
    bt_cursor_set_coupled_index(c, 0);
    bt_cursor_set_flags(c,
            bt_cursor_get_flags(c)|BT_CURSOR_FLAG_COUPLED);

    return (0);
}

static ham_status_t
my_move_previous(ham_btree_t *be, ham_bt_cursor_t *c, ham_u32_t flags)
{
    ham_status_t st;
    ham_page_t *page;
    btree_node_t *node;
    ham_db_t *db=cursor_get_db(c);

    /*
     * uncoupled cursor: couple it
     */
    if (bt_cursor_get_flags(c)&BT_CURSOR_FLAG_UNCOUPLED) {
        st=bt_cursor_couple(c);
        if (st)
            return (st);
    }
    else if (!(bt_cursor_get_flags(c)&BT_CURSOR_FLAG_COUPLED))
        return (HAM_CURSOR_IS_NIL);


    db_set_error(db, 0);

    page=bt_cursor_get_coupled_page(c);
    node=ham_page_get_btree_node(page);

    /*
     * if the index-1 is till in the coupled page, just decrement the
     * index
     */
    if (bt_cursor_get_coupled_index(c)!=0) {
        bt_cursor_set_coupled_index(c, bt_cursor_get_coupled_index(c)-1);
        return (0);
    }

    /*
     * otherwise uncouple the cursor and load the left sibling page
     */
    page_remove_cursor(page, (ham_cursor_t *)c);
    bt_cursor_set_flags(c, bt_cursor_get_flags(c)&(~BT_CURSOR_FLAG_COUPLED));

    if (!btree_node_get_left(node))
        return (HAM_CURSOR_IS_NIL);

    page=db_fetch_page(db, btree_node_get_left(node), flags);
    if (!page)
        return (db_get_error(db));

    /*
     * couple this cursor to the highest key in this page
     */
    page_add_cursor(page, (ham_cursor_t *)c);
    bt_cursor_set_coupled_page(c, page);
    bt_cursor_set_coupled_index(c, btree_node_get_count(node)-1);
    bt_cursor_set_flags(c,
            bt_cursor_get_flags(c)|BT_CURSOR_FLAG_COUPLED);

    return (0);
}

ham_status_t
bt_cursor_couple(ham_bt_cursor_t *c)
{
    ham_key_t key;
    ham_status_t st;
    ham_db_t *db=cursor_get_db(c);
    ham_txn_t txn;
    ham_bool_t local_txn=db_get_txn(db) ? HAM_FALSE : HAM_TRUE;

    ham_assert(bt_cursor_get_flags(c)&BT_CURSOR_FLAG_UNCOUPLED,
            ("coupling a cursor which is not uncoupled"));

    if (local_txn) {
        st=ham_txn_begin(&txn, db);
        if (st)
            return (st);
    }

    /*
     * make a 'find' on the cached key; if we succeed, the cursor
     * is automatically coupled
     */
    memset(&key, 0, sizeof(key));

    if (!util_copy_key(db, bt_cursor_get_uncoupled_key(c), &key)) {
        if (local_txn)
            (void)ham_txn_abort(&txn);
        return (db_get_error(db));
    }
    
    st=bt_cursor_find(c, &key, 0);
    if (st) {
        if (local_txn)
            (void)ham_txn_abort(&txn);
        return (st);
    }

    /*
     * free the cached key
     */
    if (key.data)
        ham_mem_free(key.data);

    if (local_txn)
        return (ham_txn_commit(&txn));

    return (0);
}

ham_status_t
bt_cursor_uncouple(ham_bt_cursor_t *c, ham_u32_t flags)
{
    ham_status_t st=0;
    btree_node_t *node;
    key_t *entry;
    ham_key_t *key;
    ham_db_t *db=bt_cursor_get_db(c);
    ham_txn_t txn;
    ham_bool_t local_txn=db_get_txn(db) ? HAM_FALSE : HAM_TRUE;

    if (bt_cursor_get_flags(c)&BT_CURSOR_FLAG_UNCOUPLED)
        return (0);
    if (!(bt_cursor_get_flags(c)&BT_CURSOR_FLAG_COUPLED))
        return (0);

    ham_assert(bt_cursor_get_coupled_page(c)!=0,
            ("uncoupling a cursor which has no coupled page"));

    if (local_txn) {
        st=ham_txn_begin(&txn, db);
        if (st)
            return (st);
    }

    /*
     * get the btree-entry of this key
     */
    node=ham_page_get_btree_node(bt_cursor_get_coupled_page(c));
    ham_assert(btree_node_is_leaf(node), ("iterator points to internal node"));
    entry=btree_node_get_key(db, node, bt_cursor_get_coupled_index(c));

    /*
     * copy the key
     */
    key=(ham_key_t *)ham_mem_alloc(sizeof(*key));
    if (!key) {
        if (local_txn)
            (void)ham_txn_abort(&txn);
        return (db_set_error(db, HAM_OUT_OF_MEMORY));
    }
    memset(key, 0, sizeof(*key));
    key=util_copy_key_int2pub(db, entry, key);
    if (!key) {
        if (local_txn)
            (void)ham_txn_abort(&txn);
        return (db_get_error(bt_cursor_get_db(c)));
    }

    /*
     * uncouple the page
     */
    if (!(flags&BT_CURSOR_UNCOUPLE_NO_REMOVE))
        page_remove_cursor(bt_cursor_get_coupled_page(c),
                (ham_cursor_t *)c);

    /*
     * set the flags and the uncoupled key
     */
    bt_cursor_set_flags(c, bt_cursor_get_flags(c)&(~BT_CURSOR_FLAG_COUPLED));
    bt_cursor_set_flags(c, bt_cursor_get_flags(c)|BT_CURSOR_FLAG_UNCOUPLED);
    bt_cursor_set_uncoupled_key(c, key);

    if (local_txn)
        return (ham_txn_commit(&txn));

    return (0);
}

ham_status_t
bt_cursor_create(ham_db_t *db, ham_txn_t *txn, ham_u32_t flags,
            ham_bt_cursor_t **cu)
{
    ham_bt_cursor_t *c;

    (void)flags;

    *cu=0;

    c=(ham_bt_cursor_t *)ham_mem_alloc(sizeof(*c));
    if (!c)
        return (db_set_error(db, HAM_OUT_OF_MEMORY));

    memset(c, 0, sizeof(*c));
    c->_fun_clone=bt_cursor_clone;
    c->_fun_close=bt_cursor_close;
    c->_fun_replace=bt_cursor_replace;
    c->_fun_move=bt_cursor_move;
    c->_fun_find=bt_cursor_find;
    c->_fun_insert=bt_cursor_insert;
    c->_fun_erase=bt_cursor_erase;
    bt_cursor_set_db(c, db);
    bt_cursor_set_txn(c, txn);

    *cu=c;
    return (0);
}

ham_status_t
bt_cursor_clone(ham_bt_cursor_t *oldcu, ham_bt_cursor_t **newcu)
{
    ham_status_t st=0;
    ham_bt_cursor_t *c;
    ham_db_t *db=bt_cursor_get_db(oldcu);
    ham_txn_t txn;
    ham_bool_t local_txn=db_get_txn(db) ? HAM_FALSE : HAM_TRUE;

    *newcu=0;

    c=(ham_bt_cursor_t *)ham_mem_alloc(sizeof(*c));
    if (!c)
        return (db_set_error(db, HAM_OUT_OF_MEMORY));
    memcpy(c, oldcu, sizeof(*c));
    cursor_set_next(c, 0);
    cursor_set_previous(c, 0);

    if (local_txn) {
        st=ham_txn_begin(&txn, db);
        if (st)
            return (st);
    }

    /*
     * if the old cursor is coupled: couple the new cursor, too
     */
    if (bt_cursor_get_flags(oldcu)&BT_CURSOR_FLAG_COUPLED) {
         ham_page_t *page=bt_cursor_get_coupled_page(oldcu);
         page_add_cursor(page, (ham_cursor_t *)c);
         bt_cursor_set_coupled_page(c, page);
    }
    /*
     * if the old cursor is uncoupled: copy the key
     */
    else if (bt_cursor_get_flags(oldcu)&BT_CURSOR_FLAG_UNCOUPLED) {
        ham_key_t *key;

        key=(ham_key_t *)ham_mem_alloc(sizeof(*key));
        if (!key) {
            if (local_txn)
                (void)ham_txn_abort(&txn);
            return (db_set_error(db, HAM_OUT_OF_MEMORY));
        }
        memset(key, 0, sizeof(*key));

        if (!util_copy_key(bt_cursor_get_db(c), 
                bt_cursor_get_uncoupled_key(oldcu), key)) {
            if (key->data)
                ham_mem_free(key->data);
            ham_mem_free(key);
            if (local_txn)
                (void)ham_txn_abort(&txn);
            return (db_get_error(bt_cursor_get_db(c)));
        }
        bt_cursor_set_uncoupled_key(c, key);
    }

    *newcu=c;

    if (local_txn)
        return (ham_txn_commit(&txn));

    return (0);
}

ham_status_t
bt_cursor_close(ham_bt_cursor_t *c)
{
    (void)my_set_to_nil(c);

    ham_mem_free(c);

    return (0);
}

ham_status_t
bt_cursor_replace(ham_bt_cursor_t *c, ham_record_t *record,
            ham_u32_t flags)
{
    ham_status_t st;
    btree_node_t *node;
    key_t *key;
    ham_db_t *db=bt_cursor_get_db(c);
    ham_txn_t txn;
    ham_bool_t local_txn=db_get_txn(db) ? HAM_FALSE : HAM_TRUE;

    if (local_txn) {
        st=ham_txn_begin(&txn, db);
        if (st)
            return (st);
    }

    /*
     * uncoupled cursor: couple it
     */
    if (bt_cursor_get_flags(c)&BT_CURSOR_FLAG_UNCOUPLED) {
        st=bt_cursor_couple(c);
        if (st) {
            if (local_txn)
                (void)ham_txn_abort(&txn);
            return (st);
        }
    }
    else if (!(bt_cursor_get_flags(c)&BT_CURSOR_FLAG_COUPLED)) {
        if (local_txn)
            (void)ham_txn_abort(&txn);
        return (HAM_CURSOR_IS_NIL);
    }

    /*
     * get the btree node entry
     */
    node=ham_page_get_btree_node(bt_cursor_get_coupled_page(c));
    ham_assert(btree_node_is_leaf(node), ("iterator points to internal node"));
    key=btree_node_get_key(db, node, bt_cursor_get_coupled_index(c));

    /*
     * delete the record?
     */
    if (record->size==0) {
        if (!((key_get_flags(key)&KEY_BLOB_SIZE_TINY) ||
            (key_get_flags(key)&KEY_BLOB_SIZE_SMALL) ||
            (key_get_flags(key)&KEY_BLOB_SIZE_EMPTY))) {
            /* remove the cached extended key */
            if (db_get_extkey_cache(db))
                (void)extkey_cache_remove(db_get_extkey_cache(db),
                        key_get_ptr(key));
            (void)blob_free(db, key_get_ptr(key), 0);
        }
        key_set_ptr(key, 0);
        key_set_flags(key, key_get_flags(key)|KEY_BLOB_SIZE_EMPTY);
    }

    /*
     * or replace with a big record?
     */
    else if (record->size>sizeof(ham_offset_t)) {
        ham_offset_t rid;

        /*
         * make sure that we only call blob_replace(), if there IS a blob
         * to replace! otherwise call blob_allocate()
         */
        if (!((key_get_flags(key)&KEY_BLOB_SIZE_TINY) ||
            (key_get_flags(key)&KEY_BLOB_SIZE_SMALL) ||
            (key_get_flags(key)&KEY_BLOB_SIZE_EMPTY))) {
            /* remove the cached extended key */
            if (db_get_extkey_cache(db))
                (void)extkey_cache_remove(db_get_extkey_cache(db),
                        key_get_ptr(key));
            st=blob_replace(db, key_get_ptr(key), record->data,
                        record->size, 0, &rid);
        }
        else
            st=blob_allocate(db, record->data, record->size, 0, &rid);
        if (st) {
            if (local_txn)
                (void)ham_txn_abort(&txn);
            return (st);
        }

        key_set_ptr(key, rid);
    }

    /*
     * or replace with a small record?
     */
    else {
        ham_offset_t rid;

        if (!((key_get_flags(key)&KEY_BLOB_SIZE_TINY) ||
            (key_get_flags(key)&KEY_BLOB_SIZE_SMALL) ||
            (key_get_flags(key)&KEY_BLOB_SIZE_EMPTY))) {
            /* remove the cached extended key */
            if (db_get_extkey_cache(db))
                (void)extkey_cache_remove(db_get_extkey_cache(db),
                        key_get_ptr(key));
            (void)blob_free(db, key_get_ptr(key), 0);
        }

        memcpy(&rid, record->data, record->size);
        if (record->size<sizeof(ham_offset_t)) {
            char *p=(char *)&rid;
            p[sizeof(ham_offset_t)-1]=record->size;
            key_set_flags(key, key_get_flags(key)|KEY_BLOB_SIZE_TINY);
        }
        else {
            key_set_flags(key, key_get_flags(key)|KEY_BLOB_SIZE_SMALL);
        }
    }

    page_set_dirty(bt_cursor_get_coupled_page(c), 1);

    if (local_txn)
        return (ham_txn_commit(&txn));

    return (0);
}

ham_status_t
bt_cursor_move(ham_bt_cursor_t *c, ham_key_t *key,
            ham_record_t *record, ham_u32_t flags)
{
    ham_status_t st=0;
    ham_page_t *page;
    btree_node_t *node;
    ham_db_t *db=bt_cursor_get_db(c);
    ham_btree_t *be=(ham_btree_t *)db_get_backend(db);
    key_t *entry;
    ham_txn_t txn;
    ham_bool_t local_txn=db_get_txn(db) ? HAM_FALSE : HAM_TRUE;

    if (!be)
        return (HAM_INV_INDEX);

    if (local_txn) {
        st=ham_txn_begin(&txn, db);
        if (st)
            return (st);
    }

    /*
     * if the cursor is NIL, and the user requests a NEXT, we set it to FIRST;
     * if the user requests a PREVIOUS, we set it to LAST, resp.
     */
    if (bt_cursor_is_nil(c)) {
        if (flags&HAM_CURSOR_NEXT) {
            flags&=~HAM_CURSOR_NEXT;
            flags|=HAM_CURSOR_FIRST;
        }
        else if (flags&HAM_CURSOR_PREVIOUS) {
            flags&=~HAM_CURSOR_PREVIOUS;
            flags|=HAM_CURSOR_LAST;
        }
    }

    if (flags&HAM_CURSOR_FIRST)
        st=my_move_first(be, c, flags);
    else if (flags&HAM_CURSOR_LAST)
        st=my_move_last(be, c, flags);
    else if (flags&HAM_CURSOR_NEXT)
        st=my_move_next(be, c, flags);
    else if (flags&HAM_CURSOR_PREVIOUS)
        st=my_move_previous(be, c, flags);
    if (st) {
        if (local_txn)
            (void)ham_txn_abort(&txn);
        return (st);
    }

    /*
     * during util_read_key and util_read_record, new pages might be needed,
     * and the page at which we're pointing could be moved out of memory; 
     * that would mean that the cursor would be uncoupled, and we're losing
     * the 'entry'-pointer. therefore we 'lock' the page by incrementing 
     * the inuse-flag.
     */
    ham_assert(bt_cursor_get_flags(c)&BT_CURSOR_FLAG_COUPLED, 
            ("move: cursor is not coupled"));
    page=bt_cursor_get_coupled_page(c);
    page_inc_inuse(page);
    node=ham_page_get_btree_node(page);
    ham_assert(btree_node_is_leaf(node), ("iterator points to internal node"));
    entry=btree_node_get_key(db, node, bt_cursor_get_coupled_index(c));

    if (key) {
        st=util_read_key(db, entry, key, 0);
        if (st) {
            page_dec_inuse(page);
            if (local_txn)
                (void)ham_txn_abort(&txn);
            return (st);
        }
    }

    if (record) {
        record->_rid=key_get_ptr(entry);
        record->_intflags=key_get_flags(entry);
        st=util_read_record(db, record, 0);
        if (st) {
            page_dec_inuse(page);
            if (local_txn)
                (void)ham_txn_abort(&txn);
            return (st);
        }
    }

    page_dec_inuse(page);

    if (local_txn)
        return (ham_txn_commit(&txn));

    return (0);
}

ham_status_t
bt_cursor_find(ham_bt_cursor_t *c, ham_key_t *key, ham_u32_t flags)
{
    ham_status_t st;
    ham_backend_t *be=db_get_backend(bt_cursor_get_db(c));
    ham_txn_t txn;
    ham_db_t *db=cursor_get_db(c);
    ham_bool_t local_txn=db_get_txn(db) ? HAM_FALSE : HAM_TRUE;

    if (!be)
        return (HAM_INV_INDEX);
    if (!key)
        return (HAM_INV_PARAMETER);

    if (local_txn) {
        st=ham_txn_begin(&txn, db);
        if (st)
            return (st);
    }

    st=my_set_to_nil(c);
    if (st) {
        if (local_txn)
            (void)ham_txn_abort(&txn);
        return (st);
    }
    /*
    if (bt_cursor_get_flags(c)&BT_CURSOR_FLAG_COUPLED) {
        page_remove_cursor(bt_cursor_get_coupled_page(c),
                (ham_cursor_t *)c);
        bt_cursor_set_flags(c,
                bt_cursor_get_flags(c)&(~BT_CURSOR_FLAG_COUPLED));
    }
    */

    st=btree_find_cursor((ham_btree_t *)be, c, key, 0, flags);
    if (st) {
        /* cursor is now NIL */
        if (local_txn)
            (void)ham_txn_abort(&txn);
        return (st);
    }

    if (local_txn)
        return (ham_txn_commit(&txn));

    return (0);
}

ham_status_t
bt_cursor_insert(ham_bt_cursor_t *c, ham_key_t *key,
            ham_record_t *record, ham_u32_t flags)
{
    ham_status_t st;
    ham_db_t *db=bt_cursor_get_db(c);
    ham_btree_t *be=(ham_btree_t *)db_get_backend(db);
    ham_txn_t txn;
    ham_bool_t local_txn=db_get_txn(db) ? HAM_FALSE : HAM_TRUE;

    if (!be)
        return (HAM_INV_INDEX);
    if (!key)
        return (HAM_INV_PARAMETER);
    if (!record)
        return (HAM_INV_PARAMETER);

    if (local_txn) {
        st=ham_txn_begin(&txn, db);
        if (st)
            return (st);
    }

    /*
     * set the cursor to nil
     */
    st=my_set_to_nil(c);
    if (st) {
        if (local_txn)
            (void)ham_txn_abort(&txn);
        return (st);
    }

    /*
     * then call the btree insert function
     */
    st=btree_insert_cursor(be, key, record, c, flags);
    if (st) {
        if (local_txn)
            (void)ham_txn_abort(&txn);
        return (st);
    }

    if (local_txn)
        return (ham_txn_commit(&txn));

    return (0);
}

ham_status_t
bt_cursor_erase(ham_bt_cursor_t *c, ham_offset_t *rid,
            ham_u32_t *intflags, ham_u32_t flags)
{
    ham_status_t st;
    ham_db_t *db=bt_cursor_get_db(c);
    ham_btree_t *be=(ham_btree_t *)db_get_backend(db);
    ham_key_t oldkey;
    ham_txn_t txn;
    ham_bool_t local_txn=db_get_txn(db) ? HAM_FALSE : HAM_TRUE;

    if (!be)
        return (HAM_INV_INDEX);

    /*
     * make sure that the cursor is not NIL
     */
    if (bt_cursor_is_nil(c))
        return (HAM_CURSOR_IS_NIL);

    if (local_txn) {
        st=ham_txn_begin(&txn, db);
        if (st)
            return (st);
    }

    /*
     * make a backup of the key, then move the iterator to the next key
     * and delete the old key. 
     */

    memset(&oldkey, 0, sizeof(oldkey));

    if (bt_cursor_get_flags(c)&BT_CURSOR_FLAG_UNCOUPLED) {
        if (!util_copy_key(db, bt_cursor_get_uncoupled_key(c), &oldkey)) {
            if (local_txn)
                (void)ham_txn_abort(&txn);
            return (db_get_error(db));
        }
    }
    else {
        btree_node_t *node;
        key_t *entry;

        node=ham_page_get_btree_node(bt_cursor_get_coupled_page(c));
        ham_assert(btree_node_is_leaf(node), 
                ("iterator points to internal node"));
        entry=btree_node_get_key(db, node, bt_cursor_get_coupled_index(c));

        if (!util_copy_key_int2pub(db, entry, &oldkey)) {
            if (local_txn)
                (void)ham_txn_abort(&txn);
            return (db_get_error(db));
        }
    }

    st=my_move_next(be, c, flags);
    if (st) {
        if (local_txn)
            (void)ham_txn_abort(&txn);
        return (st);
    }

    st=btree_erase(be, &oldkey, rid, intflags, flags);
    if (st) {
        if (local_txn)
            (void)ham_txn_abort(&txn);
        return (st);
    }

    if (local_txn)
        return (ham_txn_commit(&txn));

    return (0);
}

