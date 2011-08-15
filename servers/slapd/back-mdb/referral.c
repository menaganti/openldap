/* referral.c - MDB backend referral handler */
/* $OpenLDAP$ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 2000-2011 The OpenLDAP Foundation.
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

#include "back-mdb.h"

int
mdb_referrals( Operation *op, SlapReply *rs )
{
	struct mdb_info *mdb = (struct mdb_info *) op->o_bd->be_private;
	Entry *e = NULL;
	EntryInfo *ei;
	int rc = LDAP_SUCCESS;

	DB_TXN		*rtxn;
	DB_LOCK		lock;

	if( op->o_tag == LDAP_REQ_SEARCH ) {
		/* let search take care of itself */
		return rc;
	}

	if( get_manageDSAit( op ) ) {
		/* let op take care of DSA management */
		return rc;
	} 

	rc = mdb_reader_get(op, mdb->bi_dbenv, &rtxn);
	switch(rc) {
	case 0:
		break;
	default:
		return LDAP_OTHER;
	}

dn2entry_retry:
	/* get entry */
	rc = mdb_dn2entry( op, rtxn, &op->o_req_ndn, &ei, 1, &lock );

	/* mdb_dn2entry() may legally leave ei == NULL
	 * if rc != 0 and rc != DB_NOTFOUND
	 */
	if ( ei ) {
		e = ei->bei_e;
	}

	switch(rc) {
	case DB_NOTFOUND:
	case 0:
		break;
	case LDAP_BUSY:
		rs->sr_text = "ldap server busy";
		return LDAP_BUSY;
	case DB_LOCK_DEADLOCK:
	case DB_LOCK_NOTGRANTED:
		goto dn2entry_retry;
	default:
		Debug( LDAP_DEBUG_TRACE,
			LDAP_XSTRING(mdb_referrals)
			": dn2entry failed: %s (%d)\n",
			db_strerror(rc), rc, 0 ); 
		rs->sr_text = "internal error";
		return LDAP_OTHER;
	}

	if ( rc == DB_NOTFOUND ) {
		rc = LDAP_SUCCESS;
		rs->sr_matched = NULL;
		if ( e != NULL ) {
			Debug( LDAP_DEBUG_TRACE,
				LDAP_XSTRING(mdb_referrals)
				": tag=%lu target=\"%s\" matched=\"%s\"\n",
				(unsigned long)op->o_tag, op->o_req_dn.bv_val, e->e_name.bv_val );

			if( is_entry_referral( e ) ) {
				BerVarray ref = get_entry_referrals( op, e );
				rc = LDAP_OTHER;
				rs->sr_ref = referral_rewrite( ref, &e->e_name,
					&op->o_req_dn, LDAP_SCOPE_DEFAULT );
				ber_bvarray_free( ref );
				if ( rs->sr_ref ) {
					rs->sr_matched = ber_strdup_x(
					e->e_name.bv_val, op->o_tmpmemctx );
				}
			}

			mdb_cache_return_entry_r (mdb, e, &lock);
			e = NULL;
		}

		if( rs->sr_ref != NULL ) {
			/* send referrals */
			rc = rs->sr_err = LDAP_REFERRAL;
			send_ldap_result( op, rs );
			ber_bvarray_free( rs->sr_ref );
			rs->sr_ref = NULL;
		} else if ( rc != LDAP_SUCCESS ) {
			rs->sr_text = rs->sr_matched ? "bad referral object" : NULL;
		}

		if (rs->sr_matched) {
			op->o_tmpfree( (char *)rs->sr_matched, op->o_tmpmemctx );
			rs->sr_matched = NULL;
		}
		return rc;
	}

	if ( is_entry_referral( e ) ) {
		/* entry is a referral */
		BerVarray refs = get_entry_referrals( op, e );
		rs->sr_ref = referral_rewrite(
			refs, &e->e_name, &op->o_req_dn, LDAP_SCOPE_DEFAULT );

		Debug( LDAP_DEBUG_TRACE,
			LDAP_XSTRING(mdb_referrals)
			": tag=%lu target=\"%s\" matched=\"%s\"\n",
			(unsigned long)op->o_tag, op->o_req_dn.bv_val, e->e_name.bv_val );

		rs->sr_matched = e->e_name.bv_val;
		if( rs->sr_ref != NULL ) {
			rc = rs->sr_err = LDAP_REFERRAL;
			send_ldap_result( op, rs );
			ber_bvarray_free( rs->sr_ref );
			rs->sr_ref = NULL;
		} else {
			rc = LDAP_OTHER;
			rs->sr_text = "bad referral object";
		}

		rs->sr_matched = NULL;
		ber_bvarray_free( refs );
	}

	mdb_cache_return_entry_r(mdb, e, &lock);
	return rc;
}