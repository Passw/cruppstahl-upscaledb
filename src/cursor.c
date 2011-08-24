/*
 * Copyright (C) 2005-2010 Christoph Rupp (chris@crupp.de).
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or 
 * (at your option) any later version.
 *
 * See files COPYING.* for License information.
 *
 */

#include "config.h"

#include <string.h>

#include "cursor.h"
#include "db.h"
#include "env.h"
#include "error.h"
#include "mem.h"
#include "btree_cursor.h"
#include "btree_key.h"


static ham_bool_t
__btree_cursor_is_nil(btree_cursor_t *btc)
{
    return (!btree_cursor_is_coupled(btc) && !btree_cursor_is_uncoupled(btc));
}

static ham_status_t
__dupecache_resize(dupecache_t *c, ham_size_t capacity)
{
    ham_env_t *env=db_get_env(cursor_get_db(dupecache_get_cursor(c)));
    dupecache_line_t *ptr=dupecache_get_elements(c);

    if (capacity==0) {
        dupecache_clear(c);
        return (0);
    }

    ptr=(dupecache_line_t *)allocator_realloc(env_get_allocator(env), 
                    ptr, sizeof(dupecache_line_t)*capacity);
    if (ptr) {
        dupecache_set_capacity(c, capacity);
        dupecache_set_elements(c, ptr);
        return (0);
    }

    return (HAM_OUT_OF_MEMORY);
}

ham_status_t
dupecache_create(dupecache_t *c, struct ham_cursor_t *cursor, 
                    ham_size_t capacity)
{
    memset(c, 0, sizeof(*c));
    dupecache_set_cursor(c, cursor);

    return (__dupecache_resize(c, capacity==0 ? 8 : capacity));
}

ham_status_t
dupecache_clone(dupecache_t *src, dupecache_t *dest)
{
    ham_status_t st;

    *dest=*src;
    dupecache_set_elements(dest, 0);

    if (!dupecache_get_capacity(src))
        return (0);

    st=__dupecache_resize(dest, dupecache_get_capacity(src));
    if (st)
        return (st);

    memcpy(dupecache_get_elements(dest), dupecache_get_elements(src),
            dupecache_get_count(dest)*sizeof(dupecache_line_t));
    return (0);
}

ham_status_t
dupecache_insert(dupecache_t *c, ham_u32_t position, dupecache_line_t *dupe)
{
    ham_status_t st;
    dupecache_line_t *e;

    ham_assert(position<=dupecache_get_count(c), (""));

    /* append or insert in the middle? */
    if (position==dupecache_get_count(c))
        return (dupecache_append(c, dupe));

    /* resize if necessary */
    if (dupecache_get_count(c)>=dupecache_get_capacity(c)-1) {
        st=__dupecache_resize(c, dupecache_get_capacity(c)*2);
        if (st)
            return (st);
    }

    e=dupecache_get_elements(c);

    /* shift elements to the "right" */
    memmove(&e[position+1], &e[position], 
                    sizeof(dupecache_line_t)*(dupecache_get_count(c)-position));
    e[position]=*dupe;
    dupecache_set_count(c, dupecache_get_count(c)+1);

    return (0);
}

ham_status_t
dupecache_append(dupecache_t *c, dupecache_line_t *dupe)
{
    ham_status_t st;
    dupecache_line_t *e;

    /* resize if necessary */
    if (dupecache_get_count(c)>=dupecache_get_capacity(c)-1) {
        st=__dupecache_resize(c, dupecache_get_capacity(c)*2);
        if (st)
            return (st);
    }

    e=dupecache_get_elements(c);

    e[dupecache_get_count(c)]=*dupe;
    dupecache_set_count(c, dupecache_get_count(c)+1);

    return (0);
}

ham_status_t
dupecache_erase(dupecache_t *c, ham_u32_t position)
{
    dupecache_line_t *e=dupecache_get_elements(c);

    ham_assert(position<dupecache_get_count(c), (""));

    if (position<dupecache_get_count(c)-1) {
        /* shift elements to the "left" */
        memmove(&e[position], &e[position+1], 
                sizeof(dupecache_line_t)*(dupecache_get_count(c)-position-1));
    }

    dupecache_set_count(c, dupecache_get_count(c)-1);

    return (0);
}

void
dupecache_clear(dupecache_t *c)
{
    ham_env_t *env;
    if (dupecache_get_cursor(c))
        env=db_get_env(cursor_get_db(dupecache_get_cursor(c)));

    if (dupecache_get_elements(c))
        allocator_free(env_get_allocator(env), dupecache_get_elements(c));

    dupecache_set_elements(c, 0);
    dupecache_set_capacity(c, 0);
    dupecache_set_count(c, 0);
}

void
dupecache_reset(dupecache_t *c)
{
    dupecache_set_count(c, 0);
}

ham_status_t
cursor_update_dupecache(ham_cursor_t *cursor, ham_u32_t what)
{
    ham_status_t st=0;
    ham_db_t *db=cursor_get_db(cursor);
    ham_env_t *env=db_get_env(db);
    dupecache_t *dc=cursor_get_dupecache(cursor);
    btree_cursor_t *btc=cursor_get_btree_cursor(cursor);
    txn_cursor_t *txnc=cursor_get_txn_cursor(cursor);

    ham_assert(db_get_rt_flags(db)&HAM_ENABLE_DUPLICATES, (""));

    /* if the cache already exists: no need to continue, it should be
     * up to date */
    if (dupecache_get_count(dc)!=0)
        return (0);

    /* initialize the dupecache, if it was not yet initialized */
    if (dupecache_get_capacity(dc)==0) {
        st=dupecache_create(dc, cursor, 8);
        if (st)
            return (st);
    }

    if ((what&CURSOR_BTREE) && (what&CURSOR_TXN)) {
        if (cursor_is_nil(cursor, CURSOR_BTREE) 
                && !cursor_is_nil(cursor, CURSOR_TXN)) {
            ham_bool_t equal_keys;
            (void)cursor_sync(cursor, 0, &equal_keys);
            if (!equal_keys)
                cursor_set_to_nil(cursor, CURSOR_BTREE);
        }
    }

    /* first collect all duplicates from the btree. They're already sorted,
     * therefore we can just append them to our duplicate-cache. */
    if ((what&CURSOR_BTREE)
            && !cursor_is_nil(cursor, CURSOR_BTREE)) {
        ham_size_t i;
        ham_bool_t needs_free=HAM_FALSE;
        dupe_table_t *table=0;
        st=btree_cursor_get_duplicate_table(btc, &table, &needs_free);
        if (st && st!=HAM_CURSOR_IS_NIL)
            return (st);
        st=0;
        if (table) {
            for (i=0; i<dupe_table_get_count(table); i++) {
                dupecache_line_t dcl={0};
                dupecache_line_set_btree(&dcl, HAM_TRUE);
                dupecache_line_set_btree_dupe_idx(&dcl, i);
                st=dupecache_append(dc, &dcl);
                if (st) {
                    allocator_free(env_get_allocator(env), table);
                    return (st);
                }
            }
            if (needs_free)
                allocator_free(env_get_allocator(env), table);
        }
        changeset_clear(env_get_changeset(env));
    }

    /* read duplicates from the txn-cursor? */
    if ((what&CURSOR_TXN)
            && !cursor_is_nil(cursor, CURSOR_TXN)) {
        txn_op_t *op=txn_cursor_get_coupled_op(txnc);
        txn_opnode_t *node=txn_op_get_node(op);

        if (!node)
            goto bail;

        /* now start integrating the items from the transactions */
        op=txn_opnode_get_oldest_op(node);

        while (op) {
            ham_txn_t *optxn=txn_op_get_txn(op);
            /* collect all ops that are valid (even those that are 
             * from conflicting transactions) */
            if (!(txn_get_flags(optxn)&TXN_STATE_ABORTED)) {
                /* a normal (overwriting) insert will overwrite ALL dupes,
                 * but an overwrite of a duplicate will only overwrite
                 * an entry in the dupecache */
                if (txn_op_get_flags(op)&TXN_OP_INSERT) {
                    dupecache_line_t dcl={0};
                    dupecache_line_set_btree(&dcl, HAM_FALSE);
                    dupecache_line_set_txn_op(&dcl, op);
                    /* all existing dupes are overwritten */
                    dupecache_reset(dc);
                    st=dupecache_append(dc, &dcl);
                    if (st)
                        return (st);
                }
                else if (txn_op_get_flags(op)&TXN_OP_INSERT_OW) {
                    dupecache_line_t *e=dupecache_get_elements(dc);
                    ham_u32_t ref=txn_op_get_referenced_dupe(op);
                    if (ref) {
                        ham_assert(ref<=dupecache_get_count(dc), (""));
                        dupecache_line_set_txn_op(&e[ref-1], op);
                        dupecache_line_set_btree(&e[ref-1], HAM_FALSE);
                    }
                    else {
                        dupecache_line_t dcl={0};
                        dupecache_line_set_btree(&dcl, HAM_FALSE);
                        dupecache_line_set_txn_op(&dcl, op);
                        /* all existing dupes are overwritten */
                        dupecache_reset(dc);
                        st=dupecache_append(dc, &dcl);
                        if (st)
                            return (st);
                    }
                }
                /* insert a duplicate key */
                else if (txn_op_get_flags(op)&TXN_OP_INSERT_DUP) {
                    ham_u32_t of=txn_op_get_orig_flags(op);
                    ham_u32_t ref=txn_op_get_referenced_dupe(op)-1;
                    dupecache_line_t dcl={0};
                    dupecache_line_set_btree(&dcl, HAM_FALSE);
                    dupecache_line_set_txn_op(&dcl, op);
                    if (of&HAM_DUPLICATE_INSERT_FIRST)
                        st=dupecache_insert(dc, 0, &dcl);
                    else if (of&HAM_DUPLICATE_INSERT_BEFORE) {
                        st=dupecache_insert(dc, ref, &dcl);
                    }
                    else if (of&HAM_DUPLICATE_INSERT_AFTER) {
                        if (ref+1>=dupecache_get_count(dc))
                            st=dupecache_append(dc, &dcl);
                        else
                            st=dupecache_insert(dc, ref+1, &dcl);
                    }
                    else /* default is HAM_DUPLICATE_INSERT_LAST */
                        st=dupecache_append(dc, &dcl);
                    if (st)
                        return (st);
                }
                /* a normal erase will erase ALL duplicate keys */
                else if (txn_op_get_flags(op)&TXN_OP_ERASE) {
                    ham_u32_t ref=txn_op_get_referenced_dupe(op);
                    if (ref) {
                        ham_assert(ref<=dupecache_get_count(dc), (""));
                        st=dupecache_erase(dc, ref-1);
                        if (st)
                            return (st);
                    }
                    else {
                        /* all existing dupes are erased */
                        dupecache_reset(dc);
                    }
                }
                else {
                    /* everything else is a bug! */
                    ham_assert(txn_op_get_flags(op)==TXN_OP_NOP, (""));
                }
            }
    
            /* continue with the previous/older operation */
            op=txn_op_get_next_in_node(op);
        }
    }

bail:

    return (0);
}

void
cursor_clear_dupecache(ham_cursor_t *cursor)
{
    dupecache_reset(cursor_get_dupecache(cursor));
    cursor_set_dupecache_index(cursor, 0);
}

void
cursor_couple_to_dupe(ham_cursor_t *cursor, ham_u32_t dupe_id)
{
    txn_cursor_t *txnc=cursor_get_txn_cursor(cursor);
    dupecache_t *dc=cursor_get_dupecache(cursor);
    dupecache_line_t *e=0;

    ham_assert(dc && dupecache_get_count(dc)>=dupe_id, (""));
    ham_assert(dupe_id>=1, (""));

    /* dupe-id is a 1-based index! */
    e=dupecache_get_elements(dc)+(dupe_id-1);
    if (dupecache_line_use_btree(e)) {
        btree_cursor_t *btc=cursor_get_btree_cursor(cursor);
        cursor_couple_to_btree(cursor);
        btree_cursor_set_dupe_id(btc, dupecache_line_get_btree_dupe_idx(e));
    }
    else {
        txn_cursor_couple(txnc, dupecache_line_get_txn_op(e));
        cursor_couple_to_txnop(cursor);
    }
    cursor_set_dupecache_index(cursor, dupe_id);
}

ham_status_t
cursor_check_if_btree_key_is_erased_or_overwritten(ham_cursor_t *cursor)
{
    ham_key_t key={0};
    ham_cursor_t *clone;
    txn_op_t *op;
    ham_status_t st=ham_cursor_clone(cursor, &clone);
    txn_cursor_t *txnc=cursor_get_txn_cursor(clone);
    if (st)
        return (st);
    st=btree_cursor_move(cursor_get_btree_cursor(cursor), &key, 0, 0);
    if (st) {
        ham_cursor_close(clone);
        return (st);
    }

    st=txn_cursor_find(txnc, &key, 0);
    if (st) {
        ham_cursor_close(clone);
        return (st);
    }

    op=txn_cursor_get_coupled_op(txnc);
    if (txn_op_get_flags(op)&TXN_OP_INSERT_DUP)
        st=HAM_KEY_NOT_FOUND;
    ham_cursor_close(clone);
    return (st);
}

ham_status_t
cursor_sync(ham_cursor_t *cursor, ham_u32_t flags, ham_bool_t *equal_keys)
{
    ham_status_t st=0;
    txn_cursor_t *txnc=cursor_get_txn_cursor(cursor);
    if (equal_keys)
        *equal_keys=HAM_FALSE;

    if (cursor_is_nil(cursor, CURSOR_BTREE)) {
        txn_opnode_t *node;
		ham_key_t *k;
        if (!txn_cursor_get_coupled_op(txnc))
            return (0);
        node=txn_op_get_node(txn_cursor_get_coupled_op(txnc));
        k=txn_opnode_get_key(node);

        if (!(flags&CURSOR_SYNC_ONLY_EQUAL_KEY))
            flags=flags|((flags&HAM_CURSOR_NEXT)
                    ? HAM_FIND_GEQ_MATCH
                    : HAM_FIND_LEQ_MATCH);
        /* the flag DONT_LOAD_KEY does not load the key if there's an
         * approx match - it only positions the cursor */
        st=btree_cursor_find(cursor_get_btree_cursor(cursor), k, 0, 
                CURSOR_SYNC_DONT_LOAD_KEY|flags);
        /* if we had a direct hit instead of an approx. match then
         * set fresh_start to false; otherwise do_local_cursor_move
         * will move the btree cursor again */
        if (st==0 && equal_keys && !ham_key_get_approximate_match_type(k))
            *equal_keys=HAM_TRUE;
    }
    else if (cursor_is_nil(cursor, CURSOR_TXN)) {
        ham_cursor_t *clone;
        ham_key_t *k;
        ham_status_t st=ham_cursor_clone(cursor, &clone);
        if (st)
            goto bail;
        st=btree_cursor_uncouple(cursor_get_btree_cursor(clone), 0);
        if (st) {
            ham_cursor_close(clone);
            goto bail;
        }
        k=btree_cursor_get_uncoupled_key(cursor_get_btree_cursor(clone));
        if (!(flags&CURSOR_SYNC_ONLY_EQUAL_KEY))
            flags=flags|((flags&HAM_CURSOR_NEXT)
                    ? HAM_FIND_GEQ_MATCH
                    : HAM_FIND_LEQ_MATCH);
        st=txn_cursor_find(txnc, k, CURSOR_SYNC_DONT_LOAD_KEY|flags);
        /* if we had a direct hit instead of an approx. match then
        * set fresh_start to false; otherwise do_local_cursor_move
        * will move the btree cursor again */
        if (st==0 && equal_keys && !ham_key_get_approximate_match_type(k))
            *equal_keys=HAM_TRUE;
        ham_cursor_close(clone);
    }

bail:
    return (st);
}

static ham_size_t
__cursor_has_duplicates(ham_cursor_t *cursor)
{
    return (dupecache_get_count(cursor_get_dupecache(cursor))>1);
}

static ham_status_t
__cursor_move_next_dupe(ham_cursor_t *cursor, ham_u32_t flags)
{
    dupecache_t *dc=cursor_get_dupecache(cursor);

    if (cursor_get_dupecache_index(cursor)) {
        if (cursor_get_dupecache_index(cursor)<dupecache_get_count(dc)) {
            cursor_set_dupecache_index(cursor, 
                        cursor_get_dupecache_index(cursor)+1);
            cursor_couple_to_dupe(cursor, 
                        cursor_get_dupecache_index(cursor));
            return (0);
        }
    }
    return (HAM_LIMITS_REACHED);
}

static ham_status_t
__cursor_move_previous_dupe(ham_cursor_t *cursor, ham_u32_t flags)
{
    if (cursor_get_dupecache_index(cursor)) {
        if (cursor_get_dupecache_index(cursor)>1) {
            cursor_set_dupecache_index(cursor, 
                        cursor_get_dupecache_index(cursor)-1);
            cursor_couple_to_dupe(cursor, 
                        cursor_get_dupecache_index(cursor));
            return (0);
        }
    }
    return (HAM_LIMITS_REACHED);
}

static ham_status_t
__cursor_move_first_dupe(ham_cursor_t *cursor, ham_u32_t flags)
{
    dupecache_t *dc=cursor_get_dupecache(cursor);

    if (dupecache_get_count(dc)) {
        cursor_set_dupecache_index(cursor, 1);
        cursor_couple_to_dupe(cursor, 
                    cursor_get_dupecache_index(cursor));
        return (0);
    }
    return (HAM_LIMITS_REACHED);
}

static ham_status_t
__cursor_move_last_dupe(ham_cursor_t *cursor, ham_u32_t flags)
{
    dupecache_t *dc=cursor_get_dupecache(cursor);

    if (dupecache_get_count(dc)) {
        cursor_set_dupecache_index(cursor, 
                    dupecache_get_count(dc));
        cursor_couple_to_dupe(cursor, 
                    cursor_get_dupecache_index(cursor));
        return (0);
    }
    return (HAM_LIMITS_REACHED);
}

static ham_status_t
__cursor_move_next_key(ham_cursor_t *cursor)
{
    return (0);
}

static ham_status_t
__cursor_move_previous_key(ham_cursor_t *cursor)
{
    return (0);
}

static ham_status_t
__compare_cursors(btree_cursor_t *btrc, txn_cursor_t *txnc, int *pcmp)
{
    ham_cursor_t *cursor=btree_cursor_get_parent(btrc);
    ham_db_t *db=cursor_get_db(cursor);

    txn_opnode_t *node=txn_op_get_node(txn_cursor_get_coupled_op(txnc));
    ham_key_t *txnk=txn_opnode_get_key(node);

    ham_assert(!cursor_is_nil(cursor, 0), (""));
    ham_assert(!txn_cursor_is_nil(txnc), (""));

    if (btree_cursor_is_coupled(btrc)) {
        /* clone the cursor, then uncouple the clone; get the uncoupled key
         * and discard the clone again */
        
        /* 
         * TODO TODO TODO
         * this is all correct, but of course quite inefficient, because 
         *    a) new structures have to be allocated/released
         *    b) uncoupling fetches the whole extended key, which is often
         *      not necessary
         *  -> fix it!
         */
        int cmp;
        ham_cursor_t *clone;
        ham_status_t st=ham_cursor_clone(cursor, &clone);
        if (st)
            return (st);
        st=btree_cursor_uncouple(cursor_get_btree_cursor(clone), 0);
        if (st) {
            ham_cursor_close(clone);
            return (st);
        }
        /* TODO error codes are swallowed */
        cmp=db_compare_keys(db, 
                btree_cursor_get_uncoupled_key(cursor_get_btree_cursor(clone)), 
                txnk);
        ham_cursor_close(clone);
        *pcmp=cmp;
        return (0);
    }
    else if (btree_cursor_is_uncoupled(btrc)) {
        /* TODO error codes are swallowed */
        *pcmp=db_compare_keys(db, btree_cursor_get_uncoupled_key(btrc), txnk);
        return (0);
    }

    ham_assert(!"shouldn't be here", (""));
    return (0);
}

static ham_status_t
__cursor_move_first_key(ham_cursor_t *cursor)
{
    ham_status_t st=0, btrs, txns;
    txn_cursor_t *txnc=cursor_get_txn_cursor(cursor);
    btree_cursor_t *btrc=cursor_get_btree_cursor(cursor);

    /* fetch the smallest/first key from the transaction tree. */
    txns=txn_cursor_move(txnc, HAM_CURSOR_FIRST);
    /* fetch the smallest/first key from the btree tree. */
    btrs=btree_cursor_move(btrc, 0, 0, HAM_CURSOR_FIRST);
    /* now consolidate - if both trees are empty then return */
    if (btrs==HAM_KEY_NOT_FOUND && txns==HAM_KEY_NOT_FOUND) {
        return (HAM_KEY_NOT_FOUND);
    }
    /* if btree is empty but txn-tree is not: couple to txn */
    else if (btrs==HAM_KEY_NOT_FOUND && txns==0) {
        cursor_couple_to_txnop(cursor);
        cursor_update_dupecache(cursor, CURSOR_TXN);
    }
    /* if txn-tree is empty but btree is not: couple to btree */
    else if (txns==HAM_KEY_NOT_FOUND && btrs==0) {
        cursor_couple_to_btree(cursor);
        cursor_update_dupecache(cursor, CURSOR_BTREE);
    }
    /* if both trees are not empty then pick the smaller key, but make sure
     * that it was not erased in another transaction.
     *
     * !!
     * if the key has duplicates which were erased then return - dupes
     * are handled by the caller 
     *
     * !!
     * if both keys are equal: make sure that the btree key was not
     * erased in the transaction; otherwise couple to the txn-op
     * (it's chronologically newer and has faster access) 
     */
    else if (btrs==0 
            && (txns==0 
                || txns==HAM_KEY_ERASED_IN_TXN
                || txns==HAM_TXN_CONFLICT)) {
        int cmp;

        st=__compare_cursors(btrc, txnc, &cmp);
        if (st)
            return (st);

        /* both keys are equal */
        if (cmp==0) {
            cursor_couple_to_txnop(cursor);

            /* we have duplicates */
            if (cursor_get_dupecache_count(cursor)) {
                if (txns==HAM_KEY_ERASED_IN_TXN) {
                    cursor_update_dupecache(cursor, CURSOR_BOTH);
                    return (txns);
                }
                /* btree and txn-tree have duplicates of the same key */
                else if (txns==HAM_SUCCESS && btrs==HAM_SUCCESS) {
                    cursor_update_dupecache(cursor, CURSOR_BOTH);
                    return (0);
                }
                else
                    return (txns ? txns : btrs);
            }
            /* otherwise (we do not have duplicates) */
            if (txns==HAM_KEY_ERASED_IN_TXN) {
                /* if this btree key was erased or overwritten then couple
                 * to the txn, but already move the btree cursor to the
                 * next item */
                st=btree_cursor_move(btrc, 0, 0, HAM_CURSOR_NEXT);
                /* if the key was erased: continue moving "next" till 
                 * we find a key or reach the end of the database */
                st=__cursor_move_next_key(cursor);
                if (st==HAM_KEY_ERASED_IN_TXN) {
                    cursor_set_to_nil(cursor, 0);
                    return (HAM_KEY_NOT_FOUND);
                }
                return (st);
            }
            if (txns==HAM_TXN_CONFLICT) {
                return (txns);
            }
            /* if the btree entry was overwritten in the txn: move the
             * btree entry to the next key */
            if (txns==HAM_SUCCESS) {
                st=__cursor_move_next_key(cursor);
                return (st);
            }
        }
        else if (cmp<1) {
            /* couple to btree */
            cursor_couple_to_btree(cursor);
            cursor_update_dupecache(cursor, CURSOR_BTREE);
        }
        else {
            if (txns==HAM_TXN_CONFLICT)
                return (txns);
            /* couple to txn */
            cursor_couple_to_txnop(cursor);
            cursor_update_dupecache(cursor, CURSOR_TXN);
        }
    }

    /* every other error code is returned to the caller */
    if ((btrs==HAM_KEY_NOT_FOUND) && (txns==HAM_KEY_ERASED_IN_TXN))
        cursor_update_dupecache(cursor, CURSOR_TXN); /* TODO required? */
    return (txns ? txns : btrs);
}

static ham_status_t
__cursor_move_last_key(ham_cursor_t *cursor)
{
    return (0);
}

ham_status_t
cursor_move(ham_cursor_t *cursor, ham_key_t *key, ham_record_t *record,
                ham_u32_t flags)
{
    ham_status_t st=0;
    ham_bool_t changed_dir=HAM_FALSE;
    ham_bool_t skip_duplicates=HAM_FALSE;
    ham_db_t *db=cursor_get_db(cursor);
    txn_cursor_t *txnc=cursor_get_txn_cursor(cursor);
    btree_cursor_t *btrc=cursor_get_btree_cursor(cursor);

    if (!(db_get_rt_flags(db)&HAM_ENABLE_DUPLICATES)
            || (flags&HAM_SKIP_DUPLICATES))
        skip_duplicates=HAM_TRUE;

    /* no movement requested? directly retrieve key/record */
    if (!flags)
        goto retrieve_key_and_record;

    /* synchronize the btree and transaction cursor if the last operation was
     * not a move next/previous OR if the direction changed */
    if ((cursor_get_lastop(cursor)==HAM_CURSOR_PREVIOUS)
            && (flags&HAM_CURSOR_NEXT))
        changed_dir=HAM_TRUE;
    else if ((cursor_get_lastop(cursor)==HAM_CURSOR_NEXT)
            && (flags&HAM_CURSOR_PREVIOUS))
        changed_dir=HAM_TRUE;
    if (((flags&HAM_CURSOR_NEXT) || (flags&HAM_CURSOR_PREVIOUS))
            && (cursor_get_lastop(cursor)==CURSOR_LOOKUP_INSERT
                || changed_dir)) {
        st=cursor_sync(cursor, flags, 0);
        if (st)
            goto bail;
    }

    /* should we move through the duplicate list? */
    if (!skip_duplicates) {
        if (flags&HAM_CURSOR_NEXT)
            st=__cursor_move_next_dupe(cursor, flags);
        else if (flags&HAM_CURSOR_PREVIOUS)
            st=__cursor_move_previous_dupe(cursor, flags);
        else if (flags&HAM_CURSOR_FIRST)
            st=__cursor_move_first_dupe(cursor, flags);
        else {
            ham_assert(flags&HAM_CURSOR_LAST, (""));
            st=__cursor_move_last_dupe(cursor, flags);
        }
        if (st==0)
            goto retrieve_key_and_record;
        if (st!=HAM_LIMITS_REACHED)
            return (st);
    }

    /* we have either skipped duplicates or reached the end of the duplicate
     * list. btree cursor and txn cursor are synced and relative close to 
     * each other. Move the cursor in the requested direction. */
    cursor_clear_dupecache(cursor);
    if (flags&HAM_CURSOR_NEXT)
        st=__cursor_move_next_key(cursor);
    else if (flags&HAM_CURSOR_PREVIOUS)
        st=__cursor_move_previous_key(cursor);
    else if (flags&HAM_CURSOR_FIRST)
        st=__cursor_move_first_key(cursor);
    else {
        ham_assert(flags&HAM_CURSOR_LAST, (""));
        st=__cursor_move_last_key(cursor);
    }
    if (st)
        return (st);

    /* now move once more through the duplicate list, if required. Since this
     * key is "fresh" and we have not yet returned any cursor we will start
     * at the beginning or at the end of the duplicate list. */
    if (!skip_duplicates && __cursor_has_duplicates(cursor)) {
        if ((flags&HAM_CURSOR_NEXT) || (flags&HAM_CURSOR_FIRST))
            st=__cursor_move_first_dupe(cursor, flags);
        else {
            ham_assert((flags&HAM_CURSOR_LAST) || (flags&HAM_CURSOR_PREVIOUS), 
                            (""));
            st=__cursor_move_last_dupe(cursor, flags);
        }
        /* all duplicates were erased in a transaction? then move forward
         * or backwards */
        if (st==HAM_LIMITS_REACHED) {
            if (flags&HAM_CURSOR_FIRST) {
                flags&=~HAM_CURSOR_FIRST;
                flags|=HAM_CURSOR_NEXT;
            }
            else if (flags&HAM_CURSOR_LAST) {
                flags&=~HAM_CURSOR_LAST;
                flags|=HAM_CURSOR_PREVIOUS;
            }
            return (cursor_move(cursor, key, record, flags));
        }
        else if (st)
            return (st);
    }

retrieve_key_and_record:
    /* retrieve key/record, if requested */
    if (st==0) {
        if (cursor_is_coupled_to_txnop(cursor)) {
            txn_op_t *op=txn_cursor_get_coupled_op(txnc);
            ham_assert(!(txn_op_get_flags(op)&TXN_OP_ERASE), (""));
            if (key) {
                st=txn_cursor_get_key(txnc, key);
                if (st)
                    goto bail;
            }
            if (record) {
                st=txn_cursor_get_record(txnc, record);
                if (st)
                    goto bail;
            }
        }
        else {
            st=btree_cursor_move(btrc, key, record, 0);
        }
    }

bail:
    return (st);
}

ham_size_t
cursor_get_dupecache_count(ham_cursor_t *cursor)
{
    ham_db_t *db=cursor_get_db(cursor);
    txn_cursor_t *txnc=cursor_get_txn_cursor(cursor);

    if (!(db_get_rt_flags(db)&HAM_ENABLE_DUPLICATES))
        return (HAM_FALSE);

    if (txn_cursor_get_coupled_op(txnc))
        cursor_update_dupecache(cursor, CURSOR_BTREE|CURSOR_TXN);
    else
        cursor_update_dupecache(cursor, CURSOR_BTREE);

    return (dupecache_get_count(cursor_get_dupecache(cursor)));
}

ham_status_t
cursor_create(ham_db_t *db, ham_txn_t *txn, ham_u32_t flags,
            ham_cursor_t **pcursor)
{
    ham_env_t *env = db_get_env(db);
    ham_cursor_t *c;

    *pcursor=0;

    c=(ham_cursor_t *)allocator_calloc(env_get_allocator(env), 
            sizeof(ham_cursor_t));
    if (!c)
        return (HAM_OUT_OF_MEMORY);

    cursor_set_flags(c, flags);
    cursor_set_db(c, db);

    /* don't use cursor_get_txn_cursor() because it asserts that the struct
     * was setup correctly (but this was not yet the case here) */
    txn_cursor_create(db, txn, flags, &c->_txn_cursor, c);
    btree_cursor_create(db, txn, flags, cursor_get_btree_cursor(c), c);

    *pcursor=c;
    return (0);
}

ham_status_t
cursor_clone(ham_cursor_t *src, ham_cursor_t **dest)
{
    ham_status_t st;
    ham_db_t *db=cursor_get_db(src);
    ham_env_t *env=db_get_env(db);
    ham_cursor_t *c;

    *dest=0;

    c=(ham_cursor_t *)allocator_alloc(env_get_allocator(env), 
            sizeof(ham_cursor_t));
    if (!c)
        return (HAM_OUT_OF_MEMORY);
    memcpy(c, src, sizeof(ham_cursor_t));
    cursor_set_next_in_page(c, 0);
    cursor_set_previous_in_page(c, 0);

    st=btree_cursor_clone(cursor_get_btree_cursor(src), 
                    cursor_get_btree_cursor(c), c);
    if (st)
        return (st);

    /* always clone the txn-cursor, even if transactions are not required 
     *
     * don't use cursor_get_txn_cursor() because it asserts that the struct
     * was setup correctly (but this was not yet the case here) */
    txn_cursor_clone(cursor_get_txn_cursor(src), &c->_txn_cursor, c);

    if (db_get_rt_flags(db)&HAM_ENABLE_DUPLICATES) {
        dupecache_clone(cursor_get_dupecache(src), 
                        cursor_get_dupecache(c));
    }

    *dest=c;
    return (0);
}

ham_bool_t
cursor_is_nil(ham_cursor_t *cursor, int what)
{
    ham_assert(cursor!=0, (""));

    switch (what) {
      case CURSOR_BTREE:
        return (__btree_cursor_is_nil(cursor_get_btree_cursor(cursor)));
      case CURSOR_TXN:
        return (txn_cursor_is_nil(cursor_get_txn_cursor(cursor)));
      default:
        ham_assert(what==0, (""));
        /* TODO btree_cursor_is_nil is different from __btree_cursor_is_nil
         * - refactor and clean up! */
        return (btree_cursor_is_nil(cursor_get_btree_cursor(cursor)));
    }
}

void
cursor_set_to_nil(ham_cursor_t *cursor, int what)
{
    switch (what) {
      case CURSOR_BTREE:
        btree_cursor_set_to_nil(cursor_get_btree_cursor(cursor));
        break;
      case CURSOR_TXN:
        txn_cursor_set_to_nil(cursor_get_txn_cursor(cursor));
        break;
      default:
        ham_assert(what==0, (""));
        btree_cursor_set_to_nil(cursor_get_btree_cursor(cursor));
        txn_cursor_set_to_nil(cursor_get_txn_cursor(cursor));
        break;
    }
}

ham_status_t
cursor_erase(ham_cursor_t *cursor, ham_txn_t *txn, ham_u32_t flags)
{
    ham_status_t st;

    /* if transactions are enabled: add a erase-op to the txn-tree */
    if (txn) {
        /* if cursor is coupled to a btree item: set the txn-cursor to 
         * nil; otherwise txn_cursor_erase() doesn't know which cursor 
         * part is the valid one */
        if (cursor_is_coupled_to_btree(cursor))
            cursor_set_to_nil(cursor, CURSOR_TXN);
        st=txn_cursor_erase(cursor_get_txn_cursor(cursor));
    }
    else {
        st=btree_cursor_erase(cursor_get_btree_cursor(cursor), flags);
    }

    if (st==0)
        cursor_set_to_nil(cursor, 0);
    return (st);
}

ham_status_t
cursor_get_duplicate_count(ham_cursor_t *cursor, ham_txn_t *txn, 
            ham_u32_t *pcount, ham_u32_t flags)
{
    ham_status_t st=0;
    ham_db_t *db=cursor_get_db(cursor);

    *pcount=0;

    if (txn) {
        if (db_get_rt_flags(db)&HAM_ENABLE_DUPLICATES) {
            ham_bool_t dummy;
            dupecache_t *dc=cursor_get_dupecache(cursor);

            (void)cursor_sync(cursor, 0, &dummy);
            st=cursor_update_dupecache(cursor, CURSOR_TXN|CURSOR_BTREE);
            if (st)
                return (st);
            *pcount=dupecache_get_count(dc);
        }
        else {
            /* obviously the key exists, since the cursor is coupled to
             * a valid item */
            *pcount=1;
        }
    }
    else {
        st=btree_cursor_get_duplicate_count(cursor_get_btree_cursor(cursor), 
                    pcount, flags);
    }

    return (st);
}

ham_status_t 
cursor_overwrite(ham_cursor_t *cursor, ham_txn_t *txn, ham_record_t *record,
            ham_u32_t flags)
{
    ham_status_t st=0;
    ham_db_t *db=cursor_get_db(cursor);

    /*
     * if we're in transactional mode then just append an "insert/OW" operation
     * to the txn-tree. 
     *
     * if the txn_cursor is already coupled to a txn-op, then we can use
     * txn_cursor_overwrite(). Otherwise we have to call db_insert_txn().
     *
     * If transactions are disabled then overwrite the item in the btree.
     */
    if (txn) {
        if (txn_cursor_is_nil(cursor_get_txn_cursor(cursor))
                && !(cursor_is_nil(cursor, 0))) {
            st=btree_cursor_uncouple(cursor_get_btree_cursor(cursor), 0);
            if (st==0)
                st=db_insert_txn(db, txn, 
                    btree_cursor_get_uncoupled_key(
                            cursor_get_btree_cursor(cursor)),
                    record, flags|HAM_OVERWRITE, 
                    cursor_get_txn_cursor(cursor));
        }
        else {
            st=txn_cursor_overwrite(cursor_get_txn_cursor(cursor), record);
        }

        if (st==0)
            cursor_couple_to_txnop(cursor);
    }
    else {
        st=btree_cursor_overwrite(cursor_get_btree_cursor(cursor), 
                        record, flags);
        if (st==0)
            cursor_couple_to_btree(cursor);
    }

    return (st);
}


void
cursor_close(ham_cursor_t *cursor)
{
    btree_cursor_close(cursor_get_btree_cursor(cursor));
    txn_cursor_close(cursor_get_txn_cursor(cursor));
    dupecache_clear(cursor_get_dupecache(cursor));
}

#ifdef HAM_DEBUG
txn_cursor_t *
cursor_get_txn_cursor(ham_cursor_t *cursor)
{
    txn_cursor_t *txnc=&cursor->_txn_cursor;
    ham_assert(txn_cursor_get_parent(txnc)==cursor, (""));
    return (txnc);
}
#endif
