/* $OpenLDAP$ */
/*
 * Copyright 1998-2002 The OpenLDAP Foundation, All Rights Reserved.
 * COPYING RESTRICTIONS APPLY, see COPYRIGHT file
 */
/* ldapmodify.c - generic program to modify or add entries using LDAP */

#include "portable.h"

#include <stdio.h>

#include <ac/stdlib.h>

#include <ac/ctype.h>
#include <ac/signal.h>
#include <ac/string.h>
#include <ac/unistd.h>

#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif

#ifdef HAVE_SYS_FILE_H
#include <sys/file.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif

#include <ldap.h>

#include "lutil.h"
#include "lutil_ldap.h"
#include "ldif.h"
#include "ldap_defaults.h"
#include "ldap_log.h"
/* needed for ldap_control_dup(); we should declare it somewhere else ... */
#include "../../libraries/libldap/ldap-int.h"

static char	*prog;
static char	*binddn = NULL;
static struct berval passwd = { 0, NULL };
static char *ldapuri = NULL;
static char	*ldaphost = NULL;
static int	ldapport = 0;
#ifdef HAVE_CYRUS_SASL
static unsigned sasl_flags = LDAP_SASL_AUTOMATIC;
static char *sasl_realm = NULL;
static char	*sasl_authc_id = NULL;
static char	*sasl_authz_id = NULL;
static char	*sasl_mech = NULL;
static char	*sasl_secprops = NULL;
#endif
static int	use_tls = 0;
static int	ldapadd, not, verbose, contoper, force;
static LDAP	*ld = NULL;

#define LDAPMOD_MAXLINE		4096

/* strings found in replog/LDIF entries (mostly lifted from slurpd/slurp.h) */
#define T_VERSION_STR		"version"
#define T_REPLICA_STR		"replica"
#define T_DN_STR		"dn"
#define T_CONTROL_STR		"control"
#define T_CHANGETYPESTR         "changetype"
#define T_ADDCTSTR		"add"
#define T_MODIFYCTSTR		"modify"
#define T_DELETECTSTR		"delete"
#define T_MODRDNCTSTR		"modrdn"
#define T_MODDNCTSTR		"moddn"
#define T_RENAMECTSTR		"rename"
#define T_MODOPADDSTR		"add"
#define T_MODOPREPLACESTR	"replace"
#define T_MODOPDELETESTR	"delete"
#define T_MODSEPSTR		"-"
#define T_NEWRDNSTR		"newrdn"
#define T_DELETEOLDRDNSTR	"deleteoldrdn"
#define T_NEWSUPSTR		"newsuperior"


static void usage LDAP_P(( const char *prog )) LDAP_GCCATTR((noreturn));
static int process_ldif_rec LDAP_P(( char *rbuf, int count ));
static int parse_ldif_control LDAP_P(( char *line, LDAPControl ***pctrls ));
static void addmodifyop LDAP_P((
	LDAPMod ***pmodsp, int modop,
	const char *attr,
	struct berval *value ));
static int domodify LDAP_P((
	const char *dn,
	LDAPMod **pmods,
    LDAPControl **pctrls,
	int newentry ));
static int dodelete LDAP_P((
	const char *dn,
    LDAPControl **pctrls ));
static int dorename LDAP_P((
	const char *dn,
	const char *newrdn,
	const char *newsup,
	int deleteoldrdn,
    LDAPControl **pctrls ));
static char *read_one_record LDAP_P(( FILE *fp ));

static void
usage( const char *prog )
{
    fprintf( stderr,
"Add or modify entries from an LDAP server\n\n"
"usage: %s [options]\n"
"	The list of desired operations are read from stdin or from the file\n"
"	specified by \"-f file\".\n"
"Add or modify options:\n"
"  -a         add values (default%s)\n"
"  -c         continuous operation mode (do not stop on errors)\n"
"  -F         force all changes records to be used\n"
"  -S file    write skipped modifications to `file'\n"

"Common options:\n"
"  -d level   set LDAP debugging level to `level'\n"
"  -D binddn  bind DN\n"
"  -e [!]<ctrl>[=<ctrlparam>] general controls (! indicates criticality)\n"
"             [!]manageDSAit   (alternate form, see -M)\n"
"             [!]noop\n"
"  -f file    read operations from `file'\n"
"  -h host    LDAP server\n"
"  -H URI     LDAP Uniform Resource Indentifier(s)\n"
"  -I         use SASL Interactive mode\n"
"  -k         use Kerberos authentication\n"
"  -K         like -k, but do only step 1 of the Kerberos bind\n"
"  -M         enable Manage DSA IT control (-MM to make critical)\n"
"  -n         show what would be done but don't actually update\n"
"  -O props   SASL security properties\n"
"  -p port    port on LDAP server\n"
"  -P version procotol version (default: 3)\n"
"  -Q         use SASL Quiet mode\n"
"  -R realm   SASL realm\n"
"  -U authcid SASL authentication identity\n"
"  -v         run in verbose mode (diagnostics to standard output)\n"
"  -w passwd  bind passwd (for simple authentication)\n"
"  -W         prompt for bind passwd\n"
"  -x         Simple authentication\n"
"  -X authzid SASL authorization identity (\"dn:<dn>\" or \"u:<user>\")\n"
"  -y file    Read passwd from file\n"
"  -Y mech    SASL mechanism\n"
"  -Z         Start TLS request (-ZZ to require successful response)\n"
	     , prog, (strcmp( prog, "ldapadd" ) ? " is to replace" : "") );

    exit( EXIT_FAILURE );
}


int
main( int argc, char **argv )
{
    char		*infile, *rejfile, *rbuf, *start, *rejbuf = NULL;
    FILE		*fp, *rejfp;
	char		*matched_msg = NULL, *error_msg = NULL;
	int		rc, i, authmethod, version, want_bindpw, debug, manageDSAit, noop, referrals;
	int count, len;
	char	*pw_file = NULL;
	char	*control, *cvalue;
	int		crit;

    prog = lutil_progname( "ldapmodify", argc, argv );

    /* Print usage when no parameters */
    if( argc < 2 ) usage( prog );

	/* strncmp instead of strcmp since NT binaries carry .exe extension */
    ldapadd = ( strncmp( prog, "ldapadd", sizeof("ldapadd")-1 ) == 0 );

    infile = NULL;
    rejfile = NULL;
    not = verbose = want_bindpw = debug = manageDSAit = noop = referrals = 0;
    authmethod = -1;
	version = -1;

    while (( i = getopt( argc, argv, "acrf:E:F"
		"Cd:D:e:h:H:IkKMnO:p:P:QR:S:U:vw:WxX:y:Y:Z" )) != EOF )
	{
	switch( i ) {
	/* Modify Options */
	case 'a':	/* add */
	    ldapadd = 1;
	    break;
	case 'c':	/* continuous operation */
	    contoper = 1;
	    break;
	case 'E': /* modify controls */
		if( version == LDAP_VERSION2 ) {
			fprintf( stderr, "%s: -E incompatible with LDAPv%d\n",
				prog, version );
			return EXIT_FAILURE;
		}

		/* should be extended to support comma separated list of
		 *	[!]key[=value] parameters, e.g.  -E !foo,bar=567
		 */

		crit = 0;
		cvalue = NULL;
		if( optarg[0] == '!' ) {
			crit = 1;
			optarg++;
		}

		control = ber_strdup( optarg );
		if ( (cvalue = strchr( control, '=' )) != NULL ) {
			*cvalue++ = '\0';
		}
		fprintf( stderr, "Invalid modify control name: %s\n", control );
		usage(prog);
		return EXIT_FAILURE;
	case 'f':	/* read from file */
		if( infile != NULL ) {
			fprintf( stderr, "%s: -f previously specified\n", prog );
			return EXIT_FAILURE;
		}
	    infile = ber_strdup( optarg );
	    break;
	case 'F':	/* force all changes records to be used */
	    force = 1;
	    break;

	/* Common Options */
	case 'C':
		referrals++;
		break;
	case 'd':
	    debug |= atoi( optarg );
	    break;
	case 'D':	/* bind DN */
		if( binddn != NULL ) {
			fprintf( stderr, "%s: -D previously specified\n", prog );
			return EXIT_FAILURE;
		}
	    binddn = ber_strdup( optarg );
	    break;
	case 'e': /* general controls */
		if( version == LDAP_VERSION2 ) {
			fprintf( stderr, "%s: -e incompatible with LDAPv%d\n",
				prog, version );
			return EXIT_FAILURE;
		}

		/* should be extended to support comma separated list of
		 *	[!]key[=value] parameters, e.g.  -e !foo,bar=567
		 */

		crit = 0;
		cvalue = NULL;
		if( optarg[0] == '!' ) {
			crit = 1;
			optarg++;
		}

		control = ber_strdup( optarg );
		if ( (cvalue = strchr( control, '=' )) != NULL ) {
			*cvalue++ = '\0';
		}

		if ( strcasecmp( control, "manageDSAit" ) == 0 ) {
			if( manageDSAit ) {
				fprintf( stderr, "manageDSAit control previously specified");
				return EXIT_FAILURE;
			}
			if( cvalue != NULL ) {
				fprintf( stderr, "manageDSAit: no control value expected" );
				usage(prog);
				return EXIT_FAILURE;
			}

			manageDSAit = 1 + crit;
			free( control );
			break;
			
		} else if ( strcasecmp( control, "noop" ) == 0 ) {
			if( noop ) {
				fprintf( stderr, "noop control previously specified");
				return EXIT_FAILURE;
			}
			if( cvalue != NULL ) {
				fprintf( stderr, "noop: no control value expected" );
				usage(prog);
				return EXIT_FAILURE;
			}

			noop = 1 + crit;
			free( control );
			break;

		} else {
			fprintf( stderr, "Invalid general control name: %s\n", control );
			usage(prog);
			return EXIT_FAILURE;
		}
	case 'h':	/* ldap host */
		if( ldapuri != NULL ) {
			fprintf( stderr, "%s: -h incompatible with -H\n", prog );
			return EXIT_FAILURE;
		}
		if( ldaphost != NULL ) {
			fprintf( stderr, "%s: -h previously specified\n", prog );
			return EXIT_FAILURE;
		}
	    ldaphost = ber_strdup( optarg );
	    break;
	case 'H':	/* ldap URI */
		if( ldaphost != NULL ) {
			fprintf( stderr, "%s: -H incompatible with -h\n", prog );
			return EXIT_FAILURE;
		}
		if( ldapport ) {
			fprintf( stderr, "%s: -H incompatible with -p\n", prog );
			return EXIT_FAILURE;
		}
		if( ldapuri != NULL ) {
			fprintf( stderr, "%s: -H previously specified\n", prog );
			return EXIT_FAILURE;
		}
	    ldapuri = ber_strdup( optarg );
	    break;
	case 'I':
#ifdef HAVE_CYRUS_SASL
		if( version == LDAP_VERSION2 ) {
			fprintf( stderr, "%s: -I incompatible with version %d\n",
				prog, version );
			return EXIT_FAILURE;
		}
		if( authmethod != -1 && authmethod != LDAP_AUTH_SASL ) {
			fprintf( stderr, "%s: incompatible previous "
				"authentication choice\n",
				prog );
			return EXIT_FAILURE;
		}
		authmethod = LDAP_AUTH_SASL;
		version = LDAP_VERSION3;
		sasl_flags = LDAP_SASL_INTERACTIVE;
		break;
#else
		fprintf( stderr, "%s: was not compiled with SASL support\n",
			prog );
		return( EXIT_FAILURE );
#endif
	case 'k':	/* kerberos bind */
#ifdef LDAP_API_FEATURE_X_OPENLDAP_V2_KBIND
		if( version > LDAP_VERSION2 ) {
			fprintf( stderr, "%s: -k incompatible with LDAPv%d\n",
				prog, version );
			return EXIT_FAILURE;
		}

		if( authmethod != -1 ) {
			fprintf( stderr, "%s: -k incompatible with previous "
				"authentication choice\n", prog );
			return EXIT_FAILURE;
		}
			
		authmethod = LDAP_AUTH_KRBV4;
#else
		fprintf( stderr, "%s: not compiled with Kerberos support\n", prog );
		return EXIT_FAILURE;
#endif
	    break;
	case 'K':	/* kerberos bind, part one only */
#ifdef LDAP_API_FEATURE_X_OPENLDAP_V2_KBIND
		if( version > LDAP_VERSION2 ) {
			fprintf( stderr, "%s: -k incompatible with LDAPv%d\n",
				prog, version );
			return EXIT_FAILURE;
		}
		if( authmethod != -1 ) {
			fprintf( stderr, "%s: incompatible with previous "
				"authentication choice\n", prog );
			return EXIT_FAILURE;
		}

		authmethod = LDAP_AUTH_KRBV41;
#else
		fprintf( stderr, "%s: not compiled with Kerberos support\n", prog );
		return( EXIT_FAILURE );
#endif
	    break;
	case 'M':
		/* enable Manage DSA IT */
		if( version == LDAP_VERSION2 ) {
			fprintf( stderr, "%s: -M incompatible with LDAPv%d\n",
				prog, version );
			return EXIT_FAILURE;
		}
		manageDSAit++;
		version = LDAP_VERSION3;
		break;
	case 'n':	/* print deletes, don't actually do them */
	    ++not;
	    break;
	case 'O':
#ifdef HAVE_CYRUS_SASL
		if( sasl_secprops != NULL ) {
			fprintf( stderr, "%s: -O previously specified\n", prog );
			return EXIT_FAILURE;
		}
		if( version == LDAP_VERSION2 ) {
			fprintf( stderr, "%s: -O incompatible with LDAPv%d\n",
				prog, version );
			return EXIT_FAILURE;
		}
		if( authmethod != -1 && authmethod != LDAP_AUTH_SASL ) {
			fprintf( stderr, "%s: incompatible previous "
				"authentication choice\n", prog );
			return EXIT_FAILURE;
		}
		authmethod = LDAP_AUTH_SASL;
		version = LDAP_VERSION3;
		sasl_secprops = ber_strdup( optarg );
#else
		fprintf( stderr, "%s: not compiled with SASL support\n",
			prog );
		return( EXIT_FAILURE );
#endif
		break;
	case 'p':
		if( ldapport ) {
			fprintf( stderr, "%s: -p previously specified\n", prog );
			return EXIT_FAILURE;
		}
	    ldapport = atoi( optarg );
	    break;
	case 'P':
		switch( atoi(optarg) ) {
		case 2:
			if( version == LDAP_VERSION3 ) {
				fprintf( stderr, "%s: -P 2 incompatible with version %d\n",
					prog, version );
				return EXIT_FAILURE;
			}
			version = LDAP_VERSION2;
			break;
		case 3:
			if( version == LDAP_VERSION2 ) {
				fprintf( stderr, "%s: -P 2 incompatible with version %d\n",
					prog, version );
				return EXIT_FAILURE;
			}
			version = LDAP_VERSION3;
			break;
		default:
			fprintf( stderr, "%s: protocol version should be 2 or 3\n",
				prog );
			usage( prog );
			return( EXIT_FAILURE );
		} break;
	case 'Q':
#ifdef HAVE_CYRUS_SASL
		if( version == LDAP_VERSION2 ) {
			fprintf( stderr, "%s: -Q incompatible with version %d\n",
				prog, version );
			return EXIT_FAILURE;
		}
		if( authmethod != -1 && authmethod != LDAP_AUTH_SASL ) {
			fprintf( stderr, "%s: incompatible previous "
				"authentication choice\n",
				prog );
			return EXIT_FAILURE;
		}
		authmethod = LDAP_AUTH_SASL;
		version = LDAP_VERSION3;
		sasl_flags = LDAP_SASL_QUIET;
		break;
#else
		fprintf( stderr, "%s: not compiled with SASL support\n",
			prog );
		return( EXIT_FAILURE );
#endif
	case 'r':	/* replace (obsolete) */
		break;

	case 'R':
#ifdef HAVE_CYRUS_SASL
		if( sasl_realm != NULL ) {
			fprintf( stderr, "%s: -R previously specified\n", prog );
			return EXIT_FAILURE;
		}
		if( version == LDAP_VERSION2 ) {
			fprintf( stderr, "%s: -R incompatible with version %d\n",
				prog, version );
			return EXIT_FAILURE;
		}
		if( authmethod != -1 && authmethod != LDAP_AUTH_SASL ) {
			fprintf( stderr, "%s: incompatible previous "
				"authentication choice\n",
				prog );
			return EXIT_FAILURE;
		}
		authmethod = LDAP_AUTH_SASL;
		version = LDAP_VERSION3;
		sasl_realm = ber_strdup( optarg );
#else
		fprintf( stderr, "%s: not compiled with SASL support\n",
			prog );
		return( EXIT_FAILURE );
#endif
		break;
	case 'S':	/* skipped modifications to file */
		if( rejfile != NULL ) {
			fprintf( stderr, "%s: -S previously specified\n", prog );
			return EXIT_FAILURE;
		}
		rejfile = ber_strdup( optarg );
		break;
	case 'U':
#ifdef HAVE_CYRUS_SASL
		if( sasl_authc_id != NULL ) {
			fprintf( stderr, "%s: -U previously specified\n", prog );
			return EXIT_FAILURE;
		}
		if( version == LDAP_VERSION2 ) {
			fprintf( stderr, "%s: -U incompatible with version %d\n",
				prog, version );
			return EXIT_FAILURE;
		}
		if( authmethod != -1 && authmethod != LDAP_AUTH_SASL ) {
			fprintf( stderr, "%s: incompatible previous "
				"authentication choice\n",
				prog );
			return EXIT_FAILURE;
		}
		authmethod = LDAP_AUTH_SASL;
		version = LDAP_VERSION3;
		sasl_authc_id = ber_strdup( optarg );
#else
		fprintf( stderr, "%s: not compiled with SASL support\n",
			prog );
		return( EXIT_FAILURE );
#endif
		break;
	case 'v':	/* verbose mode */
	    verbose++;
	    break;
	case 'w':	/* password */
	    passwd.bv_val = ber_strdup( optarg );
		{
			char* p;

			for( p = optarg; *p != '\0'; p++ ) {
				*p = '\0';
			}
		}
		passwd.bv_len = strlen( passwd.bv_val );
	    break;
	case 'W':
		want_bindpw++;
		break;
	case 'y':
		pw_file = optarg;
		break;
	case 'Y':
#ifdef HAVE_CYRUS_SASL
		if( sasl_mech != NULL ) {
			fprintf( stderr, "%s: -Y previously specified\n", prog );
			return EXIT_FAILURE;
		}
		if( version == LDAP_VERSION2 ) {
			fprintf( stderr, "%s: -Y incompatible with version %d\n",
				prog, version );
			return EXIT_FAILURE;
		}
		if( authmethod != -1 && authmethod != LDAP_AUTH_SASL ) {
			fprintf( stderr, "%s: incompatible with authentication choice\n", prog );
			return EXIT_FAILURE;
		}
		authmethod = LDAP_AUTH_SASL;
		version = LDAP_VERSION3;
		sasl_mech = ber_strdup( optarg );
#else
		fprintf( stderr, "%s: not compiled with SASL support\n",
			prog );
		return( EXIT_FAILURE );
#endif
		break;
	case 'x':
		if( authmethod != -1 && authmethod != LDAP_AUTH_SIMPLE ) {
			fprintf( stderr, "%s: incompatible with previous "
				"authentication choice\n", prog );
			return EXIT_FAILURE;
		}
		authmethod = LDAP_AUTH_SIMPLE;
		break;
	case 'X':
#ifdef HAVE_CYRUS_SASL
		if( sasl_authz_id != NULL ) {
			fprintf( stderr, "%s: -X previously specified\n", prog );
			return EXIT_FAILURE;
		}
		if( version == LDAP_VERSION2 ) {
			fprintf( stderr, "%s: -X incompatible with LDAPv%d\n",
				prog, version );
			return EXIT_FAILURE;
		}
		if( authmethod != -1 && authmethod != LDAP_AUTH_SASL ) {
			fprintf( stderr, "%s: -X incompatible with "
				"authentication choice\n", prog );
			return EXIT_FAILURE;
		}
		authmethod = LDAP_AUTH_SASL;
		version = LDAP_VERSION3;
		sasl_authz_id = ber_strdup( optarg );
#else
		fprintf( stderr, "%s: not compiled with SASL support\n",
			prog );
		return( EXIT_FAILURE );
#endif
		break;
	case 'Z':
#ifdef HAVE_TLS
		if( version == LDAP_VERSION2 ) {
			fprintf( stderr, "%s: -Z incompatible with version %d\n",
				prog, version );
			return EXIT_FAILURE;
		}
		version = LDAP_VERSION3;
		use_tls++;
#else
		fprintf( stderr, "%s: not compiled with TLS support\n",
			prog );
		return( EXIT_FAILURE );
#endif
		break;
	default:
		fprintf( stderr, "%s: unrecognized option -%c\n",
			prog, optopt );
	    usage( prog );
	}
    }

	if (version == -1) {
		version = LDAP_VERSION3;
	}
	if (authmethod == -1 && version > LDAP_VERSION2) {
#ifdef HAVE_CYRUS_SASL
		authmethod = LDAP_AUTH_SASL;
#else
		authmethod = LDAP_AUTH_SIMPLE;
#endif
	}

	if ( argc != optind )
	usage( prog );

    if ( rejfile != NULL ) {
	if (( rejfp = fopen( rejfile, "w" )) == NULL ) {
	    perror( rejfile );
	    return( EXIT_FAILURE );
	}
    } else {
	rejfp = NULL;
    }

    if ( infile != NULL ) {
	if (( fp = fopen( infile, "r" )) == NULL ) {
	    perror( infile );
	    return( EXIT_FAILURE );
	}
    } else {
	fp = stdin;
    }

	if ( debug ) {
		if( ber_set_option( NULL, LBER_OPT_DEBUG_LEVEL, &debug ) != LBER_OPT_SUCCESS ) {
			fprintf( stderr, "Could not set LBER_OPT_DEBUG_LEVEL %d\n", debug );
		}
		if( ldap_set_option( NULL, LDAP_OPT_DEBUG_LEVEL, &debug ) != LDAP_OPT_SUCCESS ) {
			fprintf( stderr, "Could not set LDAP_OPT_DEBUG_LEVEL %d\n", debug );
		}
		ldif_debug = debug;
	}

#ifdef SIGPIPE
	(void) SIGNAL( SIGPIPE, SIG_IGN );
#endif

    if ( !not ) {
	if( ( ldaphost != NULL || ldapport ) && ( ldapuri == NULL ) ) {
		if ( verbose ) {
			fprintf( stderr, "ldap_init( %s, %d )\n",
				ldaphost != NULL ? ldaphost : "<DEFAULT>",
				ldapport );
		}

		ld = ldap_init( ldaphost, ldapport );
		if( ld == NULL ) {
			perror("ldapmodify: ldap_init");
			return EXIT_FAILURE;
		}

	} else {
		if ( verbose ) {
			fprintf( stderr, "ldap_initialize( %s )\n",
				ldapuri != NULL ? ldapuri : "<DEFAULT>" );
		}

		rc = ldap_initialize( &ld, ldapuri );
		if( rc != LDAP_SUCCESS ) {
			fprintf( stderr, "Could not create LDAP session handle (%d): %s\n",
				rc, ldap_err2string(rc) );
			return EXIT_FAILURE;
		}
	}

	/* referrals */
	if( ldap_set_option( ld, LDAP_OPT_REFERRALS,
		referrals ? LDAP_OPT_ON : LDAP_OPT_OFF ) != LDAP_OPT_SUCCESS )
	{
		fprintf( stderr, "Could not set LDAP_OPT_REFERRALS %s\n",
			referrals ? "on" : "off" );
		return EXIT_FAILURE;
	}


	if (version == -1 ) {
		version = LDAP_VERSION3;
	}

	if( ldap_set_option( ld, LDAP_OPT_PROTOCOL_VERSION, &version )
		!= LDAP_OPT_SUCCESS )
	{
		fprintf( stderr, "Could not set LDAP_OPT_PROTOCOL_VERSION %d\n",
			version );
		return EXIT_FAILURE;
	}

	if ( use_tls && ( ldap_start_tls_s( ld, NULL, NULL ) != LDAP_SUCCESS )) {
		ldap_perror( ld, "ldap_start_tls" );
		if ( use_tls > 1 ) {
			return( EXIT_FAILURE );
		}
	}

	if ( pw_file || want_bindpw ) {
		if ( pw_file ) {
			rc = lutil_get_filed_password( pw_file, &passwd );
			if( rc ) return EXIT_FAILURE;
		} else {
			passwd.bv_val = getpassphrase( "Enter LDAP Password: " );
			passwd.bv_len = passwd.bv_val ? strlen( passwd.bv_val ) : 0;
		}
	}

	if ( authmethod == LDAP_AUTH_SASL ) {
#ifdef HAVE_CYRUS_SASL
		void *defaults;

		if( sasl_secprops != NULL ) {
			rc = ldap_set_option( ld, LDAP_OPT_X_SASL_SECPROPS,
				(void *) sasl_secprops );
			
			if( rc != LDAP_OPT_SUCCESS ) {
				fprintf( stderr,
					"Could not set LDAP_OPT_X_SASL_SECPROPS: %s\n",
					sasl_secprops );
				return( EXIT_FAILURE );
			}
		}
		
		defaults = lutil_sasl_defaults( ld,
			sasl_mech,
			sasl_realm,
			sasl_authc_id,
			passwd.bv_val,
			sasl_authz_id );

		rc = ldap_sasl_interactive_bind_s( ld, binddn,
			sasl_mech, NULL, NULL,
			sasl_flags, lutil_sasl_interact, defaults );

		if( rc != LDAP_SUCCESS ) {
			ldap_perror( ld, "ldap_sasl_interactive_bind_s" );
			return( EXIT_FAILURE );
		}
#else
		fprintf( stderr, "%s: not compiled with SASL support\n",
			prog );
		return( EXIT_FAILURE );
#endif
	}
	else {
		if ( ldap_bind_s( ld, binddn, passwd.bv_val, authmethod )
				!= LDAP_SUCCESS ) {
			ldap_perror( ld, "ldap_bind" );
			return( EXIT_FAILURE );
		}

	}

    }

    rc = 0;

	if ( manageDSAit || noop ) {
		int err, i = 0;
		LDAPControl c1, c2;
		LDAPControl *ctrls[3];

		if ( manageDSAit ) {
			ctrls[i++] = &c1;
			ctrls[i] = NULL;
			c1.ldctl_oid = LDAP_CONTROL_MANAGEDSAIT;
			c1.ldctl_value.bv_val = NULL;
			c1.ldctl_value.bv_len = 0;
			c1.ldctl_iscritical = manageDSAit > 1;
		}

		if ( noop ) {
			ctrls[i++] = &c2;
			ctrls[i] = NULL;

			c2.ldctl_oid = LDAP_CONTROL_NOOP;
			c2.ldctl_value.bv_val = NULL;
			c2.ldctl_value.bv_len = 0;
			c2.ldctl_iscritical = noop > 1;
		}
	
		err = ldap_set_option( ld, LDAP_OPT_SERVER_CONTROLS, ctrls );

		if( err != LDAP_OPT_SUCCESS ) {
			fprintf( stderr, "Could not set %scontrols\n",
				(c1.ldctl_iscritical || c2.ldctl_iscritical)
				? "critical " : "" );
			if ( c1.ldctl_iscritical && c2.ldctl_iscritical ) {
				return EXIT_FAILURE;
			}
		}
	}

	count = 0;
    while (( rc == 0 || contoper ) &&
		( rbuf = read_one_record( fp )) != NULL ) {
	count++;

	start = rbuf;

	if ( rejfp ) {
		len = strlen( rbuf );
		if (( rejbuf = (char *)ber_memalloc( len+1 )) == NULL ) {
			perror( "malloc" );
			exit( EXIT_FAILURE );
		}
		memcpy( rejbuf, rbuf, len+1 );
	}

    rc = process_ldif_rec( start, count );

	if ( rc && rejfp ) {
		fprintf(rejfp, "# Error: %s (%d)", ldap_err2string(rc), rc);

		ldap_get_option(ld, LDAP_OPT_MATCHED_DN, &matched_msg);
		if ( matched_msg != NULL && *matched_msg != '\0' ) {
			fprintf( rejfp, ", matched DN: %s", matched_msg );
		}

		ldap_get_option(ld, LDAP_OPT_ERROR_STRING, &error_msg);
		if ( error_msg != NULL && *error_msg != '\0' ) {
			fprintf( rejfp, ", additional info: %s", error_msg );
		}
		fprintf( rejfp, "\n%s\n", rejbuf );
	}
		if (rejfp) 
			free( rejbuf );
		free( rbuf );
    }

    if ( !not ) {
		ldap_unbind( ld );
    }

    if ( rejfp != NULL ) {
	    fclose( rejfp );
    }

	return( rc );
}


static int
process_ldif_rec( char *rbuf, int count )
{
    char	*line, *dn, *type, *newrdn, *newsup, *p;
    int		rc, linenum, modop, replicaport;
    int		expect_modop, expect_sep, expect_ct, expect_newrdn, expect_newsup;
    int		expect_deleteoldrdn, deleteoldrdn;
    int		saw_replica, use_record, new_entry, delete_entry, got_all;
    LDAPMod	**pmods;
	int version;
	struct berval val;
    LDAPControl **pctrls;

    new_entry = ldapadd;

    rc = got_all = saw_replica = delete_entry = modop = expect_modop = 0;
    expect_deleteoldrdn = expect_newrdn = expect_newsup = 0;
	expect_sep = expect_ct = 0;
    linenum = 0;
	version = 0;
    deleteoldrdn = 1;
    use_record = force;
    pmods = NULL;
    pctrls = NULL;
    dn = newrdn = newsup = NULL;

    while ( rc == 0 && ( line = ldif_getline( &rbuf )) != NULL ) {
	++linenum;

	if ( expect_sep && strcasecmp( line, T_MODSEPSTR ) == 0 ) {
	    expect_sep = 0;
	    expect_ct = 1;
	    continue;
	}
	
	if ( ldif_parse_line( line, &type, &val.bv_val, &val.bv_len ) < 0 ) {
	    fprintf( stderr, "%s: invalid format (line %d) entry: \"%s\"\n",
		    prog, linenum, dn == NULL ? "" : dn );
	    rc = LDAP_PARAM_ERROR;
	    break;
	}

	if ( dn == NULL ) {
	    if ( !use_record && strcasecmp( type, T_REPLICA_STR ) == 0 ) {
		++saw_replica;
		if (( p = strchr( val.bv_val, ':' )) == NULL ) {
		    replicaport = 0;
		} else {
		    *p++ = '\0';
		    replicaport = atoi( p );
		}
		if ( ldaphost != NULL && strcasecmp( val.bv_val, ldaphost ) == 0 &&
			replicaport == ldapport ) {
		    use_record = 1;
		}
	    } else if ( count == 1 && linenum == 1 && 
			strcasecmp( type, T_VERSION_STR ) == 0 )
		{
			if( val.bv_len == 0 || atoi(val.bv_val) != 1 ) {
		    	fprintf( stderr, "%s: invalid version %s, line %d (ignored)\n",
			   	prog, val.bv_val == NULL ? "(null)" : val.bv_val, linenum );
			}
			version++;

	    } else if ( strcasecmp( type, T_DN_STR ) == 0 ) {
		if (( dn = ber_strdup( val.bv_val ? val.bv_val : "" )) == NULL ) {
		    perror( "strdup" );
		    exit( EXIT_FAILURE );
		}
		expect_ct = 1;
	    }
	    goto end_line;	/* skip all lines until we see "dn:" */
	}

	if ( expect_ct ) {
        
        /* Check for "control" tag after dn and before changetype. */
        if (strcasecmp(type, T_CONTROL_STR) == 0) {
            /* Parse and add it to the list of controls */
            rc = parse_ldif_control( line, &pctrls );
            if (rc != 0) {
		    	fprintf( stderr, "%s: Error processing %s line, line %d: %s\n",
			   	prog, T_CONTROL_STR, linenum, ldap_err2string(rc) );
            }
            goto end_line;
        }
        
	    expect_ct = 0;
	    if ( !use_record && saw_replica ) {
		printf( "%s: skipping change record for entry: %s\n"
			"\t(LDAP host/port does not match replica: lines)\n",
			prog, dn );
		free( dn );
		ber_memfree( type );
		ber_memfree( val.bv_val );
		return( 0 );
	    }

	    if ( strcasecmp( type, T_CHANGETYPESTR ) == 0 ) {
#ifdef LIBERAL_CHANGETYPE_MODOP
		/* trim trailing spaces (and log warning ...) */

		int icnt;
		for ( icnt = val.bv_len; --icnt > 0; ) {
		    if ( !isspace( (unsigned char) val.bv_val[icnt] ) ) {
			break;
		    }
		}

		if ( ++icnt != val.bv_len ) {
		    fprintf( stderr, "%s: illegal trailing space after \"%s: %s\" trimmed (line %d of entry \"%s\")\n",
			    prog, T_CHANGETYPESTR, val.bv_val, linenum, dn );
		    val.bv_val[icnt] = '\0';
		}
#endif /* LIBERAL_CHANGETYPE_MODOP */

		if ( strcasecmp( val.bv_val, T_MODIFYCTSTR ) == 0 ) {
			new_entry = 0;
			expect_modop = 1;
		} else if ( strcasecmp( val.bv_val, T_ADDCTSTR ) == 0 ) {
			new_entry = 1;
		} else if ( strcasecmp( val.bv_val, T_MODRDNCTSTR ) == 0
			|| strcasecmp( val.bv_val, T_MODDNCTSTR ) == 0
			|| strcasecmp( val.bv_val, T_RENAMECTSTR ) == 0)
		{
		    expect_newrdn = 1;
		} else if ( strcasecmp( val.bv_val, T_DELETECTSTR ) == 0 ) {
		    got_all = delete_entry = 1;
		} else {
		    fprintf( stderr,
			    "%s:  unknown %s \"%s\" (line %d of entry \"%s\")\n",
			    prog, T_CHANGETYPESTR, val.bv_val, linenum, dn );
		    rc = LDAP_PARAM_ERROR;
		}
		goto end_line;
	    } else if ( ldapadd ) {		/*  missing changetype => add */
		new_entry = 1;
		modop = LDAP_MOD_ADD;
	    } else {
		expect_modop = 1;	/* missing changetype => modify */
	    }
	}

	if ( expect_modop ) {
#ifdef LIBERAL_CHANGETYPE_MODOP
	    /* trim trailing spaces (and log warning ...) */
	    
	    int icnt;
	    for ( icnt = val.bv_len; --icnt > 0; ) {
		if ( !isspace( (unsigned char) val.bv_val[icnt] ) ) {
		    break;
		}
	    }
	    
	    if ( ++icnt != val.bv_len ) {
		fprintf( stderr, "%s: illegal trailing space after \"%s: %s\" trimmed (line %d of entry \"%s\")\n",
    			prog, type, val.bv_val, linenum, dn );
		val.bv_val[icnt] = '\0';
	    }
#endif /* LIBERAL_CHANGETYPE_MODOP */

	    expect_modop = 0;
	    expect_sep = 1;
	    if ( strcasecmp( type, T_MODOPADDSTR ) == 0 ) {
		modop = LDAP_MOD_ADD;
		goto end_line;
	    } else if ( strcasecmp( type, T_MODOPREPLACESTR ) == 0 ) {
		modop = LDAP_MOD_REPLACE;
		addmodifyop( &pmods, modop, val.bv_val, NULL );
		goto end_line;
	    } else if ( strcasecmp( type, T_MODOPDELETESTR ) == 0 ) {
		modop = LDAP_MOD_DELETE;
		addmodifyop( &pmods, modop, val.bv_val, NULL );
		goto end_line;
	    } else {	/* no modify op:  use default */
		modop = ldapadd ? LDAP_MOD_ADD : LDAP_MOD_REPLACE;
	    }
	}

	if ( expect_newrdn ) {
	    if ( strcasecmp( type, T_NEWRDNSTR ) == 0 ) {
			if (( newrdn = ber_strdup( val.bv_val ? val.bv_val : "" )) == NULL ) {
		    perror( "strdup" );
		    exit( EXIT_FAILURE );
		}
		expect_deleteoldrdn = 1;
		expect_newrdn = 0;
	    } else {
		fprintf( stderr, "%s: expecting \"%s:\" but saw \"%s:\" (line %d of entry \"%s\")\n",
			prog, T_NEWRDNSTR, type, linenum, dn );
		rc = LDAP_PARAM_ERROR;
	    }
	} else if ( expect_deleteoldrdn ) {
	    if ( strcasecmp( type, T_DELETEOLDRDNSTR ) == 0 ) {
		deleteoldrdn = ( *val.bv_val == '0' ) ? 0 : 1;
		expect_deleteoldrdn = 0;
		expect_newsup = 1;
		got_all = 1;
	    } else {
		fprintf( stderr, "%s: expecting \"%s:\" but saw \"%s:\" (line %d of entry \"%s\")\n",
			prog, T_DELETEOLDRDNSTR, type, linenum, dn );
		rc = LDAP_PARAM_ERROR;
	    }
	} else if ( expect_newsup ) {
	    if ( strcasecmp( type, T_NEWSUPSTR ) == 0 ) {
		if (( newsup = ber_strdup( val.bv_val ? val.bv_val : "" )) == NULL ) {
		    perror( "strdup" );
		    exit( EXIT_FAILURE );
		}
		expect_newsup = 0;
	    } else {
		fprintf( stderr, "%s: expecting \"%s:\" but saw \"%s:\" (line %d of entry \"%s\")\n",
			prog, T_NEWSUPSTR, type, linenum, dn );
		rc = LDAP_PARAM_ERROR;
	    }
	} else if ( got_all ) {
	    fprintf( stderr,
		    "%s: extra lines at end (line %d of entry \"%s\")\n",
		    prog, linenum, dn );
	    rc = LDAP_PARAM_ERROR;
	} else {
		addmodifyop( &pmods, modop, type, val.bv_val == NULL ? NULL : &val );
	}

end_line:
	ber_memfree( type );
	ber_memfree( val.bv_val );
    }

	if( linenum == 0 ) {
		return 0;
	}

	if( version && linenum == 1 ) {
		return 0;
	}

    /* If default controls are set (as with -M option) and controls are
       specified in the LDIF file, we must add the default controls to
       the list of controls sent with the ldap operation.
    */
    if ( rc == 0 ) {
        if (pctrls) {
            LDAPControl **defctrls = NULL;   /* Default server controls */
            LDAPControl **newctrls = NULL;
            ldap_get_option(ld, LDAP_OPT_SERVER_CONTROLS, &defctrls);
            if (defctrls) {
                int npc=0;                  /* Number of LDIF controls */
                int ndefc=0;                /* Number of default controls */
                while (pctrls[npc])         /* Count LDIF controls */
                    npc++; 
                while (defctrls[ndefc])     /* Count default controls */
                    ndefc++;
                newctrls = ber_memrealloc(pctrls, (npc+ndefc+1)*sizeof(LDAPControl*));
                if (newctrls == NULL)
                    rc = LDAP_NO_MEMORY;
                else {
                    int i;
                    pctrls = newctrls;
                    for (i=npc; i<npc+ndefc; i++) {
                        pctrls[i] = ldap_control_dup(defctrls[i-npc]);
                        if (pctrls[i] == NULL) {
                            rc = LDAP_NO_MEMORY;
                            break;
                        }
                    }
                    pctrls[npc+ndefc] = NULL;
                    ldap_controls_free(defctrls);  /* Must be freed by library */
                }
            }
        }
    }


    if ( rc == 0 ) {
	if ( delete_entry ) {
	    rc = dodelete( dn, pctrls );
	} else if ( newrdn != NULL ) {
	    rc = dorename( dn, newrdn, newsup, deleteoldrdn, pctrls );
	} else {
	    rc = domodify( dn, pmods, pctrls, new_entry );
	}

	if ( rc == LDAP_SUCCESS ) {
	    rc = 0;
	}
    }

    if ( dn != NULL ) {
	free( dn );
    }
    if ( newrdn != NULL ) {
	free( newrdn );
    }
    if ( pmods != NULL ) {
	ldap_mods_free( pmods, 1 );
    }

    if (pctrls != NULL) {
        ldap_controls_free( pctrls );
    }

    return( rc );
}

/* Parse an LDIF control line of the form
      control:  oid  [true/false]  [: value]              or
      control:  oid  [true/false]  [:: base64-value]      or
      control:  oid  [true/false]  [:< url]
   The control is added to the list of controls in *ppctrls.
*/      
static int
parse_ldif_control( char *line, 
                    LDAPControl ***ppctrls )
{
    char *oid = NULL;
    int criticality = 0;   /* Default is false if not present */
    char *type=NULL;
    char *val = NULL;
    ber_len_t value_len = 0;
    int i, rc=0;
    char *s, *oidStart, *pcolon;
    LDAPControl *newctrl = NULL;
    LDAPControl **pctrls = NULL;

    if (ppctrls) {
        pctrls = *ppctrls;
    }
    s = line + strlen(T_CONTROL_STR);  /* Skip over "control" */
    pcolon = s;                        /* Save this position for later */
    if (*s++ != ':')                   /* Make sure colon follows */
        return ( LDAP_PARAM_ERROR );
    while (*s && isspace(*s))  s++;    /* Skip white space before OID */

    /* OID should come next. Validate and extract it. */
    if (*s == 0)
        return ( LDAP_PARAM_ERROR );
    oidStart = s;
    while (isdigit(*s) || *s == '.')  s++;    /* OID should be digits or . */
    if (s == oidStart) 
        return ( LDAP_PARAM_ERROR );   /* OID was not present */
    if (*s) {                          /* End of OID should be space or NULL */
        if (!isspace(*s))
            return ( LDAP_PARAM_ERROR ); /* else OID contained invalid chars */
        *s++ = 0;                    /* Replace space with null to terminate */
    }

    
    oid = ber_strdup(oidStart);
    if (oid == NULL)
        return ( LDAP_NO_MEMORY );

    /* Optional Criticality field is next. */
    while (*s && isspace(*s))  s++;   /* Skip white space before criticality */
    if (strncasecmp(s, "true", 4) == 0) {
        criticality = 1;
        s += 4;
    } 
    else if (strncasecmp(s, "false", 5) == 0) {
        criticality = 0;
        s += 5;
    }

    /* Optional value field is next */
    while (*s && isspace(*s))  s++;    /* Skip white space before value */
    if (*s) {
        if (*s != ':') {           /* If value is present, must start with : */
            rc = LDAP_PARAM_ERROR;
            goto cleanup;
        }

        /* Shift value down over OID and criticality so it's in the form
             control: value
             control:: base64-value
             control:< url
           Then we can use ldif_parse_line to extract and decode the value
        */
        while ( (*pcolon++ = *s++) != 0)     /* Shift value */
            ;
        rc = ldif_parse_line(line, &type, &val, &value_len);
        if (type)  ber_memfree(type);   /* Don't need this field*/
        if (rc < 0) {
            rc = LDAP_PARAM_ERROR;
            goto cleanup;
        }
    }

    /* Create a new LDAPControl structure. */
    newctrl = (LDAPControl *)ber_memalloc(sizeof(LDAPControl));
    if ( newctrl == NULL ) {
        rc = LDAP_NO_MEMORY;
        goto cleanup;
    }
    newctrl->ldctl_oid = oid;
    oid = NULL;
    newctrl->ldctl_iscritical = criticality;
    newctrl->ldctl_value.bv_len = value_len;
    newctrl->ldctl_value.bv_val = val;
    val = NULL;

    /* Add the new control to the passed-in list of controls. */
    i = 0;
    if (pctrls) {
        while ( pctrls[i] )      /* Count the # of controls passed in */
            i++;
    }
    /* Allocate 1 more slot for the new control and 1 for the NULL. */
    pctrls = (LDAPControl **)ber_memrealloc(pctrls, (i+2)*(sizeof(LDAPControl *)));
    if (pctrls == NULL) {
        rc = LDAP_NO_MEMORY;
        goto cleanup;
    }
    pctrls[i] = newctrl;
    newctrl = NULL;
    pctrls[i+1] = NULL;
    *ppctrls = pctrls;

cleanup:
    if (newctrl) {
        if (newctrl->ldctl_oid)
            ber_memfree(newctrl->ldctl_oid);
        if (newctrl->ldctl_value.bv_val)
            ber_memfree(newctrl->ldctl_value.bv_val);
        ber_memfree(newctrl);
    }
    if (val)
        ber_memfree(val);
    if (oid)
        ber_memfree(oid);

    return( rc );
}


static void
addmodifyop(
	LDAPMod ***pmodsp,
	int modop,
	const char *attr,
	struct berval *val )
{
	LDAPMod		**pmods;
	int			i, j;

	pmods = *pmodsp;
	modop |= LDAP_MOD_BVALUES;

	i = 0;
	if ( pmods != NULL ) {
		for ( ; pmods[ i ] != NULL; ++i ) {
			if ( strcasecmp( pmods[ i ]->mod_type, attr ) == 0 &&
				pmods[ i ]->mod_op == modop )
			{
				break;
			}
		}
	}

	if ( pmods == NULL || pmods[ i ] == NULL ) {
		if (( pmods = (LDAPMod **)ber_memrealloc( pmods, (i + 2) *
			sizeof( LDAPMod * ))) == NULL )
		{
			perror( "realloc" );
			exit( EXIT_FAILURE );
		}

		*pmodsp = pmods;
		pmods[ i + 1 ] = NULL;

		pmods[ i ] = (LDAPMod *)ber_memcalloc( 1, sizeof( LDAPMod ));
		if ( pmods[ i ] == NULL ) {
			perror( "calloc" );
			exit( EXIT_FAILURE );
		}

		pmods[ i ]->mod_op = modop;
		pmods[ i ]->mod_type = ber_strdup( attr );
		if ( pmods[ i ]->mod_type == NULL ) {
			perror( "strdup" );
			exit( EXIT_FAILURE );
		}
	}

	if ( val != NULL ) {
		j = 0;
		if ( pmods[ i ]->mod_bvalues != NULL ) {
			for ( ; pmods[ i ]->mod_bvalues[ j ] != NULL; ++j ) {
				/* Empty */;
			}
		}

		pmods[ i ]->mod_bvalues = (struct berval **) ber_memrealloc(
			pmods[ i ]->mod_bvalues, (j + 2) * sizeof( struct berval * ));
		if ( pmods[ i ]->mod_bvalues == NULL ) {
			perror( "ber_realloc" );
			exit( EXIT_FAILURE );
		}

		pmods[ i ]->mod_bvalues[ j + 1 ] = NULL;
		pmods[ i ]->mod_bvalues[ j ] = ber_bvdup( val );
		if ( pmods[ i ]->mod_bvalues[ j ] == NULL ) {
			perror( "ber_bvdup" );
			exit( EXIT_FAILURE );
		}
	}
}


static int
domodify(
	const char *dn,
	LDAPMod **pmods,
    LDAPControl **pctrls,
	int newentry )
{
    int			i, j, k, notascii, op;
    struct berval	*bvp;

    if ( pmods == NULL ) {
	fprintf( stderr, "%s: no attributes to change or add (entry=\"%s\")\n",
		prog, dn );
	return( LDAP_PARAM_ERROR );
    } 

    for ( i = 0; pmods[ i ] != NULL; ++i ) {
	op = pmods[ i ]->mod_op & ~LDAP_MOD_BVALUES;
	if( op == LDAP_MOD_ADD && ( pmods[i]->mod_bvalues == NULL )) {
		fprintf( stderr,
			"%s: attribute \"%s\" has no values (entry=\"%s\")\n",
			prog, pmods[i]->mod_type, dn );
		return LDAP_PARAM_ERROR;
	}
    }

    if ( verbose ) {
	for ( i = 0; pmods[ i ] != NULL; ++i ) {
	    op = pmods[ i ]->mod_op & ~LDAP_MOD_BVALUES;
	    printf( "%s %s:\n", op == LDAP_MOD_REPLACE ?
		    "replace" : op == LDAP_MOD_ADD ?
		    "add" : "delete", pmods[ i ]->mod_type );
	    if ( pmods[ i ]->mod_bvalues != NULL ) {
		for ( j = 0; pmods[ i ]->mod_bvalues[ j ] != NULL; ++j ) {
		    bvp = pmods[ i ]->mod_bvalues[ j ];
		    notascii = 0;
		    for ( k = 0; (unsigned long) k < bvp->bv_len; ++k ) {
			if ( !isascii( bvp->bv_val[ k ] )) {
			    notascii = 1;
			    break;
			}
		    }
		    if ( notascii ) {
			printf( "\tNOT ASCII (%ld bytes)\n", bvp->bv_len );
		    } else {
			printf( "\t%s\n", bvp->bv_val );
		    }
		}
	    }
	}
    }

    if ( newentry ) {
	printf( "%sadding new entry \"%s\"\n", not ? "!" : "", dn );
    } else {
	printf( "%smodifying entry \"%s\"\n", not ? "!" : "", dn );
    }

    if ( !not ) {
	if ( newentry ) {
	    i = ldap_add_ext_s( ld, dn, pmods, pctrls, NULL );
	} else {
	    i = ldap_modify_ext_s( ld, dn, pmods, pctrls, NULL );
	}
	if ( i != LDAP_SUCCESS ) {
		/* print error message about failed update including DN */
		fprintf( stderr, "%s: update failed: %s\n", prog, dn );
		ldap_perror( ld, newentry ? "ldap_add" : "ldap_modify" );
	} else if ( verbose ) {
	    printf( "modify complete\n" );
	}
    } else {
	i = LDAP_SUCCESS;
    }

    putchar( '\n' );

    return( i );
}


static int
dodelete(
	const char *dn,
    LDAPControl **pctrls )
{
    int	rc;

    printf( "%sdeleting entry \"%s\"\n", not ? "!" : "", dn );
    if ( !not ) {
	if (( rc = ldap_delete_ext_s( ld, dn, pctrls, NULL )) != LDAP_SUCCESS ) {
		fprintf( stderr, "%s: delete failed: %s\n", prog, dn );
		ldap_perror( ld, "ldap_delete" );
	} else if ( verbose ) {
	    printf( "delete complete" );
	}
    } else {
	rc = LDAP_SUCCESS;
    }

    putchar( '\n' );

    return( rc );
}


static int
dorename(
	const char *dn,
	const char *newrdn,
	const char* newsup,
	int deleteoldrdn,
    LDAPControl **pctrls )
{
    int	rc;


    printf( "%smodifying rdn of entry \"%s\"\n", not ? "!" : "", dn );
    if ( verbose ) {
	printf( "\tnew RDN: \"%s\" (%skeep existing values)\n",
		newrdn, deleteoldrdn ? "do not " : "" );
    }
    if ( !not ) {
	if (( rc = ldap_rename_s( ld, dn, newrdn, newsup, deleteoldrdn, pctrls, NULL ))
		!= LDAP_SUCCESS ) {
		fprintf( stderr, "%s: rename failed: %s\n", prog, dn );
		ldap_perror( ld, "ldap_modrdn" );
	} else {
	    printf( "modrdn completed\n" );
	}
    } else {
	rc = LDAP_SUCCESS;
    }

    putchar( '\n' );

    return( rc );
}


static char *
read_one_record( FILE *fp )
{
    char        *buf, line[ LDAPMOD_MAXLINE ];
    int		lcur, lmax;

    lcur = lmax = 0;
    buf = NULL;

    while ( fgets( line, sizeof(line), fp ) != NULL ) {
    	int len = strlen( line );

		if( len < 2 || ( len == 2 && *line == '\r' )) {
			if( buf == NULL ) {
				continue;
			} else {
				break;
			}
		}

		if ( lcur + len + 1 > lmax ) {
			lmax = LDAPMOD_MAXLINE
				* (( lcur + len + 1 ) / LDAPMOD_MAXLINE + 1 );

			if (( buf = (char *)ber_memrealloc( buf, lmax )) == NULL ) {
				perror( "realloc" );
				exit( EXIT_FAILURE );
			}
		}

		strcpy( buf + lcur, line );
		lcur += len;
    }

    return( buf );
}


