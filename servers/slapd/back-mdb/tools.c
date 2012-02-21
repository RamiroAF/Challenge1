/* tools.c - tools for slap tools */
/* $OpenLDAP$ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 2011-2012 The OpenLDAP Foundation.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted only as authorized by the OpenLDAP
 * Public License.
 *
 * A copy of this license is available in the file LICENSE in the
 * top-level directory of the distribution or, alternatively, at
 * <http://www.OpenLDAP.org/license.html>.
 */

#include "portable.h"

#include <stdio.h>
#include <ac/string.h>
#include <ac/errno.h>

#define AVL_INTERNAL
#include "back-mdb.h"
#include "idl.h"

#ifdef MDB_TOOL_IDL_CACHING
static int mdb_tool_idl_flush( BackendDB *be, MDB_txn *txn );

#define	IDBLOCK	1024

typedef struct mdb_tool_idl_cache_entry {
	struct mdb_tool_idl_cache_entry *next;
	ID ids[IDBLOCK];
} mdb_tool_idl_cache_entry;

typedef struct mdb_tool_idl_cache {
	struct berval kstr;
	mdb_tool_idl_cache_entry *head, *tail;
	ID first, last;
	int count;
	short offset;
	short flags;
} mdb_tool_idl_cache;
#define WAS_FOUND	0x01
#define WAS_RANGE	0x02

#define MDB_TOOL_IDL_FLUSH(be, txn)	mdb_tool_idl_flush(be, txn)
#else
#define MDB_TOOL_IDL_FLUSH(be, txn)
#endif /* MDB_TOOL_IDL_CACHING */

static MDB_txn *txn = NULL, *txi = NULL;
static MDB_cursor *cursor = NULL, *idcursor = NULL;
static MDB_cursor *mcp = NULL, *mcd = NULL;
static MDB_val key, data;
static ID previd = NOID;

typedef struct dn_id {
	ID id;
	struct berval dn;
} dn_id;

#define	HOLE_SIZE	4096
static dn_id hbuf[HOLE_SIZE], *holes = hbuf;
static unsigned nhmax = HOLE_SIZE;
static unsigned nholes;

static struct berval	*tool_base;
static int		tool_scope;
static Filter		*tool_filter;
static Entry		*tool_next_entry;

static ID mdb_tool_ix_id;
static Operation *mdb_tool_ix_op;
static MDB_txn *mdb_tool_ix_txn;
static int mdb_tool_index_tcount, mdb_tool_threads;
static IndexRec *mdb_tool_index_rec;
static struct mdb_info *mdb_tool_info;
static ldap_pvt_thread_mutex_t mdb_tool_index_mutex;
static ldap_pvt_thread_cond_t mdb_tool_index_cond_main;
static ldap_pvt_thread_cond_t mdb_tool_index_cond_work;
static void * mdb_tool_index_task( void *ctx, void *ptr );

static int	mdb_writes, mdb_writes_per_commit;

static int
mdb_tool_entry_get_int( BackendDB *be, ID id, Entry **ep );

int mdb_tool_entry_open(
	BackendDB *be, int mode )
{
	/* In Quick mode, commit once per 1000 entries */
	mdb_writes = 0;
	if ( slapMode & SLAP_TOOL_QUICK )
		mdb_writes_per_commit = 1000;
	else
		mdb_writes_per_commit = 1;

	/* Set up for threaded slapindex */
	if (( slapMode & (SLAP_TOOL_QUICK|SLAP_TOOL_READONLY)) == SLAP_TOOL_QUICK ) {
		if ( !mdb_tool_info ) {
			struct mdb_info *mdb = (struct mdb_info *) be->be_private;
			ldap_pvt_thread_mutex_init( &mdb_tool_index_mutex );
			ldap_pvt_thread_cond_init( &mdb_tool_index_cond_main );
			ldap_pvt_thread_cond_init( &mdb_tool_index_cond_work );
			if ( mdb->mi_nattrs ) {
				int i;
				mdb_tool_threads = slap_tool_thread_max - 1;
				if ( mdb_tool_threads > 1 ) {
					mdb_tool_index_rec = ch_calloc( mdb->mi_nattrs, sizeof( IndexRec ));
					mdb_tool_index_tcount = mdb_tool_threads - 1;
					for (i=1; i<mdb_tool_threads; i++) {
						int *ptr = ch_malloc( sizeof( int ));
						*ptr = i;
						ldap_pvt_thread_pool_submit( &connection_pool,
							mdb_tool_index_task, ptr );
					}
					mdb_tool_info = mdb;
				}
			}
		}
	}

	return 0;
}

int mdb_tool_entry_close(
	BackendDB *be )
{
	if ( mdb_tool_info ) {
		slapd_shutdown = 1;
		ldap_pvt_thread_mutex_lock( &mdb_tool_index_mutex );

		/* There might still be some threads starting */
		while ( mdb_tool_index_tcount > 0 ) {
			ldap_pvt_thread_cond_wait( &mdb_tool_index_cond_main,
					&mdb_tool_index_mutex );
		}

		mdb_tool_index_tcount = mdb_tool_threads - 1;
		ldap_pvt_thread_cond_broadcast( &mdb_tool_index_cond_work );

		/* Make sure all threads are stopped */
		while ( mdb_tool_index_tcount > 0 ) {
			ldap_pvt_thread_cond_wait( &mdb_tool_index_cond_main,
				&mdb_tool_index_mutex );
		}
		ldap_pvt_thread_mutex_unlock( &mdb_tool_index_mutex );

		mdb_tool_info = NULL;
		slapd_shutdown = 0;
		ch_free( mdb_tool_index_rec );
		mdb_tool_index_tcount = mdb_tool_threads - 1;
	}

	if( idcursor ) {
		mdb_cursor_close( idcursor );
		idcursor = NULL;
	}
	if( cursor ) {
		mdb_cursor_close( cursor );
		cursor = NULL;
	}
	if( txn ) {
		MDB_TOOL_IDL_FLUSH( be, txn );
		if ( mdb_txn_commit( txn ))
			return -1;
		txn = NULL;
	}

	if( nholes ) {
		unsigned i;
		fprintf( stderr, "Error, entries missing!\n");
		for (i=0; i<nholes; i++) {
			fprintf(stderr, "  entry %ld: %s\n",
				holes[i].id, holes[i].dn.bv_val);
		}
		nholes = 0;
		return -1;
	}

	return 0;
}

ID
mdb_tool_entry_first_x(
	BackendDB *be,
	struct berval *base,
	int scope,
	Filter *f )
{
	tool_base = base;
	tool_scope = scope;
	tool_filter = f;

	return mdb_tool_entry_next( be );
}

ID mdb_tool_entry_next(
	BackendDB *be )
{
	int rc;
	ID id;
	struct mdb_info *mdb;

	assert( be != NULL );
	assert( slapMode & SLAP_TOOL_MODE );

	mdb = (struct mdb_info *) be->be_private;
	assert( mdb != NULL );

	if ( !txn ) {
		rc = mdb_txn_begin( mdb->mi_dbenv, NULL, MDB_RDONLY, &txn );
		if ( rc )
			return NOID;
		rc = mdb_cursor_open( txn, mdb->mi_id2entry, &cursor );
		if ( rc ) {
			mdb_txn_abort( txn );
			return NOID;
		}
	}

next:;
	rc = mdb_cursor_get( cursor, &key, &data, MDB_NEXT );

	if( rc ) {
		return NOID;
	}

	previd = *(ID *)key.mv_data;
	id = previd;

	if ( tool_filter || tool_base ) {
		static Operation op = {0};
		static Opheader ohdr = {0};

		op.o_hdr = &ohdr;
		op.o_bd = be;
		op.o_tmpmemctx = NULL;
		op.o_tmpmfuncs = &ch_mfuncs;

		if ( tool_next_entry ) {
			mdb_entry_release( &op, tool_next_entry, 0 );
			tool_next_entry = NULL;
		}

		rc = mdb_tool_entry_get_int( be, id, &tool_next_entry );
		if ( rc == LDAP_NO_SUCH_OBJECT ) {
			goto next;
		}

		assert( tool_next_entry != NULL );

		if ( tool_filter && test_filter( NULL, tool_next_entry, tool_filter ) != LDAP_COMPARE_TRUE )
		{
			mdb_entry_release( &op, tool_next_entry, 0 );
			tool_next_entry = NULL;
			goto next;
		}
	}

	return id;
}

ID mdb_tool_dn2id_get(
	Backend *be,
	struct berval *dn
)
{
	struct mdb_info *mdb;
	Operation op = {0};
	Opheader ohdr = {0};
	ID id;
	int rc;

	if ( BER_BVISEMPTY(dn) )
		return 0;

	mdb = (struct mdb_info *) be->be_private;

	if ( !txn ) {
		rc = mdb_txn_begin( mdb->mi_dbenv, NULL, (slapMode & SLAP_TOOL_READONLY) != 0 ?
			MDB_RDONLY : 0, &txn );
		if ( rc )
			return NOID;
	}

	op.o_hdr = &ohdr;
	op.o_bd = be;
	op.o_tmpmemctx = NULL;
	op.o_tmpmfuncs = &ch_mfuncs;

	rc = mdb_dn2id( &op, txn, NULL, dn, &id, NULL, NULL );
	if ( rc == MDB_NOTFOUND )
		return NOID;

	return id;
}

static int
mdb_tool_entry_get_int( BackendDB *be, ID id, Entry **ep )
{
	Operation op = {0};
	Opheader ohdr = {0};

	Entry *e = NULL;
	struct berval dn = BER_BVNULL, ndn = BER_BVNULL;
	int rc;

	assert( be != NULL );
	assert( slapMode & SLAP_TOOL_MODE );

	if ( ( tool_filter || tool_base ) && id == previd && tool_next_entry != NULL ) {
		*ep = tool_next_entry;
		tool_next_entry = NULL;
		return LDAP_SUCCESS;
	}

	if ( id != previd ) {
		key.mv_size = sizeof(ID);
		key.mv_data = &id;
		rc = mdb_cursor_get( cursor, &key, &data, MDB_SET );
		if ( rc ) {
			rc = LDAP_OTHER;
			goto done;
		}
	}

	op.o_hdr = &ohdr;
	op.o_bd = be;
	op.o_tmpmemctx = NULL;
	op.o_tmpmfuncs = &ch_mfuncs;
	if ( slapMode & SLAP_TOOL_READONLY ) {
		rc = mdb_id2name( &op, txn, &idcursor, id, &dn, &ndn );
		if ( rc  ) {
			rc = LDAP_OTHER;
			mdb_entry_return( &op, e );
			e = NULL;
			goto done;
		}
		if ( tool_base != NULL ) {
			if ( !dnIsSuffixScope( &ndn, tool_base, tool_scope ) ) {
				ch_free( dn.bv_val );
				ch_free( ndn.bv_val );
				rc = LDAP_NO_SUCH_OBJECT;
			}
		}
	}
	rc = mdb_entry_decode( &op, &data, &e );
	e->e_id = id;
	if ( !BER_BVISNULL( &dn )) {
		e->e_name = dn;
		e->e_nname = ndn;
	} else {
		e->e_name.bv_val = NULL;
		e->e_nname.bv_val = NULL;
	}

done:
	if ( e != NULL ) {
		*ep = e;
	}

	return rc;
}

Entry*
mdb_tool_entry_get( BackendDB *be, ID id )
{
	Entry *e = NULL;

	(void)mdb_tool_entry_get_int( be, id, &e );
	return e;
}

static int mdb_tool_next_id(
	Operation *op,
	MDB_txn *tid,
	Entry *e,
	struct berval *text,
	int hole )
{
	struct mdb_info *mdb = (struct mdb_info *) op->o_bd->be_private;
	struct berval dn = e->e_name;
	struct berval ndn = e->e_nname;
	struct berval pdn, npdn, nmatched;
	ID id, pid = 0;
	int rc;

	if (ndn.bv_len == 0) {
		e->e_id = 0;
		return 0;
	}

	rc = mdb_dn2id( op, tid, mcp, &ndn, &id, NULL, &nmatched );
	if ( rc == MDB_NOTFOUND ) {
		if ( !be_issuffix( op->o_bd, &ndn ) ) {
			ID eid = e->e_id;
			dnParent( &ndn, &npdn );
			if ( nmatched.bv_len != npdn.bv_len ) {
				dnParent( &dn, &pdn );
				e->e_name = pdn;
				e->e_nname = npdn;
				rc = mdb_tool_next_id( op, tid, e, text, 1 );
				e->e_name = dn;
				e->e_nname = ndn;
				if ( rc ) {
					return rc;
				}
				/* If parent didn't exist, it was created just now
				 * and its ID is now in e->e_id. Make sure the current
				 * entry gets added under the new parent ID.
				 */
				if ( eid != e->e_id ) {
					pid = e->e_id;
				}
			} else {
				pid = id;
			}
		}
		rc = mdb_next_id( op->o_bd, idcursor, &e->e_id );
		if ( rc ) {
			snprintf( text->bv_val, text->bv_len,
				"next_id failed: %s (%d)",
				mdb_strerror(rc), rc );
		Debug( LDAP_DEBUG_ANY,
			"=> mdb_tool_next_id: %s\n", text->bv_val, 0, 0 );
			return rc;
		}
		rc = mdb_dn2id_add( op, mcp, mcd, pid, e );
		if ( rc ) {
			snprintf( text->bv_val, text->bv_len,
				"dn2id_add failed: %s (%d)",
				mdb_strerror(rc), rc );
			Debug( LDAP_DEBUG_ANY,
				"=> mdb_tool_next_id: %s\n", text->bv_val, 0, 0 );
		} else if ( hole ) {
			MDB_val key, data;
			if ( nholes == nhmax - 1 ) {
				if ( holes == hbuf ) {
					holes = ch_malloc( nhmax * sizeof(dn_id) * 2 );
					AC_MEMCPY( holes, hbuf, sizeof(hbuf) );
				} else {
					holes = ch_realloc( holes, nhmax * sizeof(dn_id) * 2 );
				}
				nhmax *= 2;
			}
			ber_dupbv( &holes[nholes].dn, &ndn );
			holes[nholes++].id = e->e_id;
			key.mv_size = sizeof(ID);
			key.mv_data = &e->e_id;
			data.mv_size = 0;
			data.mv_data = NULL;
			rc = mdb_cursor_put( idcursor, &key, &data, MDB_NOOVERWRITE );
			if ( rc == MDB_KEYEXIST )
				rc = 0;
			if ( rc ) {
				snprintf( text->bv_val, text->bv_len,
					"dummy id2entry add failed: %s (%d)",
					mdb_strerror(rc), rc );
				Debug( LDAP_DEBUG_ANY,
					"=> mdb_tool_next_id: %s\n", text->bv_val, 0, 0 );
			}
		}
	} else if ( !hole ) {
		unsigned i, j;

		e->e_id = id;

		for ( i=0; i<nholes; i++) {
			if ( holes[i].id == e->e_id ) {
				free(holes[i].dn.bv_val);
				for (j=i;j<nholes;j++) holes[j] = holes[j+1];
				holes[j].id = 0;
				nholes--;
				break;
			} else if ( holes[i].id > e->e_id ) {
				break;
			}
		}
	}
	return rc;
}

static int
mdb_tool_index_add(
	Operation *op,
	MDB_txn *txn,
	Entry *e )
{
	struct mdb_info *mdb = (struct mdb_info *) op->o_bd->be_private;

	if ( !mdb->mi_nattrs )
		return 0;

	if ( mdb_tool_threads > 1 ) {
		IndexRec *ir;
		int i, rc;
		Attribute *a;

		ir = mdb_tool_index_rec;
		for (i=0; i<mdb->mi_nattrs; i++)
			ir[i].ir_attrs = NULL;

		for ( a = e->e_attrs; a != NULL; a = a->a_next ) {
			rc = mdb_index_recset( mdb, a, a->a_desc->ad_type,
				&a->a_desc->ad_tags, ir );
			if ( rc )
				return rc;
		}
		for (i=0; i<mdb->mi_nattrs; i++) {
			if ( !ir[i].ir_ai )
				break;
			rc = mdb_cursor_open( txn, ir[i].ir_ai->ai_dbi,
				 &ir[i].ir_ai->ai_cursor );
			if ( rc )
				return rc;
		}
		mdb_tool_ix_id = e->e_id;
		mdb_tool_ix_op = op;
		mdb_tool_ix_txn = txn;
		ldap_pvt_thread_mutex_lock( &mdb_tool_index_mutex );
		/* Wait for all threads to be ready */
		while ( mdb_tool_index_tcount ) {
			ldap_pvt_thread_cond_wait( &mdb_tool_index_cond_main,
				&mdb_tool_index_mutex );
		}

		for ( i=1; i<mdb_tool_threads; i++ )
			mdb_tool_index_rec[i].ir_i = LDAP_BUSY;
		mdb_tool_index_tcount = mdb_tool_threads - 1;
		ldap_pvt_thread_mutex_unlock( &mdb_tool_index_mutex );
		ldap_pvt_thread_cond_broadcast( &mdb_tool_index_cond_work );

		rc = mdb_index_recrun( op, txn, mdb, ir, e->e_id, 0 );
		if ( rc )
			return rc;
		ldap_pvt_thread_mutex_lock( &mdb_tool_index_mutex );
		for ( i=1; i<mdb_tool_threads; i++ ) {
			if ( mdb_tool_index_rec[i].ir_i == LDAP_BUSY ) {
				ldap_pvt_thread_cond_wait( &mdb_tool_index_cond_main,
					&mdb_tool_index_mutex );
				i--;
				continue;
			}
			if ( mdb_tool_index_rec[i].ir_i ) {
				rc = mdb_tool_index_rec[i].ir_i;
				break;
			}
		}
		ldap_pvt_thread_mutex_unlock( &mdb_tool_index_mutex );
		return rc;
	} else
	{
		return mdb_index_entry_add( op, txn, e );
	}
}

ID mdb_tool_entry_put(
	BackendDB *be,
	Entry *e,
	struct berval *text )
{
	int rc;
	struct mdb_info *mdb;
	Operation op = {0};
	Opheader ohdr = {0};

	assert( be != NULL );
	assert( slapMode & SLAP_TOOL_MODE );

	assert( text != NULL );
	assert( text->bv_val != NULL );
	assert( text->bv_val[0] == '\0' );	/* overconservative? */

	Debug( LDAP_DEBUG_TRACE, "=> " LDAP_XSTRING(mdb_tool_entry_put)
		"( %ld, \"%s\" )\n", (long) e->e_id, e->e_dn, 0 );

	mdb = (struct mdb_info *) be->be_private;

	if ( !txn ) {
		rc = mdb_txn_begin( mdb->mi_dbenv, NULL, 0, &txn );
		if( rc != 0 ) {
			snprintf( text->bv_val, text->bv_len,
				"txn_begin failed: %s (%d)",
				mdb_strerror(rc), rc );
			Debug( LDAP_DEBUG_ANY,
				"=> " LDAP_XSTRING(mdb_tool_entry_put) ": %s\n",
				 text->bv_val, 0, 0 );
			return NOID;
		}
		rc = mdb_cursor_open( txn, mdb->mi_id2entry, &idcursor );
		if( rc != 0 ) {
			snprintf( text->bv_val, text->bv_len,
				"cursor_open failed: %s (%d)",
				mdb_strerror(rc), rc );
			Debug( LDAP_DEBUG_ANY,
				"=> " LDAP_XSTRING(mdb_tool_entry_put) ": %s\n",
				 text->bv_val, 0, 0 );
			return NOID;
		}
		rc = mdb_cursor_open( txn, mdb->mi_dn2id, &mcp );
		if( rc != 0 ) {
			snprintf( text->bv_val, text->bv_len,
				"cursor_open failed: %s (%d)",
				mdb_strerror(rc), rc );
			Debug( LDAP_DEBUG_ANY,
				"=> " LDAP_XSTRING(mdb_tool_entry_put) ": %s\n",
				 text->bv_val, 0, 0 );
			return NOID;
		}
		rc = mdb_cursor_open( txn, mdb->mi_dn2id, &mcd );
		if( rc != 0 ) {
			snprintf( text->bv_val, text->bv_len,
				"cursor_open failed: %s (%d)",
				mdb_strerror(rc), rc );
			Debug( LDAP_DEBUG_ANY,
				"=> " LDAP_XSTRING(mdb_tool_entry_put) ": %s\n",
				 text->bv_val, 0, 0 );
			return NOID;
		}
	}

	op.o_hdr = &ohdr;
	op.o_bd = be;
	op.o_tmpmemctx = NULL;
	op.o_tmpmfuncs = &ch_mfuncs;

	/* add dn2id indices */
	rc = mdb_tool_next_id( &op, txn, e, text, 0 );
	if( rc != 0 ) {
		goto done;
	}

	rc = mdb_tool_index_add( &op, txn, e );
	if( rc != 0 ) {
		snprintf( text->bv_val, text->bv_len,
				"index_entry_add failed: err=%d", rc );
		Debug( LDAP_DEBUG_ANY,
			"=> " LDAP_XSTRING(mdb_tool_entry_put) ": %s\n",
			text->bv_val, 0, 0 );
		goto done;
	}


	/* id2entry index */
	rc = mdb_id2entry_add( &op, txn, idcursor, e );
	if( rc != 0 ) {
		snprintf( text->bv_val, text->bv_len,
				"id2entry_add failed: err=%d", rc );
		Debug( LDAP_DEBUG_ANY,
			"=> " LDAP_XSTRING(mdb_tool_entry_put) ": %s\n",
			text->bv_val, 0, 0 );
		goto done;
	}

done:
	if( rc == 0 ) {
		mdb_writes++;
		if ( mdb_writes >= mdb_writes_per_commit ) {
			unsigned i;
			MDB_TOOL_IDL_FLUSH( be, txn );
			rc = mdb_txn_commit( txn );
			for ( i=0; i<mdb->mi_nattrs; i++ )
				mdb->mi_attrs[i]->ai_cursor = NULL;
			mdb_writes = 0;
			txn = NULL;
			idcursor = NULL;
			if( rc != 0 ) {
				snprintf( text->bv_val, text->bv_len,
						"txn_commit failed: %s (%d)",
						mdb_strerror(rc), rc );
				Debug( LDAP_DEBUG_ANY,
					"=> " LDAP_XSTRING(mdb_tool_entry_put) ": %s\n",
					text->bv_val, 0, 0 );
				e->e_id = NOID;
			}
		}

	} else {
		unsigned i;
		mdb_txn_abort( txn );
		txn = NULL;
		idcursor = NULL;
		for ( i=0; i<mdb->mi_nattrs; i++ )
			mdb->mi_attrs[i]->ai_cursor = NULL;
		mdb_writes = 0;
		snprintf( text->bv_val, text->bv_len,
			"txn_aborted! %s (%d)",
			rc == LDAP_OTHER ? "Internal error" :
			mdb_strerror(rc), rc );
		Debug( LDAP_DEBUG_ANY,
			"=> " LDAP_XSTRING(mdb_tool_entry_put) ": %s\n",
			text->bv_val, 0, 0 );
		e->e_id = NOID;
	}

	return e->e_id;
}

int mdb_tool_entry_reindex(
	BackendDB *be,
	ID id,
	AttributeDescription **adv )
{
	struct mdb_info *mi = (struct mdb_info *) be->be_private;
	int rc;
	Entry *e;
	Operation op = {0};
	Opheader ohdr = {0};

	Debug( LDAP_DEBUG_ARGS,
		"=> " LDAP_XSTRING(mdb_tool_entry_reindex) "( %ld )\n",
		(long) id, 0, 0 );
	assert( tool_base == NULL );
	assert( tool_filter == NULL );

	/* No indexes configured, nothing to do. Could return an
	 * error here to shortcut things.
	 */
	if (!mi->mi_attrs) {
		return 0;
	}

	/* Check for explicit list of attrs to index */
	if ( adv ) {
		int i, j, n;

		if ( mi->mi_attrs[0]->ai_desc != adv[0] ) {
			/* count */
			for ( n = 0; adv[n]; n++ ) ;

			/* insertion sort */
			for ( i = 0; i < n; i++ ) {
				AttributeDescription *ad = adv[i];
				for ( j = i-1; j>=0; j--) {
					if ( SLAP_PTRCMP( adv[j], ad ) <= 0 ) break;
					adv[j+1] = adv[j];
				}
				adv[j+1] = ad;
			}
		}

		for ( i = 0; adv[i]; i++ ) {
			if ( mi->mi_attrs[i]->ai_desc != adv[i] ) {
				for ( j = i+1; j < mi->mi_nattrs; j++ ) {
					if ( mi->mi_attrs[j]->ai_desc == adv[i] ) {
						AttrInfo *ai = mi->mi_attrs[i];
						mi->mi_attrs[i] = mi->mi_attrs[j];
						mi->mi_attrs[j] = ai;
						break;
					}
				}
				if ( j == mi->mi_nattrs ) {
					Debug( LDAP_DEBUG_ANY,
						LDAP_XSTRING(mdb_tool_entry_reindex)
						": no index configured for %s\n",
						adv[i]->ad_cname.bv_val, 0, 0 );
					return -1;
				}
			}
		}
		mi->mi_nattrs = i;
	}

	e = mdb_tool_entry_get( be, id );

	if( e == NULL ) {
		Debug( LDAP_DEBUG_ANY,
			LDAP_XSTRING(mdb_tool_entry_reindex)
			": could not locate id=%ld\n",
			(long) id, 0, 0 );
		return -1;
	}

	if ( !txi ) {
		rc = mdb_txn_begin( mi->mi_dbenv, NULL, 0, &txi );
		if( rc != 0 ) {
			Debug( LDAP_DEBUG_ANY,
				"=> " LDAP_XSTRING(mdb_tool_entry_reindex) ": "
				"txn_begin failed: %s (%d)\n",
				mdb_strerror(rc), rc, 0 );
			goto done;
		}
	}

	if ( slapMode & SLAP_TRUNCATE_MODE ) {
		int i;
		for ( i=0; i < mi->mi_nattrs; i++ ) {
			rc = mdb_drop( txi, mi->mi_attrs[i]->ai_dbi, 0 );
			if ( rc ) {
				Debug( LDAP_DEBUG_ANY,
					LDAP_XSTRING(mdb_tool_entry_reindex)
					": (Truncate) mdb_drop(%s) failed: %s (%d)\n",
					mi->mi_attrs[i]->ai_desc->ad_type->sat_cname.bv_val,
					mdb_strerror(rc), rc );
				return -1;
			}
		}
		slapMode ^= SLAP_TRUNCATE_MODE;
	}

	/*
	 * just (re)add them for now
	 * Use truncate mode to empty/reset index databases
	 */

	Debug( LDAP_DEBUG_TRACE,
		"=> " LDAP_XSTRING(mdb_tool_entry_reindex) "( %ld )\n",
		(long) id, 0, 0 );

	op.o_hdr = &ohdr;
	op.o_bd = be;
	op.o_tmpmemctx = NULL;
	op.o_tmpmfuncs = &ch_mfuncs;

	rc = mdb_tool_index_add( &op, txi, e );

done:
	if( rc == 0 ) {
		mdb_writes++;
		if ( mdb_writes >= mdb_writes_per_commit ) {
			unsigned i;
			MDB_TOOL_IDL_FLUSH( be, txi );
			rc = mdb_txn_commit( txi );
			mdb_writes = 0;
			for ( i=0; i<mi->mi_nattrs; i++ )
				mi->mi_attrs[i]->ai_cursor = NULL;
			if( rc != 0 ) {
				Debug( LDAP_DEBUG_ANY,
					"=> " LDAP_XSTRING(mdb_tool_entry_reindex)
					": txn_commit failed: %s (%d)\n",
					mdb_strerror(rc), rc, 0 );
				e->e_id = NOID;
			}
			txi = NULL;
		}

	} else {
		unsigned i;
		mdb_writes = 0;
		mdb_txn_abort( txi );
		for ( i=0; i<mi->mi_nattrs; i++ )
			mi->mi_attrs[i]->ai_cursor = NULL;
		Debug( LDAP_DEBUG_ANY,
			"=> " LDAP_XSTRING(mdb_tool_entry_reindex)
			": txn_aborted! err=%d\n",
			rc, 0, 0 );
		e->e_id = NOID;
		txi = NULL;
	}
	mdb_entry_release( &op, e, 0 );

	return rc;
}

ID mdb_tool_entry_modify(
	BackendDB *be,
	Entry *e,
	struct berval *text )
{
	int rc;
	struct mdb_info *mdb;
	MDB_txn *tid;
	Operation op = {0};
	Opheader ohdr = {0};

	assert( be != NULL );
	assert( slapMode & SLAP_TOOL_MODE );

	assert( text != NULL );
	assert( text->bv_val != NULL );
	assert( text->bv_val[0] == '\0' );	/* overconservative? */

	assert ( e->e_id != NOID );

	Debug( LDAP_DEBUG_TRACE,
		"=> " LDAP_XSTRING(mdb_tool_entry_modify) "( %ld, \"%s\" )\n",
		(long) e->e_id, e->e_dn, 0 );

	mdb = (struct mdb_info *) be->be_private;

	if( cursor ) {
		mdb_cursor_close( cursor );
		cursor = NULL;
	}
	rc = mdb_txn_begin( mdb->mi_dbenv, NULL, 0, &tid );
	if( rc != 0 ) {
		snprintf( text->bv_val, text->bv_len,
			"txn_begin failed: %s (%d)",
			mdb_strerror(rc), rc );
		Debug( LDAP_DEBUG_ANY,
			"=> " LDAP_XSTRING(mdb_tool_entry_modify) ": %s\n",
			 text->bv_val, 0, 0 );
		return NOID;
	}

	op.o_hdr = &ohdr;
	op.o_bd = be;
	op.o_tmpmemctx = NULL;
	op.o_tmpmfuncs = &ch_mfuncs;

	/* id2entry index */
	rc = mdb_id2entry_update( &op, tid, NULL, e );
	if( rc != 0 ) {
		snprintf( text->bv_val, text->bv_len,
				"id2entry_update failed: err=%d", rc );
		Debug( LDAP_DEBUG_ANY,
			"=> " LDAP_XSTRING(mdb_tool_entry_modify) ": %s\n",
			text->bv_val, 0, 0 );
		goto done;
	}

done:
	if( rc == 0 ) {
		rc = mdb_txn_commit( tid );
		if( rc != 0 ) {
			snprintf( text->bv_val, text->bv_len,
					"txn_commit failed: %s (%d)",
					mdb_strerror(rc), rc );
			Debug( LDAP_DEBUG_ANY,
				"=> " LDAP_XSTRING(mdb_tool_entry_modify) ": "
				"%s\n", text->bv_val, 0, 0 );
			e->e_id = NOID;
		}

	} else {
		mdb_txn_abort( tid );
		snprintf( text->bv_val, text->bv_len,
			"txn_aborted! %s (%d)",
			mdb_strerror(rc), rc );
		Debug( LDAP_DEBUG_ANY,
			"=> " LDAP_XSTRING(mdb_tool_entry_modify) ": %s\n",
			text->bv_val, 0, 0 );
		e->e_id = NOID;
	}

	return e->e_id;
}

static void *
mdb_tool_index_task( void *ctx, void *ptr )
{
	int base = *(int *)ptr;

	free( ptr );
	while ( 1 ) {
		ldap_pvt_thread_mutex_lock( &mdb_tool_index_mutex );
		mdb_tool_index_tcount--;
		if ( !mdb_tool_index_tcount )
			ldap_pvt_thread_cond_signal( &mdb_tool_index_cond_main );
		ldap_pvt_thread_cond_wait( &mdb_tool_index_cond_work,
			&mdb_tool_index_mutex );
		if ( slapd_shutdown ) {
			mdb_tool_index_tcount--;
			if ( !mdb_tool_index_tcount )
				ldap_pvt_thread_cond_signal( &mdb_tool_index_cond_main );
			ldap_pvt_thread_mutex_unlock( &mdb_tool_index_mutex );
			break;
		}
		ldap_pvt_thread_mutex_unlock( &mdb_tool_index_mutex );
		mdb_tool_index_rec[base].ir_i = mdb_index_recrun( mdb_tool_ix_op,
			mdb_tool_ix_txn,
			mdb_tool_info, mdb_tool_index_rec, mdb_tool_ix_id, base );
	}

	return NULL;
}

#ifdef MDB_TOOL_IDL_CACHING
static int
mdb_tool_idl_cmp( const void *v1, const void *v2 )
{
	const mdb_tool_idl_cache *c1 = v1, *c2 = v2;
	int rc;

	if (( rc = c1->kstr.bv_len - c2->kstr.bv_len )) return rc;
	return memcmp( c1->kstr.bv_val, c2->kstr.bv_val, c1->kstr.bv_len );
}

static int
mdb_tool_idl_flush_one( MDB_cursor *mc, AttrInfo *ai, mdb_tool_idl_cache *ic )
{
	mdb_tool_idl_cache_entry *ice;
	MDB_val key, data[2];
	int i, rc;
	ID id, nid;

	/* Freshly allocated, ignore it */
	if ( !ic->head && ic->count <= MDB_IDL_DB_SIZE ) {
		return 0;
	}

	key.mv_data = ic->kstr.bv_val;
	key.mv_size = ic->kstr.bv_len;

	if ( ic->count > MDB_IDL_DB_SIZE ) {
		while ( ic->flags & WAS_FOUND ) {
			rc = mdb_cursor_get( mc, &key, data, MDB_SET );
			if ( rc ) {
				/* FIXME: find out why this happens */
				ic->flags = 0;
				break;
			}
			if ( ic->flags & WAS_RANGE ) {
				/* Skip lo */
				rc = mdb_cursor_get( mc, &key, data, MDB_NEXT_DUP );

				/* Get hi */
				rc = mdb_cursor_get( mc, &key, data, MDB_NEXT_DUP );

				/* Store range hi */
				data[0].mv_data = &ic->last;
				rc = mdb_cursor_put( mc, &key, data, MDB_CURRENT );
			} else {
				/* Delete old data, replace with range */
				ic->first = *(ID *)data[0].mv_data;
				mdb_cursor_del( mc, MDB_NODUPDATA );
			}
			break;
		}
		if ( !(ic->flags & WAS_RANGE)) {
			/* range, didn't exist before */
			nid = 0;
			data[0].mv_size = sizeof(ID);
			data[0].mv_data = &nid;
			rc = mdb_cursor_put( mc, &key, data, 0 );
			if ( rc == 0 ) {
				data[0].mv_data = &ic->first;
				rc = mdb_cursor_put( mc, &key, data, 0 );
				if ( rc == 0 ) {
					data[0].mv_data = &ic->last;
					rc = mdb_cursor_put( mc, &key, data, 0 );
				}
			}
			if ( rc ) {
				rc = -1;
			}
		}
	} else {
		/* Normal write */
		int n;

		data[0].mv_size = sizeof(ID);
		rc = 0;
		i = ic->offset;
		for ( ice = ic->head, n=0; ice; ice = ice->next, n++ ) {
			int end;
			if ( ice->next ) {
				end = IDBLOCK;
			} else {
				end = ic->count & (IDBLOCK-1);
				if ( !end )
					end = IDBLOCK;
			}
			data[1].mv_size = end - i;
			data[0].mv_data = &ice->ids[i];
			i = 0;
			rc = mdb_cursor_put( mc, &key, data, MDB_NODUPDATA|MDB_APPEND|MDB_MULTIPLE );
			if ( rc ) {
				if ( rc == MDB_KEYEXIST ) {
					rc = 0;
					continue;
				}
				rc = -1;
				break;
			}
		}
		if ( ic->head ) {
			ic->tail->next = ai->ai_flist;
			ai->ai_flist = ic->head;
		}
	}
	ic->head = ai->ai_clist;
	ai->ai_clist = ic;
	return rc;
}

static int
mdb_tool_idl_flush_db( MDB_txn *txn, AttrInfo *ai )
{
	MDB_cursor *mc;
	Avlnode *root;
	int rc;

	mdb_cursor_open( txn, ai->ai_dbi, &mc );
	root = tavl_end( ai->ai_root, TAVL_DIR_LEFT );
	do {
		rc = mdb_tool_idl_flush_one( mc, ai, root->avl_data );
		if ( rc != -1 )
			rc = 0;
	} while ((root = tavl_next(root, TAVL_DIR_RIGHT)));
	mdb_cursor_close( mc );

	return rc;
}

static int
mdb_tool_idl_flush( BackendDB *be, MDB_txn *txn )
{
	struct mdb_info *mdb = (struct mdb_info *) be->be_private;
	int rc = 0;
	unsigned int i, dbi;

	for ( i=0; i < mdb->mi_nattrs; i++ ) {
		if ( !mdb->mi_attrs[i]->ai_root ) continue;
		rc = mdb_tool_idl_flush_db( txn, mdb->mi_attrs[i] );
		tavl_free(mdb->mi_attrs[i]->ai_root, NULL);
		mdb->mi_attrs[i]->ai_root = NULL;
		if ( rc )
			break;
	}
	return rc;
}

int mdb_tool_idl_add(
	MDB_cursor *mc,
	struct berval *keys,
	ID id )
{
	MDB_dbi dbi;
	mdb_tool_idl_cache *ic, itmp;
	mdb_tool_idl_cache_entry *ice;
	int i, rc, lcount;
	AttrInfo *ai = (AttrInfo *)mc;
	mc = ai->ai_cursor;

	dbi = ai->ai_dbi;
	for (i=0; keys[i].bv_val; i++) {
	itmp.kstr = keys[i];
	ic = tavl_find( (Avlnode *)ai->ai_root, &itmp, mdb_tool_idl_cmp );

	/* No entry yet, create one */
	if ( !ic ) {
		MDB_val key, data;
		ID nid;
		int rc;

		if ( ai->ai_clist ) {
			ic = ai->ai_clist;
			ai->ai_clist = ic->head;
		} else {
			ic = ch_malloc( sizeof( mdb_tool_idl_cache ) + itmp.kstr.bv_len + 4 );
		}
		ic->kstr.bv_len = itmp.kstr.bv_len;
		ic->kstr.bv_val = (char *)(ic+1);
		memcpy( ic->kstr.bv_val, itmp.kstr.bv_val, ic->kstr.bv_len );
		ic->head = ic->tail = NULL;
		ic->last = 0;
		ic->count = 0;
		ic->offset = 0;
		ic->flags = 0;
		tavl_insert( (Avlnode **)&ai->ai_root, ic, mdb_tool_idl_cmp,
			avl_dup_error );

		/* load existing key count here */
		key.mv_size = keys[i].bv_len;
		key.mv_data = keys[i].bv_val;
		rc = mdb_cursor_get( mc, &key, &data, MDB_SET );
		if ( rc == 0 ) {
			ic->flags |= WAS_FOUND;
			nid = *(ID *)data.mv_data;
			if ( nid == 0 ) {
				ic->count = MDB_IDL_DB_SIZE+1;
				ic->flags |= WAS_RANGE;
			} else {
				size_t count;

				mdb_cursor_count( mc, &count );
				ic->count = count;
				ic->first = nid;
				ic->offset = count & (IDBLOCK-1);
			}
		}
	}
	/* are we a range already? */
	if ( ic->count > MDB_IDL_DB_SIZE ) {
		ic->last = id;
		continue;
	/* Are we at the limit, and converting to a range? */
	} else if ( ic->count == MDB_IDL_DB_SIZE ) {
		if ( ic->head ) {
			ic->tail->next = ai->ai_flist;
			ai->ai_flist = ic->head;
		}
		ic->head = ic->tail = NULL;
		ic->last = id;
		ic->count++;
		continue;
	}
	/* No free block, create that too */
	lcount = ic->count & (IDBLOCK-1);
	if ( !ic->tail || lcount == 0) {
		if ( ai->ai_flist ) {
			ice = ai->ai_flist;
			ai->ai_flist = ice->next;
		} else {
			ice = ch_malloc( sizeof( mdb_tool_idl_cache_entry ));
		}
		ice->next = NULL;
		if ( !ic->head ) {
			ic->head = ice;
		} else {
			ic->tail->next = ice;
		}
		ic->tail = ice;
		if ( lcount )
			ice->ids[lcount-1] = 0;
		if ( !ic->count )
			ic->first = id;
	}
	ice = ic->tail;
	if (!lcount || ice->ids[lcount-1] != id)
		ice->ids[lcount] = id;
	ic->count++;
	}

	return 0;
}
#endif /* MDB_TOOL_IDL_CACHING */
