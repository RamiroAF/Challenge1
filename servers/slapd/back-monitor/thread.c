/* thread.c - deal with thread subsystem */
/*
 * Copyright 1998-2000 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */
/*
 * Copyright 2001 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 * 
 * Copyright 2001, Pierangelo Masarati, All rights reserved. <ando@sys-net.it>
 * 
 * This work has beed deveolped for the OpenLDAP Foundation 
 * in the hope that it may be useful to the Open Source community, 
 * but WITHOUT ANY WARRANTY.
 * 
 * Permission is granted to anyone to use this software for any purpose
 * on any computer system, and to alter it and redistribute it, subject
 * to the following restrictions:
 * 
 * 1. The author and SysNet s.n.c. are not responsible for the consequences
 *    of use of this software, no matter how awful, even if they arise from
 *    flaws in it.
 * 
 * 2. The origin of this software must not be misrepresented, either by
 *    explicit claim or by omission.  Since few users ever read sources,
 *    credits should appear in the documentation.
 * 
 * 3. Altered versions must be plainly marked as such, and must not be
 *    misrepresented as being the original software.  Since few users
 *    ever read sources, credits should appear in the documentation.
 *    SysNet s.n.c. cannot be responsible for the consequences of the
 *    alterations.
 * 
 * 4. This notice may not be removed or altered.
 */

#include "portable.h"

#include <stdio.h>

#include "slap.h"
#include "back-monitor.h"

/*
*  * initializes log subentry
*   */
int
monitor_subsys_thread_init(
	BackendDB       *be
)
{
	struct monitorinfo      *mi;
	Entry                   *e;
	struct monitorentrypriv *mp;
	struct berval           val, *bv[2] = { &val, NULL };
	static char		buf[1024];

	mi = ( struct monitorinfo * )be->be_private;

	if ( monitor_cache_get( mi, 
				monitor_subsys[SLAPD_MONITOR_THREAD].mss_ndn,
				&e ) ) {
#ifdef NEW_LOGGING
		LDAP_LOG(( "operation", LDAP_LEVEL_CRIT,
					"monitor_subsys_thread_init: "
					"unable to get entry '%s'\n",
					monitor_subsys[SLAPD_MONITOR_THREAD].mss_ndn ));
#else
		Debug( LDAP_DEBUG_ANY,
				"monitor_subsys_thread_init: "
				"unable to get entry '%s'\n%s%s",
				monitor_subsys[SLAPD_MONITOR_THREAD].mss_ndn,
				"", "" );
#endif
		return( -1 );
	}

	/* initialize the thread number */
	snprintf( buf, sizeof( buf ), "max=%d", connection_pool_max );

	val.bv_val = buf;
	val.bv_len = strlen( val.bv_val );

	attr_merge( e, monitor_ad_desc, bv );

	monitor_cache_release( mi, e );

	return( 0 );
}

int 
monitor_subsys_thread_update( 
	struct monitorinfo 	*mi,
	Entry 			*e
)
{
	Attribute		*a;
	struct berval           *bv[2], val, **b = NULL;
	char 			buf[1024];

	bv[0] = &val;
	bv[1] = NULL;

	snprintf( buf, sizeof( buf ), "backload=%d", 
			ldap_pvt_thread_pool_backload( &connection_pool ) );

	if ( ( a = attr_find( e->e_attrs, monitor_ad_desc ) ) != NULL ) {

		for ( b = a->a_vals; b[0] != NULL; b++ ) {
			if ( strncmp( b[0]->bv_val, "backload=", 
					sizeof( "backload=" ) - 1 ) == 0 ) {
				ber_bvfree( b[0] );
				b[0] = ber_bvstrdup( buf );
				break;
			}
		}
	}

	if ( b == NULL || b[0] == NULL ) {
		val.bv_val = buf;
		val.bv_len = strlen( buf );
		attr_merge( e, monitor_ad_desc, bv );
	}

	return( 0 );
}

