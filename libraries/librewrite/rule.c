/* $OpenLDAP$ */
/* This work is part of OpenLDAP Software <http://www.openldap.org/>.
 *
 * Copyright 2000-2004 The OpenLDAP Foundation.
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
/* ACKNOWLEDGEMENT:
 * This work was initially developed by Pierangelo Masarati for
 * inclusion in OpenLDAP Software.
 */

#include <portable.h>

#include "rewrite-int.h"

/*
 * Appends a rule to the double linked list of rules
 * Helper for rewrite_rule_compile
 */
static int
append_rule(
		struct rewrite_context *context,
		struct rewrite_rule *rule
)
{
	struct rewrite_rule *r;

	assert( context != NULL );
	assert( context->lc_rule != NULL );
	assert( rule != NULL );

	for ( r = context->lc_rule; r->lr_next != NULL; r = r->lr_next );
	r->lr_next = rule;
	rule->lr_prev = r;
	
	return REWRITE_SUCCESS;
}

/*
 * Appends an action to the linked list of actions
 * Helper for rewrite_rule_compile
 */
static int
append_action(
		struct rewrite_action **pbase,
		struct rewrite_action *action
)
{
	struct rewrite_action **pa;

	assert( pbase != NULL );
	assert( action != NULL );
	
	for ( pa = pbase; *pa != NULL; pa = &(*pa)->la_next );
	*pa = action;
	
	return REWRITE_SUCCESS;
}

static int
destroy_action(
		struct rewrite_action **paction
)
{
	struct rewrite_action	*action;

	assert( paction );
	assert( *paction );

	action = *paction;

	/* do something */
	switch ( action->la_type ) {
	case REWRITE_FLAG_GOTO:
	case REWRITE_FLAG_USER: {
		int *pi = (int *)action->la_args;

		if ( pi ) {
			free( pi );
		}
		break;
	}

	default:
		break;
	}
	
	free( action );
	*paction = NULL;
	
	return 0;
}

/*
 * In case of error it returns NULL and does not free all the memory
 * it allocated; as this is a once only phase, and an error at this stage
 * would require the server to stop, there is no need to be paranoid
 * about memory allocation
 */
int
rewrite_rule_compile(
		struct rewrite_info *info,
		struct rewrite_context *context,
		const char *pattern,
		const char *result,
		const char *flagstring
)
{
	int flags = REWRITE_REGEX_EXTENDED | REWRITE_REGEX_ICASE;
	int mode = REWRITE_RECURSE;
	int max_passes = info->li_max_passes_per_rule;

	struct rewrite_rule *rule = NULL;
	struct rewrite_subst *subst = NULL;
	struct rewrite_action *action = NULL, *first_action = NULL;

	const char *p;

	assert( info != NULL );
	assert( context != NULL );
	assert( pattern != NULL );
	assert( result != NULL );

	/*
	 * A null flagstring should be allowed
	 */

	/*
	 * Take care of substitution string
	 */
	subst = rewrite_subst_compile( info, result );
	if ( subst == NULL ) {
		return REWRITE_ERR;
	}

	/*
	 * Take care of flags
	 */
	for ( p = flagstring; p[ 0 ] != '\0'; p++ ) {
		switch( p[ 0 ] ) {
			
		/*
		 * REGEX flags
		 */
		case REWRITE_FLAG_HONORCASE: 		/* 'C' */
			/*
			 * Honor case (default is case insensitive)
			 */
			flags &= ~REWRITE_REGEX_ICASE;
			break;
			
		case REWRITE_FLAG_BASICREGEX: 		/* 'R' */
			/*
			 * Use POSIX Basic Regular Expression syntax
			 * instead of POSIX Extended Regular Expression 
			 * syntax (default)
			 */
			flags &= ~REWRITE_REGEX_EXTENDED;
			break;
			
		/*
		 * Execution mode flags
		 */
		case REWRITE_FLAG_EXECONCE: 		/* ':' */
			/*
			 * Apply rule once only
			 */
			mode &= ~REWRITE_RECURSE;
			mode |= REWRITE_EXEC_ONCE;
			break;
		
		/*
		 * Special action flags
		 */
		case REWRITE_FLAG_STOP:	 		/* '@' */
			/*
			 * Bail out after applying rule
			 */
			action = calloc( sizeof( struct rewrite_action ), 1 );
			if ( action == NULL ) {
				/* cleanup ... */
				return REWRITE_ERR;
			}
			action->la_type = REWRITE_ACTION_STOP;
			break;
			
		case REWRITE_FLAG_UNWILLING: 		/* '#' */
			/*
			 * Matching objs will be marked as gone!
			 */
			action = calloc( sizeof( struct rewrite_action ), 1 );
			if ( action == NULL ) {
				/* cleanup ... */
				return REWRITE_ERR;
			}
			
			mode &= ~REWRITE_RECURSE;
			mode |= REWRITE_EXEC_ONCE;
			action->la_type = REWRITE_ACTION_UNWILLING;
			break;

		case REWRITE_FLAG_GOTO:				/* 'G' */
			/*
			 * After applying rule, jump N rules
			 */

		case REWRITE_FLAG_USER: {			/* 'U' */
			/*
			 * After applying rule, return user-defined
			 * error code
			 */
			char *next = NULL;
			int *d;
			
			if ( p[ 1 ] != '{' ) {
				/* XXX Need to free stuff */
				return REWRITE_ERR;
			}

			d = malloc( sizeof( int ) );
			if ( d == NULL ) {
				/* XXX Need to free stuff */
				return REWRITE_ERR;
			}

			d[ 0 ] = strtol( &p[ 2 ], &next, 0 );
			if ( next == NULL || next == &p[ 2 ] || next[0] != '}' ) {
				/* XXX Need to free stuff */
				return REWRITE_ERR;
			}

			action = calloc( sizeof( struct rewrite_action ), 1 );
			if ( action == NULL ) {
				/* cleanup ... */       
				return REWRITE_ERR;
			}
			switch ( p[ 0 ] ) {
			case REWRITE_FLAG_GOTO:
				action->la_type = REWRITE_ACTION_GOTO;
				break;

			case REWRITE_FLAG_USER:
				action->la_type = REWRITE_ACTION_USER;
				break;

			default:
				assert(0);
			}

			action->la_args = (void *)d;

			p = next;	/* p is incremented by the for ... */
		
			break;
		}

		case REWRITE_FLAG_MAX_PASSES: {			/* 'U' */
			/*
			 * Set the number of max passes per rule
			 */
			char *next = NULL;
			
			if ( p[ 1 ] != '{' ) {
				/* XXX Need to free stuff */
				return REWRITE_ERR;
			}

			max_passes = strtol( &p[ 2 ], &next, 0 );
			if ( next == NULL || next == &p[ 2 ] || next[0] != '}' ) {
				/* XXX Need to free stuff */
				return REWRITE_ERR;
			}

			if ( max_passes < 1 ) {
				/* FIXME: nonsense ... */
				max_passes = 1;
			}

			p = next;	/* p is incremented by the for ... */
		
			break;
		}

		case REWRITE_FLAG_IGNORE_ERR:               /* 'I' */
			/*
			 * Ignore errors!
			 */
			action = calloc( sizeof( struct rewrite_action ), 1 );
			if ( action == NULL ) {
				/* cleanup ... */
				return REWRITE_ERR;
			}
			
			action->la_type = REWRITE_ACTION_IGNORE_ERR;
			break;
			
		/*
		 * Other flags ...
		 */
		default:
			/*
			 * Unimplemented feature (complain only)
			 */
			break;
		}
		
		/*
		 * Stupid way to append to a list ...
		 */
		if ( action != NULL ) {
			append_action( &first_action, action );
			action = NULL;
		}
	}
	
	/*
	 * Finally, rule allocation
	 */
	rule = calloc( sizeof( struct rewrite_rule ), 1 );
	if ( rule == NULL ) {
		/* charray_free( res ); */
		/*
		 * XXX need to free the value subst stuff!
		 */
		return REWRITE_ERR;
	}
	
	/*
	 * REGEX compilation (luckily I don't need to take care of this ...)
	 */
	if ( regcomp( &rule->lr_regex, ( char * )pattern, flags ) != 0 ) {
		/* charray_free( res ); */
		/*
		 *XXX need to free the value subst stuff!
		 */
		free( rule );
		return REWRITE_ERR;
	}
	
	/*
	 * Just to remember them ...
	 */
	rule->lr_pattern = strdup( pattern );
	rule->lr_subststring = strdup( result );
	rule->lr_flagstring = strdup( flagstring );
	
	/*
	 * Load compiled data into rule
	 */
	rule->lr_subst = subst;

	/*
	 * Set various parameters
	 */
	rule->lr_flags = flags;		/* don't really need any longer ... */
	rule->lr_mode = mode;
	rule->lr_max_passes = max_passes;
	rule->lr_action = first_action;
	
	/*
	 * Append rule at the end of the rewrite context
	 */
	append_rule( context, rule );

	return REWRITE_SUCCESS;
}

/*
 * Rewrites string according to rule; may return:
 *      OK:     fine; if *result != NULL rule matched and rewrite succeeded.
 *      STOP:   fine, rule matched; stop processing following rules
 *      UNWILL: rule matched; force 'unwilling to perform'
 */
int
rewrite_rule_apply(
		struct rewrite_info *info,
		struct rewrite_op *op,
		struct rewrite_rule *rule,
		const char *arg,
		char **result
		)
{
	size_t nmatch = REWRITE_MAX_MATCH;
	regmatch_t match[ REWRITE_MAX_MATCH ];

	int rc = REWRITE_SUCCESS;

	char *string;
	int strcnt = 0;
	struct berval val = { 0, NULL };

	assert( info != NULL );
	assert( op != NULL );
	assert( rule != NULL );
	assert( arg != NULL );
	assert( result != NULL );

	*result = NULL;

	string = (char *)arg;
	
	/*
	 * In case recursive match is required (default)
	 */
recurse:;

	Debug( LDAP_DEBUG_TRACE, "==> rewrite_rule_apply"
			" rule='%s' string='%s' [%s pass(es)]\n", 
			rule->lr_pattern, string, strcnt + 1 );
	
	op->lo_num_passes++;
	if ( regexec( &rule->lr_regex, string, nmatch, match, 0 ) != 0 ) {
		if ( *result == NULL && strcnt > 0 ) {
			free( string );
			string = NULL;
		}

		/*
		 * No match is OK; *result = NULL means no match
		 */
		return REWRITE_REGEXEC_OK;
	}

	rc = rewrite_subst_apply( info, op, rule->lr_subst, string,
			match, &val );

	*result = val.bv_val;
	val.bv_val = NULL;
	if ( strcnt > 0 ) {
		free( string );
		string = NULL;
	}

	if ( rc != REWRITE_REGEXEC_OK ) {
		return rc;
	}

	if ( ( rule->lr_mode & REWRITE_RECURSE ) == REWRITE_RECURSE 
			&& op->lo_num_passes < info->li_max_passes
			&& ++strcnt < rule->lr_max_passes ) {
		string = *result;

		goto recurse;
	}

	return REWRITE_REGEXEC_OK;
}

int
rewrite_rule_destroy(
		struct rewrite_rule **prule
		)
{
	struct rewrite_rule *rule;
	struct rewrite_action *action;

	assert( prule );
	assert( *prule );

	rule = *prule;

	if ( rule->lr_pattern ) {
		free( rule->lr_pattern );
		rule->lr_pattern = NULL;
	}

	if ( rule->lr_subststring ) {
		free( rule->lr_subststring );
		rule->lr_subststring = NULL;
	}

	if ( rule->lr_flagstring ) {
		free( rule->lr_flagstring );
		rule->lr_flagstring = NULL;
	}

	if ( rule->lr_subst ) {
		rewrite_subst_destroy( &rule->lr_subst );
	}

	regfree( &rule->lr_regex );

	for ( action = rule->lr_action; action; ) {
		struct rewrite_action *curraction = action;

		action = action->la_next;
		destroy_action( &curraction );
	}

	free( rule );
	*prule = NULL;

	return 0;
}

