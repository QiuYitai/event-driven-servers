/*
   Copyright (C) 1999-2023 Marc Huber (Marc.Huber@web.de)
   All rights reserved.

   Redistribution and use in source and binary  forms,  with or without
   modification, are permitted provided  that  the following conditions
   are met:

   1. Redistributions of source code  must  retain  the above copyright
      notice, this list of conditions and the following disclaimer.

   2. Redistributions in binary form must reproduce the above copyright
      notice, this list of conditions  and  the following disclaimer in
      the  documentation  and/or  other  materials  provided  with  the
      distribution.

   3. The end-user documentation  included with the redistribution,  if
      any, must include the following acknowledgment:

          This product includes software developed by Marc Huber
	  (Marc.Huber@web.de).

      Alternately,  this  acknowledgment  may  appear  in  the software
      itself, if and wherever such third-party acknowledgments normally
      appear.

   THIS SOFTWARE IS  PROVIDED  ``AS IS''  AND  ANY EXPRESSED OR IMPLIED
   WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
   MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
   IN NO EVENT SHALL  ITS  AUTHOR  BE  LIABLE FOR ANY DIRECT, INDIRECT,
   INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
   BUT NOT LIMITED  TO,  PROCUREMENT OF  SUBSTITUTE  GOODS OR SERVICES;
   LOSS OF USE,  DATA,  OR PROFITS;  OR  BUSINESS INTERRUPTION) HOWEVER
   CAUSED AND ON ANY THEORY OF LIABILITY,  WHETHER IN CONTRACT,  STRICT
   LIABILITY,  OR TORT  (INCLUDING NEGLIGENCE OR OTHERWISE)  ARISING IN
   ANY WAY OUT OF THE  USE  OF  THIS  SOFTWARE,  EVEN IF ADVISED OF THE
   POSSIBILITY OF SUCH DAMAGE.
 */

/*
   Copyright (c) 1995-1998 by Cisco systems, Inc.

   Permission to use, copy, modify, and distribute this software for
   any purpose and without fee is hereby granted, provided that this
   copyright and permission notice appear on all copies of the
   software and supporting documentation, the name of Cisco Systems,
   Inc. not be used in advertising or publicity pertaining to
   distribution of the program without specific prior permission, and
   notice be given in supporting documentation that modification,
   copying and distribution is by permission of Cisco Systems, Inc.

   Cisco Systems, Inc. makes no representations about the suitability
   of this software for any purpose.  THIS SOFTWARE IS PROVIDED ``AS
   IS'' AND WITHOUT ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING,
   WITHOUT LIMITATION, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND
   FITNESS FOR A PARTICULAR PURPOSE.
*/

#include "headers.h"
#include "misc/version.h"
#include "misc/strops.h"
#include "misc/crc32.h"
#include "misc/mymd5.h"
#include <setjmp.h>
#include <pwd.h>
#include <grp.h>
#include <sys/utsname.h>

#include <glob.h>
#ifndef GLOB_NOMAGIC
#define GLOB_NOMAGIC 0
#endif
#ifndef GLOB_BRACE
#define GLOB_BRACE 0
#endif
#ifndef GLOB_NOESCAPE
#define GLOB_NOESCAPE 0
#endif

#ifdef WITH_CURL
#include <curl/curl.h>
#endif

#ifdef WITH_PCRE2
#include <pcre2.h>
#endif

#include <regex.h>

#ifdef WITH_SSL
#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509_vfy.h>
#endif

static const char rcsid[] __attribute__((used)) = "$Id$";

struct in6_cidr {
    struct in6_addr addr;
    int mask;
};

struct rewrite_expr {
    char *name;
#ifdef WITH_PCRE2
    pcre2_code *code;
    PCRE2_SPTR replacement;
#endif
    struct rewrite_expr *next;
};
typedef struct rewrite_expr tac_rewrite_expr;

typedef struct {
    TAC_NAME_ATTRIBUTES;
    tac_rewrite_expr *expr;
} tac_rewrite;

static void parse_host(struct sym *, tac_realm *, tac_host *);
static void parse_net(struct sym *, tac_realm *, tac_net *);
static void parse_user(struct sym *, tac_realm *);
static void parse_group(struct sym *, tac_realm *, tac_group *);
static void parse_ruleset(struct sym *, tac_realm *);
static void parse_profile(struct sym *, tac_realm *, tac_profile *);
static void parse_profile_attr(struct sym *, tac_profile *, tac_realm *);
static void parse_user_attr(struct sym *, tac_user *);
static void parse_tac_acl(struct sym *, tac_realm *);
static void parse_rewrite(struct sym *, tac_realm *);
static void parse_member(struct sym *, tac_groups **, mem_t *, tac_realm *);

static tac_group *lookup_group(char *, tac_realm *);	/* get id from tree */
static tac_group *tac_group_new(struct sym *, char *, tac_realm *);	/* add name to tree, return id (globally unique) */
static int tac_group_add(tac_group *, tac_groups *, mem_t *);	/* add id to groups struct */
static int tac_group_check(tac_group *, tac_groups *, tac_group *);	/* check for id in groups struct */
static int tac_group_regex_check(tac_session *, struct mavis_cond *, tac_groups *, tac_group *);
static int tac_tag_list_check(tac_session *, tac_host *, tac_user *);

static int tac_tag_add(mem_t *, tac_tag *, tac_tags *);
static int tac_tag_check(tac_session *, tac_tag *, tac_tags *);
static int tac_tag_regex_check(tac_session *, struct mavis_cond *, tac_tags *);
static tac_tag *tac_tag_parse(struct sym *);

struct tac_name {
    TAC_NAME_ATTRIBUTES;
};

int compare_name(const void *a, const void *b)
{
    if (((struct tac_name *) a)->name_len < ((struct tac_name *) b)->name_len)
	return -1;
    if (((struct tac_name *) a)->name_len > ((struct tac_name *) b)->name_len)
	return +1;
    return strcmp(((struct tac_name *) a)->name, ((struct tac_name *) b)->name);
}

static struct tac_acl *tac_acl_lookup(char *, tac_realm *);

struct tac_groups {
    u_int count;
    u_int allocated;		/* will be incremented on demand */
    tac_group **groups;		/* array will be reallocated on demand */
};

struct tac_group;
typedef struct tac_group tac_group;

struct tac_group {
    TAC_NAME_ATTRIBUTES;
    tac_group *parent;
    tac_groups *groups;
    u_int line;
    u_int visited:1;
};

struct tac_tags {
    u_int count;
    u_int allocated;		/* will be incremented on demand */
    tac_tag **tags;		/* array will be reallocated on demand */
};

struct tac_tag;
typedef struct tac_tag tac_tag;

struct tac_tag {
    TAC_NAME_ATTRIBUTES;
};

static rb_tree_t *tags_by_name = NULL;

#ifdef WITH_SSL
#ifndef OPENSSL_NO_PSK
static int psk_find_session_cb(SSL * ssl, const unsigned char *identity, size_t identity_len, SSL_SESSION ** sess);
#endif
static SSL_CTX *ssl_init(char *, char *, char *, char *);
#endif

static char *confdir_strdup(char *in)
{
#define S "$CONFDIR/"
    if (strncmp(in, S, sizeof(S) - 1) || !common_data.conffile)
	return strdup(in);
    size_t in_len = strlen(in);
    size_t cd_len = strlen(common_data.conffile);
    char *b = alloca(in_len + cd_len);
    strcpy(b, common_data.conffile);
    char *r = strrchr(b, '/');
    if (r)
	*r = 0;
    strcpy(b + strlen(b), in + sizeof(S) - 2);
    return strdup(b);
#undef S
}

void complete_realm(tac_realm *r)
{
    if (r->parent && !r->complete) {
	tac_realm *rp = r->parent;
	r->complete = 1;

#define RS(A,B) if(r->A == B) r->A = rp->A
	RS(chalresp, TRISTATE_DUNNO);
	RS(chpass, TRISTATE_DUNNO);
	RS(mavis_userdb, TRISTATE_DUNNO);
	RS(mavis_noauthcache, TRISTATE_DUNNO);
	RS(mavis_pap, TRISTATE_DUNNO);
	RS(mavis_login, TRISTATE_DUNNO);
	RS(mavis_pap_prefetch, TRISTATE_DUNNO);
	RS(mavis_login_prefetch, TRISTATE_DUNNO);
	RS(script_profile_parent_first, TRISTATE_DUNNO);
	RS(script_host_parent_first, TRISTATE_DUNNO);
	RS(script_realm_parent_first, TRISTATE_DUNNO);
	RS(mavis_user_acl, NULL);
	RS(enable_user_acl, NULL);
	RS(password_acl, NULL);
	RS(haproxy_autodetect, TRISTATE_DUNNO);
	RS(default_host->authfallback, TRISTATE_DUNNO);
	RS(allowed_protocol_radius, TRISTATE_DUNNO);
	RS(allowed_protocol_radius_tcp, TRISTATE_DUNNO);
	RS(allowed_protocol_radsec, TRISTATE_DUNNO);
	RS(allowed_protocol_tacacs, TRISTATE_DUNNO);
	RS(allowed_protocol_tacacss, TRISTATE_DUNNO);
#ifdef WITH_SSL
	RS(tls_sni_required, TRISTATE_DUNNO);
	RS(tls_autodetect, TRISTATE_DUNNO);
	RS(alpn_vec, NULL);
	if (!r->alpn_vec_len)
	    r->alpn_vec_len = rp->alpn_vec_len;
#endif
#if defined(WITH_SSL)
	RS(tls_accept_expired, TRISTATE_DUNNO);
#endif
#undef RS
#define RS(A) if(r->A < 0) r->A = rp->A;
	RS(caching_period);
	RS(dns_caching_period);
	RS(warning_period);
	RS(default_host->tcp_timeout);
	RS(default_host->session_timeout);
	RS(default_host->context_timeout);
	RS(default_host->dns_timeout);
	RS(default_host->max_rounds);
	RS(default_host->authen_max_attempts);
	RS(default_host->password_expiry_warning);
	RS(backend_failure_period);
#if defined(WITH_SSL)
	RS(tls_verify_depth);
#endif
#undef RS
#ifdef WITH_SSL
	if (r->tls_cert && r->tls_key) {
	    r->tls = ssl_init(r->tls_cert, r->tls_key, r->tls_pass, r->tls_ciphers);
	    if (r->tls) {
		if (r->tls_cafile && !SSL_CTX_load_verify_locations(r->tls, r->tls_cafile, NULL)) {
		    char buf[256];
		    const char *terr = ERR_error_string(ERR_get_error(), buf);
		    report(NULL, LOG_ERR, ~0,
			   "realm %s: SSL_CTX_load_verify_locations(\"%s\") failed%s%s", r->name, r->tls_cafile, terr ? ": " : "", terr ? terr : "");
		    exit(EX_CONFIG);
		}

		{
		    unsigned long flags = 0;
		    if (r->tls_accept_expired == TRISTATE_YES)
			flags |= X509_V_FLAG_NO_CHECK_TIME;
		    if (flags) {
			X509_VERIFY_PARAM *verify;
			verify = X509_VERIFY_PARAM_new();
			X509_VERIFY_PARAM_set_flags(verify, flags);
			SSL_CTX_set1_param(r->tls, verify);
			X509_VERIFY_PARAM_free(verify);
		    }
		    if (r->tls_verify_depth > -1)
			SSL_CTX_set_verify_depth(r->tls, r->tls_verify_depth);
		}
		SSL_CTX_set_verify(r->tls, SSL_VERIFY_PEER | SSL_VERIFY_CLIENT_ONCE | SSL_VERIFY_FAIL_IF_NO_PEER_CERT, NULL);
	    }
	}
#ifndef OPENSSL_NO_PSK
	if (r->use_tls_psk) {
	    if (!r->tls)
		r->tls = ssl_init(r->tls_cert, r->tls_key, r->tls_pass, r->tls_ciphers);
	    SSL_CTX_set_psk_find_session_callback(r->tls, psk_find_session_cb);
	}
#endif
#endif

#ifdef WITH_PCRE2
	if (!r->password_minimum_requirement)
	    r->password_minimum_requirement = rp->password_minimum_requirement;
#endif
	r->debug |= rp->debug;

	if (r->mavis_userdb != TRISTATE_YES)
	    r->mavis_pap_prefetch = TRISTATE_NO;
	if (r->caching_period < 11)
	    r->caching_period = 0;
	if (r->dns_caching_period < 10)
	    r->dns_caching_period = 10;

	if (!r->default_host->user_messages)
	    r->default_host->user_messages = rp->default_host->user_messages;
	else
	    for (enum user_message_enum um = 0; um < UM_MAX; um++)
		if (!r->default_host->user_messages[um])
		    r->default_host->user_messages[um] = rp->default_host->user_messages[um];
    }
    if (r->realms) {
	for (rb_node_t * rbn = RB_first(r->realms); rbn; rbn = RB_next(rbn))
	    complete_realm(RB_payload(rbn, tac_realm *));
    }
}

tac_realm *lookup_realm(char *name, tac_realm *r)
{
    if (!strcmp(name, r->name))
	return r;

    if (r->realms) {
	tac_realm *res;
	for (rb_node_t * rbn = RB_first(r->realms); rbn; rbn = RB_next(rbn))
	    if ((res = lookup_realm(name, RB_payload(rbn, tac_realm *))))
		return res;
    }
    return NULL;
}

void complete_profile(tac_profile *p)
{
    if (p && !p->complete) {
	p->complete = BISTATE_YES;
	if (p->parent) {
	    tac_profile *pp = p->parent;
	    complete_profile(pp);
	    if (p->enable) {
		if (pp->enable) {
		    for (int level = TAC_PLUS_PRIV_LVL_MIN; level < TAC_PLUS_PRIV_LVL_MAX + 1; level++)
			if (!p->enable[level])
			    p->enable[level] = pp->enable[level];
		}
	    } else
		p->enable = pp->enable;
#define PS(A,B) if(p->A == B) p->A = pp->A
	    PS(hushlogin, 0);
#undef PS
	    p->debug |= pp->debug;
	}
    }
}

radixtree_t *lookup_hosttree(tac_realm *r)
{
    while (r) {
	if (r->hosttree)
	    return r->hosttree;
	r = r->parent;
    }
    return NULL;
}

static void parse_inline(tac_realm *r, char *format, char *file, int line)
{
    struct sym sym = { 0 };
    sym.filename = file;
    sym.line = line;
    sym.in = sym.tin = format;
    sym.len = sym.tlen = strlen(sym.in);
    sym_init(&sym);
    parse_tac_acl(&sym, r);
}

void init_host(tac_host *host, tac_host *parent, tac_realm *r, int top)
{
    host->parent = parent;
    host->realm = r;
    // short-hand syntax may help not to forget some variables
    host->authen_max_attempts = top ? 1 : -1;
    host->context_timeout = top ? 3600 : -1;
    host->dns_timeout = top ? 1 : -1;
    host->max_rounds = top ? 40 : -1;
    host->session_timeout = top ? 240 : -1;
    host->tcp_timeout = top ? 600 : -1;
    if (top) {
	host->user_messages = calloc(UM_MAX, sizeof(char *));
	host->user_messages[UM_PASSWORD] = "Password: ";
	host->user_messages[UM_RESPONSE] = "Response: ";
	host->user_messages[UM_PASSWORD_OLD] = "Old password: ";
	host->user_messages[UM_PASSWORD_NEW] = "New password: ";
	host->user_messages[UM_PASSWORD_ABORT] = "Password change dialog aborted.";
	host->user_messages[UM_PASSWORD_AGAIN] = "Retype new password: ";
	host->user_messages[UM_PASSWORD_NOMATCH] = "Passwords do not match.";
	host->user_messages[UM_PASSWORD_MINREQ] = "Password doesn't meet minimum requirements.";
	host->user_messages[UM_PERMISSION_DENIED] = "Permission denied.";
	host->user_messages[UM_ENABLE_PASSWORD] = "Enable Password: ";
	host->user_messages[UM_PASSWORD_CHANGE_DIALOG] = "Entering password change dialog";
	host->user_messages[UM_PASSWORD_CHANGED] = "Password change succeeded.";
	host->user_messages[UM_BACKEND_FAILED] = "Authentication backend failure.";
	host->user_messages[UM_CHANGE_PASSWORD] = "Please change your password.";
	host->user_messages[UM_ACCOUNT_EXPIRES] = "This account will expire soon.";
	host->user_messages[UM_PASSWORD_EXPIRED] = "Pasword has expired.";
	host->user_messages[UM_PASSWORD_EXPIRES] = "Password will expire on %c.";
	host->user_messages[UM_PASSWORD_INCORRECT] = "Password incorrect.";
	host->user_messages[UM_RESPONSE_INCORRECT] = "Response incorrect.";
	host->user_messages[UM_USERNAME] = "Username: ";
	host->user_messages[UM_USER_ACCESS_VERIFICATION] = "User Access Verification";
	host->user_messages[UM_DENIED_BY_ACL] = "Denied by ACL";
	host->user_messages[UM_MAVIS_PARSE_ERROR] = "\n\
\n\
An error occured while parsing your user profile. Please ask your TACACS+\n\
administrator to have a look at the TACACS+ logs, providing the following\n\
information:\n\
\n\
        Device: ${device.address}\n\
        User:   ${user}\n\
        Date:   %Y-%m-%d %H:%M:%S %z\n\
\"";
    }
}

static tac_host *new_host(struct sym *sym, char *name, tac_host *parent, tac_realm *r, int top)
{
    tac_host *host = calloc(1, sizeof(tac_host));
    if (sym) {
	host->line = sym->line;
	host->name = strdup(sym->buf);
	sym_get(sym);
    } else
	host->name = name;
    host->name_len = strlen(host->name);
    init_host(host, parent, r, top);
    return host;
}

static tac_realm *new_realm(char *name, tac_realm *parent)
{
    tac_realm *r = calloc(1, sizeof(tac_realm));
    r->name = strdup(name);

    r->default_host = new_host(NULL, "default", NULL, r, parent ? 0 : 1);

    r->debug = parent ? 0 : common_data.debug;
#if defined(WITH_SSL)
    r->tls_verify_depth = -1;
    //r->tls_ciphers = "TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256";
#endif

    if (parent) {
	r->parent = parent;
	r->caching_period = -1;
	r->dns_caching_period = -1;
	r->warning_period = -1;
	r->backend_failure_period = -1;
    } else {
	r->caching_period = 120;
	r->dns_caching_period = 1800;
	r->warning_period = 14 * 86400;
	r->backend_failure_period = 60;
	r->allowed_protocol_radius = TRISTATE_YES;
	r->allowed_protocol_radius_tcp = TRISTATE_NO;
	r->allowed_protocol_radsec = TRISTATE_YES;
	r->allowed_protocol_tacacs = TRISTATE_YES;;
	r->allowed_protocol_tacacss = TRISTATE_YES;
	config.default_realm = r;
	r->complete = 1;
	parse_inline(r, "acl __internal__username_acl__ { if (user =~ \"[]<>/()|=[*\\\"':$]+\") deny permit }\n", __FILE__, __LINE__);
	r->mavis_user_acl = tac_acl_lookup("__internal__username_acl__", r);
	parse_inline(r, "acl __internal__enable_user__ { if (user =~ \"^\\\\$enab..?\\\\$$\") permit deny }", __FILE__, __LINE__);
	r->enable_user_acl = tac_acl_lookup("__internal__enable_user__", r);
    }

    return r;
}

void init_mcx(tac_realm *r)
{
    if (r->mcx)
	mavis_init(r->mcx, MAVIS_API_VERSION);
    if (r->realms)
	for (rb_node_t * rbn = RB_first(r->realms); rbn; rbn = RB_next(rbn))
	    init_mcx(RB_payload(rbn, tac_realm *));
}

void drop_mcx(tac_realm *r)
{
    if (r->mcx)
	mavis_drop(r->mcx);
    if (r->realms)
	for (rb_node_t * rbn = RB_first(r->realms); rbn; rbn = RB_next(rbn))
	    drop_mcx(RB_payload(rbn, tac_realm *));
}

void expire_dynamic_users(tac_realm *r)
{
    if (r->usertable) {
	for (rb_node_t * rbnext, *rbn = RB_first(r->usertable); rbn; rbn = rbnext) {
	    time_t v = RB_payload(rbn, tac_user *)->dynamic;
	    rbnext = RB_next(rbn);

	    if (v && v < io_now.tv_sec)
		RB_delete(r->usertable, rbn);
	}
    }
    if (r->realms)
	for (rb_node_t * rbn = RB_first(r->realms); rbn; rbn = RB_next(rbn))
	    expire_dynamic_users(RB_payload(rbn, tac_realm *));
}

tac_user *lookup_user(tac_session *session)
{
    session->user = NULL;
    if (!session->username_len)
	return NULL;
    tac_user user = {.name = session->username,.name_len = session->username_len };
    tac_realm *r = session->ctx->realm;
    while (r && !session->user) {
	if (r->usertable)
	    session->user = RB_lookup(r->usertable, &user);
	if (!session->user && r->aliastable) {
	    tac_alias *a = RB_lookup(r->aliastable, &user);
	    if (a)
		session->user = a->user;
	}
	if (session->user && session->user->dynamic && (session->user->dynamic < io_now.tv_sec)) {
	    RB_search_and_delete(r->usertable, session->user);
	    session->user = NULL;
	}
	r = r->parent;
    }
    return session->user;
}

static tac_profile *lookup_profile(char *name, tac_realm *r)
{
    tac_profile profile = {.name = name,.name_len = strlen(name) };
    while (r) {
	if (r->profiletable) {
	    tac_profile *res;
	    if ((res = RB_lookup(r->profiletable, &profile)))
		return res;
	}
	r = r->parent;
    }
    return NULL;
}

static tac_rewrite *lookup_rewrite(char *name, tac_realm *r)
{
    tac_rewrite rewrite = {.name = name,.name_len = strlen(name) };
    while (r) {
	if (r->rewrite) {
	    tac_rewrite *res;
	    if ((res = RB_lookup(r->rewrite, &rewrite)))
		return res;
	}
	r = r->parent;
    }
    return NULL;
}

tac_host *lookup_host(char *name, tac_realm *r)
{
    tac_host host = {.name = name,.name_len = strlen(name) };
    while (r) {
	if (r->hosttable) {
	    tac_host *res;
	    if ((res = RB_lookup(r->hosttable, &host)))
		return res;
	}
	r = r->parent;
    }
    return NULL;
}

static tac_net *lookup_net(char *name, tac_realm *r)
{
    tac_net net = {.name = name,.name_len = strlen(name) };
    while (r) {
	if (r->nettable) {
	    tac_net *res;
	    if ((res = RB_lookup(r->nettable, &net)))
		return res;
	}
	r = r->parent;
    }
    return NULL;
}

static struct mavis_timespec *lookup_timespec(char *name, tac_realm *r)
{
    while (r) {
	if (r->timespectable) {
	    struct mavis_timespec *res;
	    if ((res = find_timespec(r->timespectable, name)))
		return res;
	}
	r = r->parent;
    }
    return NULL;
}

static struct sym *globerror_sym = NULL;

static int globerror(const char *epath, int eerrno)
{
    report_cfg_error(LOG_ERR, ~0, "%s:%u: glob(%s): %s", globerror_sym->filename, globerror_sym->line, epath, strerror(eerrno));
    return 0;
}

static time_t parse_date(struct sym *sym, time_t offset);

static void parse_key(struct sym *sym, tac_host *host)
{
    struct tac_key **tk;
    int keylen;
    time_t warn = 0;

    if (sym->code == S_key)
	tk = &host->key;
    else			// S_radius_key
	tk = &host->radius_key;
    sym_get(sym);
    if (sym->code == S_warn) {
	sym_get(sym);
	if (sym->code == S_equal)
	    warn = io_now.tv_sec - 1;
	else
	    warn = parse_date(sym, 0);
    }

    while (*tk)
	tk = &(*tk)->next;

    parse(sym, S_equal);
    keylen = strlen(sym->buf);

    *tk = mem_alloc(host->mem, sizeof(struct tac_key) + keylen);
    (*tk)->warn = warn;
    (*tk)->len = keylen;
    (*tk)->line = sym->line;
    strncpy((*tk)->key, sym->buf, keylen);

    sym_get(sym);
}

struct dns_forward_mapping {
    TAC_NAME_ATTRIBUTES;
    struct dns_forward_mapping *next;
    struct in6_addr a;
};

static void free_dns_tree_a(void *payload)
{
    if (((struct dns_forward_mapping *) payload)->name)
	free(((struct dns_forward_mapping *) payload)->name);
    free(payload);
}

static void dns_add_a(rb_tree_t **t, struct in6_addr *a, char *name)
{
    struct dns_forward_mapping *ds, *dn = calloc(1, sizeof(struct dns_forward_mapping));
    struct dns_forward_mapping **dsp = &ds;

    if (*t) {
	dn->name = name;
	ds = (struct dns_forward_mapping *) RB_lookup(*t, dn);
	if (ds) {
	    while (*dsp) {
		if (!memcmp(&(*dsp)->a, a, sizeof(struct in6_addr))) {
		    // entry exists
		    free(dn);
		    return;
		}
		dsp = &(*dsp)->next;
	    }
	    *dsp = dn;
	    dn->name = NULL;
	    dn->a = *a;
	    return;
	}
    } else
	*t = RB_tree_new(compare_name, free_dns_tree_a);

    dn->a = *a;
    dn->name = strdup(name);
    dn->name_len = strlen(name);
    RB_insert(*t, dn);
}

static struct dns_forward_mapping *dns_lookup_a(tac_realm *r, char *name, int recurse)
{
    while (r) {
	if (r->dns_tree_a) {
	    struct dns_forward_mapping dn = {.name = name };
	    struct dns_forward_mapping *res = (struct dns_forward_mapping *) RB_lookup(r->dns_tree_a, &dn);
	    if (res)
		return res;
	}
	if (!recurse)
	    return NULL;
	r = r->parent;
    }
    return NULL;
}

static void parse_etc_hosts(char *url, tac_realm *r)
{
    struct sym sym = {.filename = url,.line = 1,.env_valid = 1 };
    if (setjmp(sym.env))
	tac_exit(EX_CONFIG);

    char *buf;
    int bufsize;

    char *filename = confdir_strdup(url);
    if (cfg_open_and_read(filename, &buf, &bufsize)) {
	free(filename);
	report_cfg_error(LOG_ERR, ~0, "Couldn't open %s: %s", url, strerror(errno));
	return;
    }
    free(filename);

    sym.tlen = sym.len = bufsize;
    sym.tin = sym.in = buf;

    sym_init(&sym);

    while (sym.code != S_eof) {
	struct in6_addr a;
	int cm;
	u_int line = sym.line;

	// Line:
	// ip hostname hostname1 hostname2 <EOL>

	// add first hostname to revmap:
	if (v6_ptoh(&a, &cm, sym.buf)) {
	    // IP invalid, skip line
	    while (line == sym.line && sym.code != S_eof)
		sym_get(&sym);
	} else {
	    sym_get(&sym);
	    if (sym.line != line)
		continue;
	}
	if (sym.code != S_eof) {
	    char *firstname = strdup(sym.buf);
	    add_revmap(r, &a, firstname, cm, 0);

	    // add forward mapping for all hostnames:
	    do {
		dns_add_a(&r->dns_tree_a, &a, firstname);
		sym_get(&sym);
	    }
	    while (sym.code != S_eof && sym.line == line);
	    free(firstname);
	}
    }

    cfg_close(url, buf, bufsize);
}

#define parse_tristate(A) (parse_bool(A) ? TRISTATE_YES : TRISTATE_NO);
#define parse_bistate(A) (parse_bool(A) ? BISTATE_YES : BISTATE_NO);

static void top_only(struct sym *sym, tac_realm *r)
{
    if (r != config.default_realm)
	parse_error(sym, "Directive not available at realm level.");
}

void parse_decls_real(struct sym *, tac_realm *);

static int loopcheck_group(tac_group *g)
{
    int res = 0;
    if (g->visited)
	return -1;
    g->visited = 1;
    if (g->groups) {
	for (u_int i = 0; i < g->groups->count && !res; i++)
	    res = loopcheck_group(g->groups->groups[i]);
    }
    if (!res && g->parent)
	res = loopcheck_group(g->parent);
    g->visited = 0;
    return res;
}

static int loopcheck_realm(tac_realm *r)
{
    int res = 0;
    if (r->visited)
	return -1;
    r->visited = 1;
    if (r->parent)
	res = loopcheck_realm(r->parent);
    r->visited = 0;
    return res;
}

static int loopcheck_net(tac_net *n)
{
    int res = 0;
    if (n->visited)
	return -1;
    n->visited = 1;
    if (n->parent)
	res = loopcheck_net(n->parent);
    n->visited = 0;
    return res;
}

static int loopcheck_host(tac_host *h)
{
    int res = 0;
    if (h->visited)
	return -1;
    h->visited = 1;
    if (h->parent)
	res = loopcheck_host(h->parent);
    h->visited = 0;
    return res;
}

static int loopcheck_profile(tac_profile *p)
{
    int res = 0;
    if (p->visited)
	return -1;
    p->visited = 1;
    if (p->parent)
	res = loopcheck_profile(p->parent);
    p->visited = 0;
    return res;
}

static tac_realm *parse_realm(struct sym *sym, char *name, tac_realm *parent, tac_realm *nrealm, int empty)
{
    if (!nrealm) {
	nrealm = new_realm(sym->buf, parent);
	nrealm->line = sym->line;
	nrealm->name = name;
	nrealm->name_len = strlen(name);
    }

    if (!empty)
	parse_decls_real(sym, nrealm);

    for (rb_node_t * rbn = RB_first(nrealm->profiletable); rbn; rbn = RB_next(rbn))
	complete_profile(RB_payload(rbn, tac_profile *));

    return nrealm;
}

static char hexbyte(char *);

#if defined(WITH_SSL) && !defined(OPENSSL_NO_PSK)
static void parse_tls_psk_key(struct sym *sym, tac_host *host)
{
    char k[2];
    char *t = sym->buf;
    size_t l = strlen(sym->buf);
    if (l & 1)
	parse_error(sym, "Illegal hex sequence (odd number of characters)");
    l >>= 1;
    host->tls_psk_key = mem_alloc(host->mem, l);
    host->tls_psk_key_len = l;
    for (size_t i = 0; i < l; i++) {
	k[0] = toupper(*t++);
	k[1] = toupper(*t++);
	host->tls_psk_key[i] = hexbyte(k);
    }
}
#endif

static void parse_host_attr(struct sym *, tac_realm *, tac_host *);

#ifdef WITH_DNS
static void parse_host_dns(struct sym *sym, tac_host *host)
{
    switch (sym->code) {
    case S_timeout:
	sym_get(sym);
	parse(sym, S_equal);
	host->dns_timeout = parse_seconds(sym);
	return;
    case S_reverselookup:
	sym_get(sym);
	switch (sym->code) {
	case S_equal:
	    sym_get(sym);
	    host->lookup_revmap_nac = host->lookup_revmap_nas = parse_tristate(sym);
	    break;
	case S_nac:
	case S_client:
	    sym_get(sym);
	    parse(sym, S_equal);
	    host->lookup_revmap_nac = parse_tristate(sym);
	    break;
	case S_nas:
	case S_device:
	    sym_get(sym);
	    parse(sym, S_equal);
	    host->lookup_revmap_nas = parse_tristate(sym);
	    break;
	default:
	    parse_error_expect(sym, S_equal, S_client, S_nac, S_device, S_nas, S_unknown);
	}
	if ((host->lookup_revmap_nas == TRISTATE_YES || host->lookup_revmap_nac == TRISTATE_YES)
	    && !host->realm->idc)
	    host->realm->idc = io_dns_init(common_data.io);
	return;
    default:
	;
    }
}
#endif

void parse_host_pap_password(struct sym *sym, tac_host *host)
{
    sym_get(sym);
    if (sym->code == S_mapping)
	sym_get(sym);
    parse(sym, S_equal);
    switch (sym->code) {
    case S_pap:
	host->map_pap_to_login = TRISTATE_NO;
	break;
    case S_login:
	host->map_pap_to_login = TRISTATE_YES;
	break;
    default:
	parse_error_expect(sym, S_login, S_pap, S_unknown);
    }
    sym_get(sym);
}

static time_t to_seconds(struct sym *sym)
{
    time_t n = 0;
    for (char *b = sym->buf; *b; b++)
	switch (tolower((int) *b)) {
	case 's':
	    break;
	case 'm':
	    n *= 60;
	    break;
	case 'h':
	    n *= 60 * 60;
	    break;
	case 'd':
	    n *= 60 * 60 * 24;
	    break;
	case 'w':
	    n *= 60 * 60 * 24 * 7;
	    break;
	case '0':
	case '1':
	case '2':
	case '3':
	case '4':
	case '5':
	case '6':
	case '7':
	case '8':
	case '9':
	    n *= 10;
	    n += *b - '0';
	    break;
	default:
	    parse_error(sym, "Expected a number followed by a valid size unit (s, m, h, d, w)");
	}
    sym_get(sym);
    return n;
}

static int parse_script_order(struct sym *sym)
{
    sym_get(sym);
    parse(sym, S_equal);
    switch (sym->code) {
    case S_top_down:
	sym_get(sym);
	return TRISTATE_YES;
    case S_bottom_up:
	sym_get(sym);
	return TRISTATE_NO;
    default:
	parse_error_expect(sym, S_bottom_up, S_top_down, S_unknown);
    }
    return 0;
}

static void parse_enable(struct sym *, mem_t *, struct pwdat **);

#ifdef WITH_SSL
static u_char *str2protocollist(char *in, size_t *outlen)
{
    *outlen = strlen(in) + 1;
    u_char *out = calloc(1, *outlen);
    u_char *outp = out;

    while (*in) {
	char *inp = in;
	while (*inp && *inp != ',')
	    inp++;
	if (inp - in > 255) {
	    free(out);
	    *outlen = 0;
	    return NULL;
	}
	*outp++ = (u_char) (inp - in);
	while (in != inp)
	    *outp++ = *in++;
	if (*in)
	    in++;
    }
    return out;
}
#endif

#ifdef WITH_SSL
struct sni_list {
    struct sni_list *next;
    size_t name_len;
    char name[1];
};

tac_realm *lookup_sni(const char *name, size_t name_len, tac_realm *r)
{
    struct sni_list *l = r->sni_list;
    while (l) {
	if (name_len == l->name_len && !strcmp(name, l->name))
	    return r;
	l = l->next;
    }

    if (r->realms) {
	tac_realm *res;
	for (rb_node_t * rbn = RB_first(r->realms); rbn; rbn = RB_next(rbn))
	    if ((res = lookup_sni(name, name_len, RB_payload(rbn, tac_realm *))))
		return res;
    }
    return NULL;
}

static void add_sni(struct sym *sym, tac_realm *r)
{
    size_t len = strlen(sym->buf);
    tac_realm *q = lookup_sni(sym->buf, len, r);
    if (q)
	parse_error(sym, "SNI %s already associated to realm %s", sym->buf, q->name);
    struct sni_list *l = calloc(1, sizeof(struct sni_list) + len);
    l->next = r->sni_list;
    memcpy(l->name, sym->buf, len);
    l->name_len = len;
    r->sni_list = l;
    sym_get(sym);
}
#endif

struct rad_dict_val {
    TAC_NAME_ATTRIBUTES;
    struct rad_dict_val *next;
    int line;
    int id;
};

struct rad_dict_attr {
    TAC_NAME_ATTRIBUTES;
    struct rad_dict_attr *next;
    int line;
    int id;
    enum token type;
    struct rad_dict *dict;	// back-reference to vendor
    struct rad_dict_val *val;
};

struct rad_dict {
    TAC_NAME_ATTRIBUTES;
    struct rad_dict *next;
    int line;
    int id;
    struct rad_dict_attr *attr;
};

static struct rad_dict *rad_dict_new(struct sym *sym, char *name, int id)
{
    struct rad_dict **dict = &config.rad_dict;
    while (*dict)
	dict = &(*dict)->next;
    *dict = calloc(1, sizeof(struct rad_dict));
    (*dict)->name = strdup(name);
    (*dict)->name_len = strlen(name);
    (*dict)->line = sym->line;
    (*dict)->id = id;
    return *dict;
}

struct rad_dict *rad_dict_lookup_by_id(int vendorid)
{
    for (struct rad_dict * dict = config.rad_dict; dict; dict = dict->next)
	if (dict->id == vendorid)
	    return dict;
    return NULL;
}

static struct rad_dict *rad_dict_lookup_by_name(char *vendorname)
{
    size_t vendorname_len = strlen(vendorname);
    for (struct rad_dict * dict = config.rad_dict; dict; dict = dict->next)
	if (dict->name_len == vendorname_len && !strcmp(dict->name, vendorname))
	    return dict;
    return NULL;
}

struct rad_dict_attr *rad_dict_attr_lookup_by_id(struct rad_dict *dict, int id)
{
    for (struct rad_dict_attr * attr = dict->attr; attr; attr = attr->next)
	if (attr->id == id)
	    return attr;
    return NULL;
}

static struct rad_dict_attr *rad_dict_attr_lookup_by_name(struct rad_dict *dict, char *name)
{
    size_t name_len = strlen(name);
    for (struct rad_dict_attr * attr = dict->attr; attr; attr = attr->next)
	if (attr->name_len == name_len && !strcmp(attr->name, name))
	    return attr;
    return NULL;
}

struct rad_dict_val *rad_dict_val_lookup_by_id(struct rad_dict_attr *attr, int id)
{
    for (struct rad_dict_val * val = attr->val; val; val = val->next)
	if (val->id == id)
	    return val;
    return NULL;
}

static struct rad_dict_val *rad_dict_val_lookup_by_name(struct rad_dict_attr *attr, char *name)
{
    size_t name_len = strlen(name);
    for (struct rad_dict_val * val = attr->val; val; val = val->next)
	if (val->name_len == name_len && !strcmp(val->name, name))
	    return val;
    return NULL;
}

static struct rad_dict_attr *rad_dict_attr_add(struct sym *sym, struct rad_dict *dict, char *name, int id, enum token type)
{
    struct rad_dict_attr **attr = &(dict->attr);
    while (*attr)
	attr = &(*attr)->next;
    *attr = calloc(1, sizeof(struct rad_dict_attr));
    (*attr)->name = strdup(name);
    (*attr)->name_len = strlen(name);
    (*attr)->line = sym->line;
    (*attr)->id = id;
    (*attr)->dict = dict;
    (*attr)->type = type;
    return *attr;
}

static void rad_dict_attr_add_val(struct sym *sym, struct rad_dict_attr *attr, char *name, int id)
{
    struct rad_dict_val **val = &(attr->val);
    while (*val)
	val = &(*val)->next;
    *val = calloc(1, sizeof(struct rad_dict_val));
    (*val)->name = strdup(name);
    (*val)->name_len = strlen(name);
    (*val)->line = sym->line;
    (*val)->id = id;
}

static struct rad_dict_attr *rad_dict_attr_lookup(struct sym *sym)
{
    size_t buf_len = strlen(sym->buf);
    char *vid_str = alloca(buf_len + 1);
    memcpy(vid_str, sym->buf, buf_len + 1);
    char *id_str = strchr(vid_str, ':');

    if (id_str) {
	*id_str = 0;
	id_str++;
    } else {
	id_str = sym->buf;
	vid_str = "";
    }

    struct rad_dict *dict = rad_dict_lookup_by_name(vid_str);
    if (!dict)
	parse_error(sym, "RADIUS dictionary '%s', not defined", vid_str);

    struct rad_dict_attr *attr = rad_dict_attr_lookup_by_name(dict, id_str);
    if (!attr)
	parse_error(sym, "RADIUS attribute '%s', not defined", sym->buf);

    return attr;
}

static void rad_attr_val_dump_hex(u_char *data, size_t data_len, char **buf, size_t *buf_len)
{
    char hex[16] = "0123456789abcdef";
    for (size_t i = 0; i < data_len && *buf_len > 10; i++) {
	if (i) {
	    *(*buf)++ = ' ';
	    (*buf_len)--;
	}

	*(*buf)++ = hex[data[i] >> 4];
	*(*buf)++ = hex[data[i] & 15];
	*buf_len -= 2;
    }
}

static void rad_attr_val_dump_helper(u_char *data, size_t data_len, char **buf, size_t *buf_len, struct rad_dict *dict)
{
    // dump exactly one av pair, type is attr->type, prefixed with attr->dict->name (vendor name)

    if (dict->id > -1 && *buf_len > dict->name_len + 2) {
	memcpy(*buf, dict->name, dict->name_len);
	*buf += dict->name_len;
	*buf_len -= dict->name_len;
	*(*buf)++ = ':';
	*buf_len -= 1;
    }
    struct rad_dict_attr *attr = rad_dict_attr_lookup_by_id(dict, *data);

    if (attr) {
	if (*buf_len > attr->name_len + 2) {
	    memcpy(*buf, attr->name, attr->name_len);
	    *buf += attr->name_len;
	    *buf_len -= attr->name_len;
	    *(*buf)++ = '=';
	    *buf_len -= 1;
	}
	switch (attr->type) {
	case S_string_keyword:{
		if (*buf_len > (size_t) (data[1] - 1)) {
		    if (attr->dict->id == -1 && attr->id == RADIUS_A_USER_PASSWORD) {
			*(*buf)++ = '*';
			*(*buf)++ = '*';
			*(*buf)++ = '*';
			*buf_len -= 3;
		    } else {
			memcpy(*buf, data + 2, data[1] - 2);
			*buf += data[1] - 2;
			*buf_len -= data[1] - 2;
		    }
		}
		return;
	    }
	case S_integer:{
		if (data[1] != 6)
		    return;
		int i = (data[2] << 24) | (data[3] << 16) | (data[4] << 8) | data[5];
		struct rad_dict_val *val = rad_dict_val_lookup_by_id(attr, i);
		if (val && (*buf_len > val->name_len)) {
		    memcpy(*buf, val->name, val->name_len);
		    *buf += val->name_len;
		    *buf_len -= val->name_len;
		} else {
		    int len = snprintf(*buf, *buf_len, "%d", i);
		    if (len > 0) {
			*buf += len;
			*buf_len -= len;
		    }
		}
		return;
	    }
	case S_octets:
	    rad_attr_val_dump_hex(data, data_len - 2, buf, buf_len);
	    return;
	case S_ipaddr:{
		if (data[1] != 6)
		    return;
		sockaddr_union from = { 0 };
		from.sin.sin_family = AF_INET;
		memcpy(&from.sin.sin_addr, data + 2, 4);
		if (su_ntop(&from, *buf, *buf_len)) {
		    int len = strlen(*buf);
		    *buf += len;
		    *buf_len -= len;
		}
		return;
	    }
	case S_ipv6addr:{
		if (data[1] != 18)
		    return;
		sockaddr_union from = { 0 };
		from.sin.sin_family = AF_INET6;
		memcpy(&from.sin6.sin6_addr, data + 2, 16);
		if (su_ntop(&from, *buf, *buf_len)) {
		    int len = strlen(*buf);
		    *buf += len;
		    *buf_len -= len;
		}
		return;
	    }
	default:
	    ;
	}
    } else {
	rad_attr_val_dump_hex(data, data[1], buf, buf_len);
    }
}

void rad_attr_val_dump(mem_t *mem, u_char *data, size_t data_len, char **buf, size_t *buf_len, struct rad_dict *dict, char *separator, size_t separator_len)
{
    char *buf_start = NULL;
    if (!dict)
	dict = rad_dict_lookup_by_id(-1);
    if (!*buf) {
	*buf_len = 4096;
	*buf = mem_alloc(mem, *buf_len);
	buf_start = *buf;
    }

    u_char *data_end = data + data_len;

    int add_separator = 0;
    while (data < data_end) {
	u_char *d_start = data;
	size_t d_len = data[1];
	struct rad_dict *cur_dict = dict;
	if (dict->id == -1 && data[0] == RADIUS_A_VENDOR_SPECIFIC) {
	    int vendorid = (data[2] << 24) | (data[3] << 16) | (data[4] << 8) | (data[5] << 0);
	    cur_dict = rad_dict_lookup_by_id(vendorid);
	    if (cur_dict) {
		d_start = data + 6;
		d_len = data[1] - 6;
	    }
	}

	if (dict->id != -1 || (*d_start != RADIUS_A_MESSAGE_AUTHENTICATOR && *d_start != RADIUS_A_USER_PASSWORD)) {
	    if (add_separator) {
		if (*buf_len > separator_len) {
		    memcpy(*buf, separator, separator_len);
		    *buf += separator_len;
		    *buf_len -= separator_len;
		}
	    }
	    if (cur_dict)
		rad_attr_val_dump_helper(d_start, d_len, buf, buf_len, cur_dict);
	    else
		rad_attr_val_dump_hex(d_start, d_len, buf, buf_len);
	    add_separator = 1;
	}
	data += data[1];

    }
    *(*buf) = 0;
    if (buf_start) {
	*buf_len = (*buf - buf_start);
	*buf = buf_start;
	// assert (*buf_len == strlen(buf_start));
    }
}

void rad_dict_get_val(int dict_id, int attr_id, int val_id, char **s, size_t *s_len)
{
    struct rad_dict *dict = rad_dict_lookup_by_id(dict_id);
    if (dict) {
	struct rad_dict_attr *attr = rad_dict_attr_lookup_by_id(dict, attr_id);
	if (attr) {
	    for (struct rad_dict_val * val = rad_dict_val_lookup_by_id(attr, attr_id); val; val = val->next)
		if (val->id == val_id) {
		    *s = val->name;
		    *s_len = val->name_len;
		    return;
		}
	}
    }
}

static void parse_radius_dictionary(struct sym *sym)
{
    struct rad_dict *dict = NULL;
    sym_get(sym);
    if (sym->code == S_openbra) {
	dict = rad_dict_lookup_by_id(-1);
	if (!dict)
	    dict = rad_dict_new(sym, "", -1);
    } else {
	char *vendor = NULL;
	int vendorid = -1;
	dict = rad_dict_lookup_by_name(sym->buf);
	if (!dict)
	    vendor = strdup(sym->buf);
	sym_get(sym);
	vendorid = parse_int(sym);
	if (dict && dict->id != vendorid)
	    parse_error(sym, "RADIUS dictionary '%s', already defined at line %d, with vendor id %d", sym->buf, dict->line, dict->id);
	if (vendorid < 1)
	    parse_error(sym, "Expected a valid vendor number but got '%s'", sym->buf);
	struct rad_dict *dict_by_id = rad_dict_lookup_by_id(vendorid);
	if (dict && dict != dict_by_id)
	    parse_error(sym, "RADIUS dictionary id %d is already defined at line %d, with vendor name %s", sym->buf, dict->id, dict->line, dict->name);
	if (!dict)
	    dict = rad_dict_new(sym, vendor, vendorid);
	free(vendor);
    }
    parse(sym, S_openbra);
    while (sym->code == S_attr) {
	sym_get(sym);
	char *name = strdup(sym->buf);
	sym_get(sym);
	int id = parse_int(sym);
	if (!id || (id & ~0xff))
	    parse_error(sym, "Expected a number from 1 to 255 but got '%s'", sym->buf);
	enum token type = sym->code;
	switch (type) {
	case S_string_keyword:
	case S_octets:
	case S_ipaddr:
	case S_ipv6addr:
	case S_integer:
	case S_vsa:
	    break;
	default:
	    parse_error_expect(sym, S_string_keyword, S_octets, S_ipaddr, S_ipv6addr, S_integer, S_vsa, S_unknown);
	}
	sym_get(sym);
	struct rad_dict_attr *attr = rad_dict_attr_add(sym, dict, name, id, type);
	free(name);
	if (type == S_integer && sym->code == S_openbra) {
	    sym_get(sym);
	    while (sym->code != S_closebra && sym->code != S_eof) {
		name = strdup(sym->buf);
		sym_get(sym);
		id = parse_int(sym);
		rad_dict_attr_add_val(sym, attr, name, id);
		free(name);
	    }
	    parse(sym, S_closebra);
	}
    }
    parse(sym, S_closebra);
}

static int rad_get_helper(tac_session *session, enum token type, void *val, size_t *val_len, u_char *data, size_t data_len)
{
    if (val)
	switch (type) {
	case S_string_keyword:{
		char **s = (char **) val;
		*s = mem_strndup(session->mem, data, data_len);
		if (val_len)
		    *val_len = data_len;
		return 0;
	    }
	case S_ipaddr:
	    if (data_len != 4)
		return -1;
	    memcpy(val, data, 4);
	    if (val_len)
		*val_len = data_len;
	    return 0;
	case S_ipv6addr:
	    if (data_len != 16)
		return -1;
	    memcpy(val, data, 16);
	    if (val_len)
		*val_len = data_len;
	    return 0;
	case S_integer:{
		if (data_len != 4)
		    return -1;
		int32_t i, *p = (int32_t *) val;
		memcpy(&i, data, 4);
		*p = ntohl(i);
		if (val_len)
		    *val_len = data_len;
		return 0;
	    }
	case S_octets:{
		u_char **s = (u_char **) val;
		*s = mem_copy(session->mem, data, data_len);
		if (val_len)
		    *val_len = data_len;
		return 0;
	    }
	default:
	    ;
	}
    return -1;
}

int rad_get_password(tac_session *session, char **val, size_t *val_len)
{
    u_char *p = RADIUS_DATA(session->radius_data->pak_in);
    size_t len = RADIUS_DATA_LEN(session->radius_data->pak_in);
    u_char *e = p + len;
    while (p < e) {
	if (p[0] == RADIUS_A_USER_PASSWORD) {
	    u_char digest[16];
	    char *pass = mem_alloc(session->mem, p[1] - 1);
	    for (int i = 0; i <= p[1]; i++) {
		if (!(i & 0xf)) {
		    struct iovec iov[2] = {
			{.iov_base = session->ctx->key->key,.iov_len = session->ctx->key->len },
			{.iov_base = i ? (p + i + 2 - 16) : session->radius_data->pak_in->authenticator,.iov_len = 16 }
		    };
		    md5v(digest, 16, iov, 2);
		}
		pass[i] = digest[i % 16] ^ p[i + 2];
	    }
	    *val = pass;
	    if (val_len)
		*val_len = strlen(pass);
	    return 0;
	}
	p += p[1];
    }
    return -1;
}

int rad_get(tac_session *session, int vendorid, int id, enum token type, void *val, size_t *val_len)
{
    struct rad_dict *dict = rad_dict_lookup_by_id(vendorid);
    if (dict && session->radius_data) {
	u_char *p = RADIUS_DATA(session->radius_data->pak_in);
	size_t len = RADIUS_DATA_LEN(session->radius_data->pak_in);
	u_char *e = p + len;
	while (p < e) {
	    if (vendorid == -1 && p[0] == id)
		return rad_get_helper(session, type, val, val_len, p + 2, p[1] - 2);
	    if (vendorid > -1 && p[0] == RADIUS_A_VENDOR_SPECIFIC && p[2] == ((id >> 24) & 0xff)
		&& p[3] == ((id >> 16) & 0xff)
		&& p[4] == ((id >> 8) & 0xff)
		&& p[5] == ((id >> 0) & 0xff)) {
		u_char *ve = p + p[1];
		u_char *vp = p + 6;
		while (vp < ve && vp[1] > 1) {
		    if (vp[0] == id)
			return rad_get_helper(session, type, val, val_len, vp + 2, vp[1] - 2);
		    vp += vp[1];
		}
	    }
	    p += p[1];
	}
    }
    return -1;
}

void parse_decls_real(struct sym *sym, tac_realm *r)
{
    /* Top level of parser */
    while (sym->code != S_closebra)
	switch (sym->code) {
	case S_password:
	    sym_get(sym);
	    switch (sym->code) {
	    case S_acl:
		sym_get(sym);
		parse(sym, S_equal);
		r->password_acl = tac_acl_lookup(sym->buf, r);
		if (!r->password_acl)
		    parse_error(sym, "ACL '%s' not found.", sym->buf);
		sym_get(sym);
		continue;
	    case S_maxattempts:
		sym_get(sym);
		parse(sym, S_equal);
		r->default_host->authen_max_attempts = parse_int(sym);
		return;
	    case S_expiry:
		sym_get(sym);
		parse(sym, S_warning);
		parse(sym, S_equal);
		r->default_host->password_expiry_warning = to_seconds(sym);
		continue;
	    default:
		parse_error_expect(sym, S_acl, S_maxattempts, S_expiry, S_unknown);
	    }
	case S_pap:
	    sym_get(sym);
	    switch (sym->code) {
	    case S_backend:
		sym_get(sym);
		parse(sym, S_equal);
		parse(sym, S_mavis);
		if (sym->code == S_prefetch) {
		    sym_get(sym);
		    r->mavis_pap_prefetch = TRISTATE_YES;
		}
		r->mavis_pap = TRISTATE_YES;
		r->mavis_userdb = TRISTATE_YES;
		break;
	    case S_password:
		parse_host_pap_password(sym, r->default_host);
		continue;
	    default:
		parse_error_expect(sym, S_backend, S_login, S_unknown);
	    }
	    continue;
	case S_login:
	    sym_get(sym);
	    parse(sym, S_backend);
	    parse(sym, S_equal);
	    parse(sym, S_mavis);
	    while (1) {
		switch (sym->code) {
		case S_prefetch:
		    sym_get(sym);
		    r->mavis_login_prefetch = TRISTATE_YES;
		    continue;
		case S_chalresp:
		    sym_get(sym);
		    r->chalresp = TRISTATE_YES;
		    if (sym->code == S_noecho) {
			sym_get(sym);
			r->chalresp_noecho = TRISTATE_YES;
		    }
		    continue;
		case S_chpass:
		    sym_get(sym);
		    r->chpass = TRISTATE_YES;
		    continue;
		default:;
		}
		break;
	    }
	    r->mavis_login = TRISTATE_YES;
	    r->mavis_userdb = TRISTATE_YES;
	    continue;
	case S_accounting:
	    sym_get(sym);
	    parse(sym, S_log);
	    parse(sym, S_equal);
	    log_add(sym, &r->acctlog, sym->buf, r);
	    sym_get(sym);
	    continue;
	case S_authentication:
	    sym_get(sym);
	    switch (sym->code) {
	    case S_log:
		sym_get(sym);
		parse(sym, S_equal);
		log_add(sym, &r->accesslog, sym->buf, r);
		sym_get(sym);
		continue;
	    case S_fallback:
		sym_get(sym);
		switch (sym->code) {
		case S_equal:
		    sym_get(sym);
		    r->default_host->authfallback = parse_tristate(sym);
		    break;
		case S_period:
		    r->backend_failure_period = parse_seconds(sym);
		    break;
		default:
		    parse_error_expect(sym, S_equal, S_period, S_unknown);
		}
		continue;
	    default:
		parse_error_expect(sym, S_log, S_fallback, S_unknown);
	    }
	case S_access:
	    sym_get(sym);
	    parse(sym, S_log);
	    parse(sym, S_equal);
	    log_add(sym, &r->accesslog, sym->buf, r);
	    sym_get(sym);
	    continue;
	case S_authorization:
	    sym_get(sym);
	    parse(sym, S_log);
	    parse(sym, S_equal);
	    log_add(sym, &r->authorlog, sym->buf, r);
	    sym_get(sym);
	    continue;
	case S_radius_access:
	    sym_get(sym);
	    parse(sym, S_log);
	    parse(sym, S_equal);
	    log_add(sym, &r->rad_accesslog, sym->buf, r);
	    sym_get(sym);
	    continue;
	case S_radius_accounting:
	    sym_get(sym);
	    parse(sym, S_log);
	    parse(sym, S_equal);
	    log_add(sym, &r->rad_acctlog, sym->buf, r);
	    sym_get(sym);
	    continue;
	case S_warning:
	    sym_get(sym);
	    parse(sym, S_period);
	    parse(sym, S_equal);
	    r->warning_period = parse_seconds(sym);
	    if (r->warning_period < 60)
		r->warning_period *= 86400;
	    continue;
	case S_connection:
	    sym_get(sym);
	    switch (sym->code) {
	    case S_timeout:
		sym_get(sym);
		parse(sym, S_equal);
		r->default_host->tcp_timeout = parse_seconds(sym);
		break;
	    case S_log:
		sym_get(sym);
		parse(sym, S_equal);
		log_add(sym, &r->connlog, sym->buf, r);
		sym_get(sym);
		break;
	    default:
		parse_error_expect(sym, S_timeout, S_log, S_unknown);
	    }
	    continue;
	case S_dns:
	    sym_get(sym);
	    switch (sym->code) {
	    case S_preload:
		sym_get(sym);
		switch (sym->code) {
		case S_file:
		    {
			glob_t globbuf = { 0 };

			sym_get(sym);
			parse(sym, S_equal);
			// dns preload file = /etc/hosts

			globerror_sym = sym;

			switch (glob(sym->buf, GLOB_ERR | GLOB_NOESCAPE | GLOB_NOMAGIC | GLOB_BRACE, globerror, &globbuf)) {
			case 0:
			    for (int i = 0; i < (int) globbuf.gl_pathc; i++)
				parse_etc_hosts(globbuf.gl_pathv[i], r);
			    break;
#ifdef GLOB_NOMATCH
			case GLOB_NOMATCH:
			    globerror(sym->buf, ENOENT);
			    break;
#endif				/* GLOB_NOMATCH */
			default:
			    parse_etc_hosts(sym->buf, r);
			    globfree(&globbuf);
			}
			sym_get(sym);
			continue;
		    }
		case S_address:
		    {
			// dns preload address $ip = $name
			struct in6_addr a;
			int cm = 128;

			parse(sym, S_address);

			if (v6_ptoh(&a, &cm, sym->buf))
			    parse_error(sym, "Expected an IP address or network in CIDR notation, but got '%s'.", sym->buf);
			sym_get(sym);
			parse(sym, S_equal);

			if (!r->dns_tree_ptr[0])
			    r->dns_tree_ptr[0] = radix_new(free_reverse, NULL);
			radix_add(r->dns_tree_ptr[0], &a, cm, strdup(sym->buf));

			sym_get(sym);
			continue;
		    }
		default:
		    parse_error_expect(sym, S_address, S_file, S_unknown);
		}
#ifdef WITH_DNS
	    case S_reverselookup:
	    case S_timeout:
		parse_host_dns(sym, r->default_host);
		continue;
	    case S_cache:
		sym_get(sym);
		parse(sym, S_period);
		parse(sym, S_equal);
		r->dns_caching_period = parse_int(sym);
		continue;
	    case S_servers:
		sym_get(sym);
		if (sym->code == S_vrf) {
		    sym_get(sym);
		    if (!r->idc)
			r->idc = io_dns_init(common_data.io);
		    io_dns_set_vrf(r->idc, sym->buf);
		    sym_get(sym);
		}
		parse(sym, S_equal);
		if (!r->idc)
		    r->idc = io_dns_init(common_data.io);
		io_dns_set_servers(r->idc, sym->buf);
		sym_get(sym);
		continue;
#endif
	    default:
		parse_error_expect(sym, S_preload,
#ifdef WITH_DNS
				   S_reverselookup, S_timeout, S_servers,
#endif
				   S_unknown);
	    }
	    continue;
	case S_cache:
	    sym_get(sym);
	    parse(sym, S_timeout);
	    parse(sym, S_equal);
	    r->caching_period = parse_seconds(sym);
	    continue;
	case S_log:
	    sym_get(sym);
	    parse_log(sym, r);
	    continue;
	case S_umask:
	    top_only(sym, r);
	    parse_umask(sym, &config.mask);
	    continue;
	case S_retire:
	    top_only(sym, r);
	    sym_get(sym);
	    switch (sym->code) {
	    case S_limit:
		sym_get(sym);
		parse(sym, S_equal);
		config.retire = parse_int(sym);
		continue;
	    case S_timeout:
		sym_get(sym);
		parse(sym, S_equal);
		config.suicide = parse_seconds(sym) + io_now.tv_sec;
		continue;
	    default:
		parse_error_expect(sym, S_limit, S_timeout, S_unknown);
	    }
	    continue;
	case S_last_recently_used:
	    top_only(sym, r);
	    sym_get(sym);
	    parse(sym, S_limit);
	    parse(sym, S_equal);
	    config.ctx_lru_threshold = parse_int(sym);
	    continue;
	case S_radius_dictionary:
	    top_only(sym, r);
	    parse_radius_dictionary(sym);
	    continue;
	case S_user:
	    parse_user(sym, r);
	    continue;
	case S_group:
	    parse_group(sym, r, 0);
	    continue;
	case S_profile:
	    parse_profile(sym, r, NULL);
	    continue;
	case S_acl:
	    parse_tac_acl(sym, r);
	    continue;
	case S_mavis:
	    sym_get(sym);
	    switch (sym->code) {
	    case S_module:
		if (parse_mavismodule(&r->mcx, common_data.io, sym))
		    scm_fatal();
		continue;
	    case S_path:
		parse_mavispath(sym);
		continue;
	    case S_cache:
		sym_get(sym);
		parse(sym, S_timeout);
		parse(sym, S_equal);
		r->caching_period = parse_seconds(sym);
		continue;
	    case S_noauthcache:
		sym_get(sym);
		r->mavis_noauthcache = TRISTATE_YES;
		continue;
	    case S_user:
		sym_get(sym);
		parse(sym, S_filter);
		parse(sym, S_equal);
		r->mavis_user_acl = tac_acl_lookup(sym->buf, r);
		if (!r->mavis_user_acl)
		    parse_error(sym, "ACL '%s' not found.", sym->buf);
		sym_get(sym);
		continue;
	    default:
		parse_error_expect(sym, S_module, S_path, S_cache, S_unknown);
	    }
	case S_enable:
	    sym_get(sym);
	    switch (sym->code) {
	    case S_user:
		sym_get(sym);
		parse(sym, S_acl);
		parse(sym, S_equal);
		r->enable_user_acl = tac_acl_lookup(sym->buf, r);
		if (!r->enable_user_acl)
		    parse_error(sym, "ACL '%s' not found.", sym->buf);
		sym_get(sym);
		break;
	    default:
		{
		    int dummy;
		    if (!r->default_host->enable)
			r->default_host->enable = calloc(sizeof(struct pwdat *), TAC_PLUS_PRIV_LVL_MAX + 1);
		    if (sym->code != S_equal || 1 != sscanf(sym->buf, "%d", &dummy))
			parse_error(sym, "Expected '=', 'user' or a privilege level, but got '%s'", sym->buf);
		    parse_enable(sym, NULL, r->default_host->enable);
		}
	    }
	    continue;
	case S_net:
	    parse_net(sym, r, NULL);
	    continue;
	case S_parent:
	    sym_get(sym);
	    parse(sym, S_equal);
	    r->parent = lookup_realm(sym->buf, config.default_realm);
	    if (!r->parent)
		parse_error(sym, "Realm '%s' not found.", sym->buf);
	    if (loopcheck_realm(r))
		parse_error(sym, "'%s': circular reference rejected", sym->buf);
	    sym_get(sym);
	    continue;
	case S_ruleset:
	    parse_ruleset(sym, r);
	    continue;
	case S_timespec:
	    if (!r->timespectable)
		r->timespectable = init_timespec();
	    parse_timespec(r->timespectable, sym);
	    continue;
	case S_time:
	    sym_get(sym);
	    parse(sym, S_zone);
	    parse(sym, S_equal);
	    setenv("TZ", sym->buf, 1);
	    tzset();
	    sym_get(sym);
	    continue;
	case S_realm:
	    {
		tac_realm *newrealm, *rp;
		sym_get(sym);
		if ((rp = lookup_realm(sym->buf, config.default_realm)) && rp->parent != r)
		    parse_error(sym, "Realm '%s' already defined at line %u", sym->buf, rp->line);
		if (!r->realms)
		    r->realms = RB_tree_new(compare_name, NULL);
		char *name = strdup(sym->buf);
		sym_get(sym);
		if (sym->code == S_openbra) {
		    sym_get(sym);
		    newrealm = parse_realm(sym, name, r, rp, 0);
		    parse(sym, S_closebra);
		} else
		    newrealm = parse_realm(sym, name, r, rp, 1);
		if (rp)
		    free(name);
		else
		    RB_insert(r->realms, newrealm);
		continue;
	    }
	case S_trace:
	case S_debug:
	    sym_get(sym);
	    switch (sym->code) {
	    case S_redirect:
		sym_get(sym);
		parse(sym, S_equal);
		if (freopen(sym->buf, "w+", stderr)) {
		    common_data.debug_redirected = 1;
		    common_data.font_blue = "";
		    common_data.font_red = "";
		    common_data.font_plain = "";
		    common_data.font_bold = "";
		}
		sym_get(sym);
		break;
	    case S_equal:
		parse(sym, S_equal);
		parse_debug(sym, &r->debug);
		break;
	    default:
		parse_error_expect(sym, S_redirect, S_equal, S_unknown);
	    }
	    continue;
#ifdef WITH_PCRE2
	case S_rewrite:
	    sym_get(sym);
	    parse_rewrite(sym, r);
	    continue;
#endif
	case S_skip:
	    sym_get(sym);
	    parse(sym, S_parent_script);
	    parse(sym, S_equal);
	    r->skip_parent_script = parse_bistate(sym);
	    r->default_host->skip_parent_script = r->skip_parent_script;
	    continue;
	case S_anonenable:
	case S_key:
	case S_radius_key:
	case S_motd:
	case S_welcome:
	case S_reject:
	case S_permit:
	case S_bug:
	case S_augmented_enable:
	case S_singleconnection:
	case S_context:
	case S_script:
	case S_message:
	case S_session:
	case S_maxrounds:
	case S_host:
	case S_device:
	    parse_host_attr(sym, r, r->default_host);
	    continue;
	case S_haproxy:
	    sym_get(sym);
	    parse(sym, S_autodetect);
	    parse(sym, S_equal);
	    r->haproxy_autodetect = parse_tristate(sym);
	    continue;
#if defined(WITH_SSL)
	case S_tls:
	    sym_get(sym);
	    switch (sym->code) {
#if defined(WITH_SSL) && !defined(OPENSSL_NO_PSK)
	    case S_psk:
		sym_get(sym);
		switch (sym->code) {
		case S_id:
		    sym_get(sym);
		    parse(sym, S_equal);
		    r->default_host->tls_psk_id = strdup(sym->buf);
		    sym_get(sym);
		    break;
		case S_key:
		    sym_get(sym);
		    parse(sym, S_equal);
		    parse_tls_psk_key(sym, r->default_host);
		    break;
		case S_equal:
		    sym_get(sym);
		    r->use_tls_psk = parse_bool(sym);
		    break;
		default:
		    parse_error_expect(sym, S_id, S_key, S_equal, S_unknown);
		}
		continue;
#endif
	    case S_cert_file:
		sym_get(sym);
		parse(sym, S_equal);
		r->tls_cert = confdir_strdup(sym->buf);
		sym_get(sym);
		continue;
	    case S_key_file:
		sym_get(sym);
		parse(sym, S_equal);
		r->tls_key = confdir_strdup(sym->buf);
		sym_get(sym);
		continue;
	    case S_cafile:
		sym_get(sym);
		parse(sym, S_equal);
		r->tls_cafile = confdir_strdup(sym->buf);
		sym_get(sym);
		continue;
	    case S_passphrase:
		sym_get(sym);
		parse(sym, S_equal);
		r->tls_pass = strdup(sym->buf);
		sym_get(sym);
		continue;
	    case S_ciphers:
		sym_get(sym);
		parse(sym, S_equal);
		r->tls_ciphers = strdup(sym->buf);
		sym_get(sym);
		continue;
	    case S_accept:
		sym_get(sym);
		parse(sym, S_expired);
		parse(sym, S_equal);
		r->tls_accept_expired = parse_tristate(sym);
		continue;
	    case S_verify_depth:
		sym_get(sym);
		parse(sym, S_equal);
		r->tls_verify_depth = parse_int(sym);
		continue;
	    case S_alpn:
		sym_get(sym);
		parse(sym, S_equal);
#ifdef WITH_SSL
		r->alpn_vec = str2protocollist(sym->buf, &r->alpn_vec_len);
		if (!r->alpn_vec)
		    parse_error(sym, "TLS ALPN is malformed.");
#endif
		sym_get(sym);
		continue;
#ifdef WITH_SSL
	    case S_sni:
		sym_get(sym);
		switch (sym->code) {
		case S_equal:
		    sym_get(sym);
		    add_sni(sym, r);
		    continue;
		case S_required:
		    sym_get(sym);
		    parse(sym, S_equal);
		    r->tls_sni_required = parse_tristate(sym);
		    continue;
		default:
		    parse_error_expect(sym, S_equal, S_required, S_unknown);
		}
#endif
	    case S_autodetect:
		sym_get(sym);
		parse(sym, S_equal);
		r->tls_autodetect = parse_tristate(sym);
		continue;
	    default:
		parse_error_expect(sym, S_cert_file, S_key_file, S_cafile, S_passphrase, S_ciphers, S_peer, S_accept, S_verify_depth, S_alpn, S_autodetect,
#ifdef WITH_SSL
				   S_sni,
#endif
				   S_unknown);
	    }
	    continue;
#endif
	case S_syslog:
	case S_proctitle:
	case S_coredump:
	case S_alias:
	case S_cleanup:
	    top_only(sym, r);
	    parse_common(sym);
	    break;
	case S_script_order:
	    sym_get(sym);
	    switch (sym->code) {
	    case S_realm:
		r->script_realm_parent_first = parse_script_order(sym);
		break;
	    case S_host:
		r->script_host_parent_first = parse_script_order(sym);
		break;
	    case S_profile:
		r->script_profile_parent_first = parse_script_order(sym);
		break;
	    default:
		parse_error_expect(sym, S_host, S_realm, S_profile, S_unknown);
	    }
	    continue;
	case S_aaa_protocol_allowed:
	    sym_get(sym);
	    parse(sym, S_equal);
	    r->allowed_protocol_radius = TRISTATE_NO;
	    r->allowed_protocol_radius_tcp = TRISTATE_NO;
	    r->allowed_protocol_radsec = TRISTATE_NO;
	    r->allowed_protocol_tacacs = TRISTATE_NO;
	    r->allowed_protocol_tacacss = TRISTATE_NO;
	    do {
		switch (sym->code) {
		case S_radius:
		    r->allowed_protocol_radius = TRISTATE_YES;
		    break;
		case S_radius_tcp:
		    r->allowed_protocol_radius_tcp = TRISTATE_YES;
		    break;
		case S_radsec:
		    r->allowed_protocol_radsec = TRISTATE_YES;
		    break;
		case S_tacacs:
		    r->allowed_protocol_tacacs = TRISTATE_YES;
		    break;
		case S_tacacss:
		    r->allowed_protocol_tacacss = TRISTATE_YES;
		    break;
		default:
		    parse_error_expect(sym, S_radius, S_radius_tcp, S_radsec, S_tacacs, S_tacacss, S_unknown);
		}
		sym_get(sym);
	    } while (parse_comma(sym));

	    continue;
	default:
	    parse_error_expect(sym, S_password, S_pap, S_login, S_accounting, S_authentication, S_access, S_authorization, S_warning,
			       S_connection, S_dns, S_cache, S_log, S_umask, S_retire, S_user, S_group, S_profile, S_acl, S_mavis,
			       S_enable, S_net, S_parent, S_ruleset, S_timespec, S_time, S_realm, S_trace, S_debug,
			       S_anonenable,
			       S_key, S_motd, S_welcome, S_reject, S_permit, S_bug, S_augmented_enable, S_singleconnection, S_context,
			       S_script, S_message, S_session, S_maxrounds, S_host, S_device, S_syslog, S_proctitle, S_coredump, S_alias,
			       S_script_order, S_skip, S_aaa_protocol_allowed,
#ifdef WITH_PCRE2
			       S_rewrite,
#endif
#if defined(WITH_SSL)
			       S_tls, S_radius_dictionary,
#endif
			       S_unknown);
	}
}

void parse_decls(struct sym *sym)
{
    config.default_realm = parse_realm(sym, "default", NULL, NULL, 0);
}

static time_t parse_date(struct sym *sym, time_t offset)
{
    int m, d, y;

    if (3 == sscanf(sym->buf, "%d-%d-%d", &y, &m, &d)) {
	struct tm tm = { 0 };

	tm.tm_year = y - 1900;
	tm.tm_mon = m - 1;
	tm.tm_mday = d;
	sym_get(sym);
	return mktime(&tm) + offset;
    }
    long long ll;
    if (1 == sscanf(sym->buf, "%lld", &ll)) {
	sym_get(sym);
	return (time_t) ll;
    }
    parse_error(sym, "Unrecognized date '%s' (expected format: YYYY-MM-DD)", sym->buf);

    return (time_t) 0;
}

void free_user(tac_user *user)
{
    while (user->alias) {
	tac_alias *next = user->alias->next;
	RB_search_and_delete(user->realm->aliastable, user->alias);
	user->alias = next;
    }
    if (user->avc)
	av_free(user->avc);
    mem_destroy(user->mem);
}

static struct pwdat passwd_deny = {.type = S_deny };
static struct pwdat passwd_mavis = {.type = S_mavis };
static struct pwdat passwd_login = {.type = S_login };
static struct pwdat passwd_deny_dflt = {.type = S_deny };
static struct pwdat passwd_mavis_dflt = {.type = S_mavis };
static struct pwdat passwd_login_dflt = {.type = S_login };
static struct pwdat passwd_permit = {.type = S_permit };

tac_user *new_user(char *name, enum token type, tac_realm *r)
{
    mem_t *mem = NULL;
    tac_user *user;

    report(NULL, LOG_DEBUG, DEBUG_CONFIG_FLAG, "creating user %s in realm %s", name, r->name);

    if (type == S_mavis)
	mem = mem_create(M_LIST);
    user = mem_alloc(mem, sizeof(tac_user));
    user->name = mem_strdup(mem, name);
    user->name_len = strlen(name);
    user->mem = mem;
    user->realm = r;

    for (int i = 0; i <= PW_MAVIS; i++)
	user->passwd[i] = &passwd_deny_dflt;
    if (r->mavis_login == TRISTATE_YES)
	user->passwd[PW_LOGIN] = &passwd_mavis_dflt;
    if (r->mavis_pap == TRISTATE_YES)
	user->passwd[PW_PAP] = &passwd_mavis_dflt;
    if (r->default_host->map_pap_to_login == TRISTATE_YES) {
	if (r->mavis_login == TRISTATE_YES)
	    user->passwd[PW_PAP] = &passwd_mavis_dflt;
	else
	    user->passwd[PW_PAP] = &passwd_login_dflt;
    }

    return user;
}

tac_profile *new_profile(mem_t *mem, char *name, tac_realm *r)
{
    tac_profile *profile;

    report(NULL, LOG_DEBUG, DEBUG_CONFIG_FLAG, "creating profile %s in realm %s", name, r->name);

    profile = (tac_profile *) mem_alloc(mem, sizeof(tac_profile));
    profile->name = mem_strdup(mem, name);
    profile->name_len = strlen(name);
    profile->realm = r;
    return profile;
}

static void parse_group(struct sym *sym, tac_realm *r, tac_group *parent)
{
    sym_get(sym);

    if (sym->code == S_equal)
	sym_get(sym);

    tac_group *g = tac_group_new(sym, sym->buf, r);
    g->line = sym->line;

    sym_get(sym);
    g->parent = parent;

    if (sym->code == S_openbra) {
	sym_get(sym);

	while (sym->code != S_closebra)
	    switch (sym->code) {
	    case S_group:
		parse_group(sym, r, g);
		continue;
	    case S_parent:
		sym_get(sym);
		parse(sym, S_equal);
		tac_group *ng = lookup_group(sym->buf, r);
		if (!ng)
		    parse_error(sym, "Group '%s' not found.", sym->buf);
		g->parent = ng->parent;
		if (loopcheck_group(g))
		    parse_error(sym, "'%s': circular reference rejected", sym->buf);
		sym_get(sym);
		continue;
	    case S_member:
		parse_member(sym, &g->groups, NULL, r);
		continue;
	    default:
		parse_error_expect(sym, S_group, S_parent, S_closebra, S_unknown);
	    }
	sym_get(sym);
    }
}

static void parse_profile(struct sym *sym, tac_realm *r, tac_profile *parent)
{
    if (!r->profiletable)
	r->profiletable = RB_tree_new(compare_name, NULL);

    sym_get(sym);
    if (sym->code == S_equal)
	sym_get(sym);

    tac_profile *profile = new_profile(NULL, sym->buf, r);
    tac_profile *n = (tac_profile *) RB_lookup(r->profiletable, (void *) profile);
    if (n)
	parse_error(sym, "Profile '%s' already defined at line %u", profile->name, n->line);

    profile->parent = parent;
    profile->line = sym->line;
    sym_get(sym);
    parse_profile_attr(sym, profile, r);
    RB_insert(r->profiletable, profile);
}


static struct mavis_action *tac_script_parse_r(struct sym *, mem_t *, int, tac_realm *);

static void parse_ruleset(struct sym *sym, tac_realm *realm)
{
    struct tac_rule **r = &(realm->rules);
    while (*r)
	r = &(*r)->next;

    sym_get(sym);
    if (sym->code == S_equal)
	sym_get(sym);
    parse(sym, S_openbra);

    while (sym->code == S_rule) {
	char *rulename = NULL;
	char synthname[SCM_REALM_SIZE + 16];
	sym_get(sym);
	if (sym->code == S_openbra) {
	    rulename = synthname;
	    snprintf(synthname, sizeof(synthname), "%s#%d", realm->name, realm->rulecount++);
	    // no rule name
	} else
	    rulename = sym->buf;

	*r = calloc(1, sizeof(struct tac_rule));
	(*r)->acl.name = strdup(rulename);
	(*r)->enabled = 1;	// enabled by default
	if (rulename == sym->buf)
	    sym_get(sym);

	parse(sym, S_openbra);

	while (sym->code != S_closebra) {
	    switch (sym->code) {
	    case S_enabled:
		sym_get(sym);
		parse(sym, S_equal);
		(*r)->enabled = parse_bool(sym);
		continue;
	    case S_script:
		sym_get(sym);
		parse(sym, S_openbra);

		struct mavis_action **p = &(*r)->acl.action;

		while (*p)
		    p = &(*p)->n;

		*p = tac_script_parse_r(sym, NULL, 1, realm);

		parse(sym, S_closebra);

		continue;
	    default:
		parse_error_expect(sym, S_enabled, S_script, S_closebra, S_unknown);
	    }
	}
	sym_get(sym);
	r = &(*r)->next;
    }
    if (sym->code != S_closebra)
	parse_error_expect(sym, S_closebra, S_rule, S_unknown);
    sym_get(sym);
}

static enum token lookup_user_profile(tac_session *session)
{
    uint32_t crc32 = INITCRC32;
    if (session->nac_addr_ascii)
	crc32 = crc32_update(crc32, (u_char *) session->nac_addr_ascii, session->nac_addr_ascii_len);
    if (session->port)
	crc32 = crc32_update(crc32, (u_char *) session->port, session->port_len);
    for (int i = 0; i < USER_PROFILE_CACHE_SIZE; i++) {
	if (session->ctx->user_profile_cache[i].user == session->user && session->ctx->user_profile_cache[i].crc32 == crc32) {
	    if (session->ctx->user_profile_cache[i].valid_until >= io_now.tv_sec) {
		session->profile = session->ctx->user_profile_cache[i].profile;
		return session->ctx->user_profile_cache[i].res;
	    }
	    session->ctx->user_profile_cache[i].user = NULL;
	    session->ctx->user_profile_cache[i].profile = NULL;
	    return S_unknown;
	}
    }
    return S_unknown;
}

static void cache_user_profile(tac_session *session, enum token res)
{
    uint32_t crc32 = INITCRC32;
    if (session->nac_addr_ascii)
	crc32 = crc32_update(crc32, (u_char *) session->nac_addr_ascii, session->nac_addr_ascii_len);
    if (session->port)
	crc32 = crc32_update(crc32, (u_char *) session->port, session->port_len);

    int j = 0;
    for (int i = 0; i < USER_PROFILE_CACHE_SIZE; i++) {
	if (session->ctx->user_profile_cache[i].user == session->user && session->ctx->user_profile_cache[i].crc32 == crc32) {
	    j = i;
	    goto set;
	}
    }
    for (int i = 0; i < USER_PROFILE_CACHE_SIZE; i++) {
	if (session->ctx->user_profile_cache[j].valid_until > session->ctx->user_profile_cache[i].valid_until)
	    j = i;
	if (session->ctx->user_profile_cache[i].valid_until < io_now.tv_sec) {
	    j = i;
	    goto set;
	}
    }
  set:
    session->ctx->user_profile_cache[j].user = session->user;
    session->ctx->user_profile_cache[j].crc32 = crc32;
    session->ctx->user_profile_cache[j].profile = session->profile;
    session->ctx->user_profile_cache[j].res = res;
    session->ctx->user_profile_cache[j].valid_until = io_now.tv_sec + 120;
}

enum token eval_ruleset_r(tac_session *session, tac_realm *realm, int parent_first)
{
    enum token res = S_unknown;

    if (!realm)
	return res;

    if (session->user && session->user->profile) {
	session->profile = session->user->profile;
	return S_permit;
    }

    if (parent_first == TRISTATE_YES && realm->skip_parent_script != BISTATE_YES)
	res = eval_ruleset_r(session, realm->parent, parent_first);

    if (res == S_permit || res == S_deny)
	return res;

    struct tac_rule *rule = realm->rules;
    while (rule) {
	if (rule->enabled) {
	    res = eval_tac_acl(session, &rule->acl);
	    report(session, LOG_DEBUG, DEBUG_ACL_FLAG | DEBUG_REGEX_FLAG,
		   "%s@%s: ACL %s: %s (profile: %s)", session->username,
		   session->nac_addr_ascii, rule->acl.name, codestring[res].txt, session->profile ? session->profile->name : "n/a");
	    switch (res) {
	    case S_permit:
	    case S_deny:
		cache_user_profile(session, res);
		session->rule = rule->acl.name;
		session->rule_len = rule->acl.name_len;
		return res;
	    default:;
	    }
	}
	rule = rule->next;
    }

    if (parent_first != TRISTATE_YES && realm->skip_parent_script != BISTATE_YES)
	res = eval_ruleset_r(session, realm->parent, parent_first);
    return res;
}

enum token eval_ruleset(tac_session *session, tac_realm *realm)
{
    enum token res = lookup_user_profile(session);
    if (res != S_unknown) {
	report(session, LOG_DEBUG, DEBUG_ACL_FLAG | DEBUG_REGEX_FLAG,
	       "%s@%s: cached: %s (profile: %s)", session->username,
	       session->nac_addr_ascii, codestring[res].txt, session->profile ? session->profile->name : "n/a");
	return res;
    }
    res = eval_ruleset_r(session, realm, session->ctx->realm->script_realm_parent_first);
    if (res == S_permit)
	return res;
    return S_deny;
}


static void parse_user(struct sym *sym, tac_realm *r)
{
    enum token type = sym->code;

    if (!r->usertable)
	r->usertable = RB_tree_new(compare_name, (void (*)(void *)) free_user);

    sym_get(sym);

    if (sym->code == S_backend) {
	parse(sym, S_backend);
	parse(sym, S_equal);
	switch (sym->code) {
	case S_mavis:
	    r->mavis_userdb = TRISTATE_YES;
	    break;
	case S_local:
	    r->mavis_userdb = TRISTATE_NO;
	    break;
	default:
	    parse_error_expect(sym, S_mavis, S_local, S_unknown);
	}
	sym_get(sym);
	return;
    }

    if (sym->code == S_equal)
	sym_get(sym);
    tac_user *user = new_user(sym->buf, type, r);
    user->line = sym->line;

    tac_user *n = (tac_user *) RB_lookup(r->usertable, (void *) user);
    if (n)
	parse_error(sym, "User '%s' already defined at line %u", user->name, n->line);

    sym_get(sym);
    parse_user_attr(sym, user);
    RB_insert(r->usertable, user);
    //report(NULL, LOG_INFO, ~0, "user %s added to realm %s", user->name, r->name);
}

int parse_host_profile(struct sym *sym, tac_realm *r, tac_host *host)
{
    sym->env_valid = 1;
    if (setjmp(sym->env))
	return -1;
    sym_init(sym);
    parse(sym, S_openbra);
    while (sym->code != S_closebra)
	parse_host_attr(sym, r, host);
    sym_get(sym);
    return 0;
}

int parse_user_profile(struct sym *sym, tac_user *user)
{
    sym->env_valid = 1;
    if (setjmp(sym->env))
	return -1;
    sym_init(sym);
    while (sym->code == S_openbra)
	parse_user_attr(sym, user);
    return 0;
}

int parse_user_profile_fmt(struct sym *sym, tac_user *user, char *fmt, ...)
{
    va_list ap;
    int l, len = 2 * MAX_INPUT_LINE_LEN;
    char *s = alloca(len);

    va_start(ap, fmt);
    l = vsnprintf(s, len, fmt, ap) + 1;
    if (l > len) {
	s = alloca(l);
	vsnprintf(s, l, fmt, ap);
    }
    va_end(ap);
    sym->in = sym->tin = s;
    sym->len = sym->tlen = l;
    return parse_user_profile(sym, user);
}


static char hexbyte(char *s)
{
    char *h = "\0\01\02\03\04\05\06\07\010\011\0\0\0\0\0\0\0\012\013\014\015\016\017\0\0\0\0\0\0\0\0\0";
    return (h[(s[0] - '0') & 0x1F] << 4) | h[(s[1] - '0') & 0x1F];
}

static int c7decode(mem_t *mem, char *in)
{
    char *out = in;
    size_t len = strlen(in);
    static char *c7 = NULL;
    static size_t c7_len = 0;

    if (!c7) {
	char *e = "051207055A0A070E204D4F08180416130A0D052B2A2529323423120617020057585952550F021917585956525354550A5A07065956";
	char *u, *t = e;

	c7 = mem_alloc(mem, strlen(e) / 2 + 1);
	u = c7;
	while (*t) {
	    *u = 'a' ^ hexbyte(t);
	    u++, t += 2;
	}
	c7_len = strlen(c7);
    }

    if (len & 1 || len < 4)
	return -1;

    len -= 2;
    int seed = 10 * (in[0] - '0') + in[1] - '0';
    in += 2;

    while (len) {
	*out = hexbyte(in) ^ c7[seed % c7_len];
	in += 2, seed++, len -= 2, out++;
    }

    *out = 0;

    return 0;
}

static struct pwdat *parse_pw(struct sym *sym, mem_t *mem, int cry)
{
    int c7 = 0;
    parse(sym, S_equal);

    switch (sym->code) {
    case S_mavis:
	sym_get(sym);
	return &passwd_mavis;
    case S_permit:
	sym_get(sym);
	return &passwd_permit;
    case S_login:
	sym_get(sym);
	return &passwd_login;
    case S_deny:
	sym_get(sym);
	return &passwd_deny;
    case S_crypt:
	if (cry)
	    break;
    case S_clear:
	break;
    case S_7:
	sym->code = S_clear;
	c7++;
	break;
    default:
	parse_error_expect(sym, S_clear, S_permit, S_deny, S_login, cry ? S_crypt : S_unknown, S_unknown);
    }

    enum token sc = sym->code;
    sym_get(sym);

    if (c7 && c7decode(mem, sym->buf))
	parse_error(sym, "type 7 password is malformed");

    struct pwdat *pp = mem_alloc(mem, sizeof(struct pwdat) + strlen(sym->buf));
    pp->type = sc;
    strcpy(pp->value, sym->buf);
    sym_get(sym);
    return pp;
}

static void parse_password(struct sym *sym, tac_user *user)
{
    int one = 0;

    sym_get(sym);

    switch (sym->code) {
    case S_login:
    case S_pap:
    case S_chap:
    case S_mschap:
	one = 1;
    default:;
    }

    if (one || sym->code == S_openbra) {
	if (!one)
	    sym_get(sym);

	enum pw_ix pw_ix = 0;
	struct pwdat **pp;
	pp = user->passwd;

	while (sym->code != S_closebra) {
	    int cry = 0;
	    switch (sym->code) {
	    case S_login:
		pw_ix = PW_LOGIN, cry = 1;
		break;
	    case S_pap:
		pw_ix = PW_PAP, cry = 1;
		break;
	    case S_chap:
		pw_ix = PW_CHAP;
		break;
	    case S_mschap:
		pw_ix = PW_MSCHAP;
		break;
	    default:
		parse_error_expect(sym, S_login, S_pap, S_chap, S_mschap, one ? S_unknown : S_closebra, S_unknown);
	    }
	    sym_get(sym);
	    if (sym->code == S_fallback) {
		switch (pw_ix) {
		case PW_LOGIN:
		    pw_ix = PW_LOGIN_FALLBACK;
		    sym_get(sym);
		    break;
		case PW_PAP:
		    pw_ix = PW_PAP_FALLBACK;
		    sym_get(sym);
		    break;
		default:;
		}
	    }
	    pp[pw_ix] = parse_pw(sym, user->mem, cry);
	    if (one)
		break;
	}
	if (!one)
	    sym_get(sym);
    } else
	parse_error_expect(sym, S_login, S_pap, S_chap, S_mschap, S_openbra, S_unknown);
}

static struct tac_acl *tac_acl_lookup(char *s, tac_realm *r)
{
    struct tac_acl a = {.name = s,.name_len = strlen(s) };
    while (r) {
	if (r->acltable) {
	    struct tac_acl *res = RB_lookup(r->acltable, &a);
	    if (res)
		return res;
	}
	r = r->parent;
    }
    return NULL;
}

static void parse_member(struct sym *sym, tac_groups **groups, mem_t *mem, tac_realm *r)
{
    sym_get(sym);

    parse(sym, S_equal);
    if (!*groups)
	*groups = mem_alloc(mem, sizeof(tac_groups));

    do {
	tac_group *g = lookup_group(sym->buf, r);
	if (g)
	    tac_group_add(g, *groups, mem);
	else if (!setjmp(sym->env)) {
	    tac_group_new(sym, sym->buf, r);
	    parse_error(sym, "Group '%s' not found.", sym->buf);
	}

	sym_get(sym);
    }
    while (parse_comma(sym));
}

static void parse_enable(struct sym *sym, mem_t *mem, struct pwdat **enable)
{
    int level = TAC_PLUS_PRIV_LVL_MAX;

    if (1 == sscanf(sym->buf, "%d", &level)) {
	if (level < TAC_PLUS_PRIV_LVL_MIN)
	    level = TAC_PLUS_PRIV_LVL_MIN;
	else if (level > TAC_PLUS_PRIV_LVL_MAX)
	    level = TAC_PLUS_PRIV_LVL_MAX;
	sym_get(sym);
    }

    enable[level] = parse_pw(sym, mem, 1);
}

static void parse_profile_attr(struct sym *sym, tac_profile *profile, tac_realm *r)
{
    struct mavis_action **p;
    mem_t *mem = profile->mem;

    parse(sym, S_openbra);

    while (sym->code != S_closebra)
	switch (sym->code) {
	case S_script:
	    sym_get(sym);
	    p = &profile->action;
	    while (*p)
		p = &(*p)->n;
	    *p = tac_script_parse_r(sym, mem, 0, r);
	    continue;
	case S_debug:
	    sym_get(sym);
	    parse(sym, S_equal);
	    parse_debug(sym, &profile->debug);
	    continue;
	case S_hushlogin:
	    sym_get(sym);
	    parse(sym, S_equal);
	    profile->hushlogin = parse_tristate(sym);
	    continue;
	case S_enable:
	    sym_get(sym);
	    if (!profile->enable)
		profile->enable = mem_alloc(mem, sizeof(struct pwdat *) * (TAC_PLUS_PRIV_LVL_MAX + 1));
	    parse_enable(sym, mem, profile->enable);
	    continue;
	case S_profile:
	    if (profile->mem)
		parse_error(sym, "User profiles may not contain sub profiles", sym->buf);
	    parse_profile(sym, r, profile);
	    continue;
	case S_parent:
	    sym_get(sym);
	    parse(sym, S_equal);
	    tac_realm *rp = r;
	    profile->parent = NULL;
	    while (rp && !profile->parent) {
		profile->parent = lookup_profile(sym->buf, r);
		rp = rp->parent;
	    }
	    if (!profile->parent)
		parse_error(sym, "Host '%s' not found.", sym->buf);
	    if (loopcheck_profile(profile))
		parse_error(sym, "'%s': circular reference rejected", sym->buf);
	    sym_get(sym);
	    continue;
	case S_skip:
	    sym_get(sym);
	    parse(sym, S_parent_script);
	    parse(sym, S_equal);
	    profile->skip_parent_script = parse_bistate(sym);
	    continue;
	default:
	    parse_error_expect(sym, S_script, S_debug, S_hushlogin, S_enable, S_profile, S_skip, S_closebra, S_unknown);
	}
    sym_get(sym);
}

// FIXME
//
// MD5 hashing is what IOS currently uses internally. In the CLI config the
// hash gets represented as a sequence of hex bytes.
//
// As there's no standard representation for SSH hashes it might be advisable
// to run a normalizing compare function instead of just strcmp().
//
// Sample Perl normalization code (and probably reusable for PCRE) for MD5 hash normalization:
//
// if ($key =~ /(([\da-f]{2}:?){16})( |$)/i) {
//     my $raw = lc $1;
//     $raw =~ s/://g;
// }
//
// On the other hand, it might make sense to ignore what routers currently do and
// take the ssh-keygen fingerprint as a reference:
// MD5:00:01:02:03:04:...:0E:0F
// SHA256:<base64>
//
// Also, giving the router the option to send more than one fingerprint seems appropriate.
// The server then has the option to require one or more fingerprints to match, e.g.
// MD5 *and* SHA256.
//

struct ssh_key {
    struct ssh_key *next;
    char *key;
    char hash[1];
};

enum token validate_ssh_hash(tac_session *session, char *hash, char **key)
{
    enum token res = S_deny;
    *key = NULL;
    if (hash) {
	// assumption: NAD sends a single hash
	struct ssh_key **ssh_key = &session->user->ssh_key;
	// Check hashes with key first:
	while (*ssh_key) {
	    if ((*ssh_key)->key && !strcmp((*ssh_key)->hash, hash)) {
		*key = (*ssh_key)->key;
		return S_permit;
	    }
	    ssh_key = &((*ssh_key)->next);
	}
	// Try hashes without key:
	ssh_key = &session->user->ssh_key;
	while (*ssh_key) {
	    if (!(*ssh_key)->key && !strcmp((*ssh_key)->hash, hash))
		return S_permit;
	    ssh_key = &((*ssh_key)->next);
	}
    }
    return res;
}

static void parse_sshkeyhash(struct sym *sym, tac_user *user)
{
    struct ssh_key **ssh_key = &user->ssh_key;
    while (*ssh_key)
	ssh_key = &((*ssh_key)->next);

    do {
	size_t len = strlen(sym->buf);
	*ssh_key = mem_alloc(user->mem, sizeof(struct ssh_key) + len);
	memcpy((*ssh_key)->hash, sym->buf, len + 1);
	sym_get(sym);
	ssh_key = &((*ssh_key)->next);
    }
    while (parse_comma(sym));
}

// Experimental SSH Cert validation code

struct ssh_key_id {
    struct ssh_key_id *next;
    char s[1];
};

enum token validate_ssh_key_id(tac_session *session)
{
    if (!session->user->ssh_key_id) {
	if (strcmp(session->username, session->ssh_key_id))
	    return S_deny;
	return S_permit;
    }
    struct ssh_key_id **ssh_key_id = &session->user->ssh_key_id;
    while (*ssh_key_id) {
	size_t len = strlen((*ssh_key_id)->s) + 1;
	char *v = alloca(len);
	memcpy(v, (*ssh_key_id)->s, len);
	while (*v) {
	    char *e;
	    int quoted = 0;

	    quoted = (*v == '"');
	    if (quoted)
		v++;
	    for (e = v; *e && *e != ','; e++);
	    if (quoted && *(e - 1) != '"')
		break;
	    if (quoted)
		*(e - 1) = 0;
	    *e++ = 0;
	    if (!strcmp(session->ssh_key_id, v))
		return S_permit;
	    v = e;
	    if (!*v || *v != ',')
		break;
	    v++;
	}
	ssh_key_id = &((*ssh_key_id)->next);
    }
    return S_deny;
}

static void parse_sshkeyid(struct sym *sym, tac_user *user)
{
    struct ssh_key_id **ssh_key_id = &user->ssh_key_id;
    while (*ssh_key_id)
	ssh_key_id = &((*ssh_key_id)->next);

    do {
	size_t len;
	len = strlen(sym->buf);
	*ssh_key_id = mem_alloc(user->mem, sizeof(struct ssh_key_id) + len);
	memcpy((*ssh_key_id)->s, sym->buf, len + 1);
	sym_get(sym);
	ssh_key_id = &((*ssh_key_id)->next);
    }
    while (parse_comma(sym));
}

#ifdef WITH_CRYPTO
#if OPENSSL_VERSION_NUMBER < 0x30000000
#include <openssl/md5.h>
#include <openssl/sha.h>
#else
#include <openssl/evp.h>
#endif

static char *calc_ssh_key_hash(char *hashname, unsigned char *in, size_t in_len)
{
    static unsigned char out[512];
    unsigned char md[256];
    size_t md_len = sizeof(md);
    size_t hashname_len;
    unsigned char *o;
    char *t = strchr(hashname, ':');
    if (t) {
	// <HASHNAME>:<HASH> => <HASHNAME>
	ssize_t len = t - hashname;
	t = alloca(len);
	len--;
	t[len] = 0;
	while (len > -1) {
	    len--;
	    t[len] = hashname[len];
	}
	hashname = t;
    }
    if (!in_len)
	in_len = strlen((char *) in);
#if OPENSSL_VERSION_NUMBER < 0x30000000
    if (!strcmp(hashname, "MD5")) {
	if (!MD5((const unsigned char *) in, (unsigned long) in_len, md))
	    return NULL;
	md_len = MD5_DIGEST_LENGTH;
    } else if (!strcmp(hashname, "SHA256")) {
	if (!SHA256((const unsigned char *) in, (size_t) in_len, md))
	    return NULL;
	md_len = SHA256_DIGEST_LENGTH;
    } else
	return NULL;
#else
    if (!EVP_Q_digest(NULL, hashname, NULL, in, in_len, md, &md_len))
	return NULL;
#endif

    if (!strcmp(hashname, "MD5")) {
	char hex[] = "0123456789abcdef";
	o = out;
	*o++ = 'M';
	*o++ = 'D';
	*o++ = '5';
	*o++ = ':';
	for (int i = 0; i < 16; i++) {
	    *o++ = hex[md[i] >> 4];
	    *o++ = hex[md[i] & 15];
	    *o++ = (i < 15) ? ':' : 0;
	}
	return (char *) out;
    }

    hashname_len = strlen(hashname);
    memcpy(out, hashname, hashname_len);
    o = out + hashname_len;
    *o++ = ':';
    if (EVP_EncodeBlock(o, md, md_len))
	return (char *) out;

    return NULL;
}

static void parse_sshkey(struct sym *sym, tac_user *user)
{
    struct ssh_key **ssh_key = &user->ssh_key;

    while (*ssh_key)
	ssh_key = &((*ssh_key)->next);

    do {
	size_t slen = strlen(sym->buf);
	unsigned char *t = alloca(slen);
	char *hash, *key;
	size_t hash_len;
	int len = -1;
	int pad = 0;
	char *p = sym->buf;
	static char *begin_marker = "---- BEGIN SSH2 PUBLIC KEY ----";
	static char *end_marker = "---- END SSH2 PUBLIC KEY ----";
	static size_t begin_marker_len = 0;
	static size_t end_marker_len = 0;
	int is_rfc4716 = 0;

	if (!begin_marker_len) {
	    begin_marker_len = strlen(begin_marker);
	    end_marker_len = strlen(end_marker);
	}

	if (!strncmp(p, begin_marker, begin_marker_len)) {
	    char *n = alloca(slen);
	    char *t = n;
	    char *h;
	    is_rfc4716 = 1;

	    p = strchr(p, '\n');	// skip header

	    // skip contuinations
	    h = strrchr(p, '\\');
	    if (h) {
		p = strchr(h, '\n');
		if (p) {
		    p++;
		    p = strchr(p, '\n');
		    if (p)
			p++;
		}
	    }

	    // skip remaining header lines
	    h = strrchr(p, ':');
	    if (h) {
		p = strchr(h, '\n');
		if (p)
		    p++;
	    }

	    while (p && strncmp(p, end_marker, end_marker_len)) {
		char *e = strchr(p, '\n');
		if (e) {
		    size_t l = e - p;
		    memcpy(t, p, l);
		    t += l;
		    p = e + 1;
		} else
		    p = NULL;
	    }
	    *t = 0;
	    p = n;
	}

	while (p && *p) {
	    char *space = strchr(p, ' ');
	    if (space) {
		slen = space - p;
		while (*space == ' ')
		    space++;
	    } else
		slen = strlen(p);
	    len = EVP_DecodeBlock(t, (const unsigned char *) p, slen);
	    if (len < 40)
		p = space;
	    else
		break;
	}

	if (!p || len < 40)
	    parse_error(sym, "BASE64 decode of SSH key failed.");

	if (p[slen - 1] == '=')
	    pad++;
	if (p[slen - 2] == '=')
	    pad++;
	len = (slen * 3) / 4 - pad;

	hash = calc_ssh_key_hash("MD5", t, len);
	if (!hash)
	    parse_error(sym, "MD5 hashing failed.");
	hash_len = strlen(hash);
	*ssh_key = mem_alloc(user->mem, sizeof(struct ssh_key) + len);
	if (is_rfc4716)
	    key = mem_strdup(user->mem, sym->buf);
	else {
	    int l = slen;
	    char *ck = alloca(slen + 200);
	    *ck = 0;
	    strcat(ck, begin_marker);
	    strcat(ck, "\n");
	    while (l > 0) {
		strncat(ck, p, 72);
		l -= 72;
		strcat(ck, "\n");
	    }
	    strcat(ck, end_marker);
	    strcat(ck, "\n");
	    key = mem_strdup(user->mem, ck);
	}
	(*ssh_key)->key = key;
	memcpy((*ssh_key)->hash, hash, len + 1);
	ssh_key = &((*ssh_key)->next);

	hash = calc_ssh_key_hash("SHA256", t, len);
	if (!hash)
	    parse_error(sym, "SHA256 hashing failed.");
	hash_len = strlen(hash);
	while (hash[hash_len - 1] == '=') {
	    hash_len--;
	    hash[hash_len] = 0;
	}
	*ssh_key = mem_alloc(user->mem, sizeof(struct ssh_key) + len);
	memcpy((*ssh_key)->hash, hash, len + 1);
	(*ssh_key)->key = key;

	sym_get(sym);
	ssh_key = &((*ssh_key)->next);

    }
    while (parse_comma(sym));
}
#endif				// WITH_SSL

static void parse_user_attr(struct sym *sym, tac_user *user)
{
    tac_realm *r = user->realm;

    parse(sym, S_openbra);

    while (sym->code != S_closebra) {
	switch (sym->code) {
	case S_member:
	    parse_member(sym, &user->groups, user->mem, r);
	    continue;
	case S_valid:
	    sym_get(sym);
	    switch (sym->code) {
	    case S_until:
		sym_get(sym);
		parse(sym, S_equal);
		user->valid_until = parse_date(sym, 86400);
		break;
	    case S_from:
		sym_get(sym);
	    default:
		parse(sym, S_equal);
		user->valid_from = parse_date(sym, 0);
		break;
	    }
	    continue;
	case S_debug:
	    sym_get(sym);
	    parse(sym, S_equal);
	    parse_debug(sym, &user->debug);
	    continue;
	case S_message:
	    sym_get(sym);
	    parse(sym, S_equal);
	    user->msg = mem_strdup(user->mem, sym->buf);
	    sym_get(sym);
	    continue;
	case S_password:
	    parse_password(sym, user);
	    continue;
	case S_enable:
	    sym_get(sym);
	    if (!user->enable)
		user->enable = mem_alloc(user->mem, sizeof(struct pwdat *) * (TAC_PLUS_PRIV_LVL_MAX + 1));
	    parse_enable(sym, user->mem, user->enable);
	    continue;
	case S_fallback_only:
	    sym_get(sym);
	    user->fallback_only = 1;
	    continue;
	case S_alias:
	    if (user->dynamic)
		parse_error(sym, "Aliases aren't available for dynamic users.");
	    else {
		tac_alias *a;
		sym_get(sym);
		parse(sym, S_equal);
		if (r->aliastable) {
		    tac_alias ta = {.name = sym->buf,.name_len = strlen(sym->buf) };
		    a = RB_lookup(r->aliastable, &ta);
		    if (a)
			parse_error(sym, "Alias '%s' already assigned to user '%s'.", sym->buf, a->name);
		} else
		    r->aliastable = RB_tree_new(compare_name, NULL);
		a = mem_alloc(user->mem, sizeof(tac_alias));
		a->name = mem_strdup(user->mem, sym->buf);
		a->name_len = strlen(sym->buf);
		a->user = user;
		a->line = sym->line;
		a->next = user->alias;
		user->alias = a;
		RB_insert(r->aliastable, a);
		sym_get(sym);
		continue;
	    }
	case S_tag:
	case S_usertag:
	    {
		if (!tags_by_name)
		    tags_by_name = RB_tree_new(compare_name, NULL);
		if (!user->tags)
		    user->tags = mem_alloc(user->mem, sizeof(tac_tags));
		sym_get(sym);
		parse(sym, S_equal);
		do
		    tac_tag_add(user->mem, tac_tag_parse(sym), user->tags);
		while (parse_comma(sym));
		continue;
	    }
#ifdef WITH_PCRE2
	case S_rewritten_only:
	    sym_get(sym);
	    user->rewritten_only = 1;
	    continue;
#endif
	case S_hushlogin:
	    sym_get(sym);
	    parse(sym, S_equal);
	    user->hushlogin = parse_tristate(sym);
	    continue;
	case S_ssh_key_hash:
	    sym_get(sym);
	    parse(sym, S_equal);
	    parse_sshkeyhash(sym, user);
	    continue;
#ifdef WITH_CRYPTO
	case S_ssh_key:
	    sym_get(sym);
	    parse(sym, S_equal);
	    parse_sshkey(sym, user);
	    continue;
#endif
	case S_ssh_key_id:
	    sym_get(sym);
	    parse(sym, S_equal);
	    parse_sshkeyid(sym, user);
	    continue;
	case S_profile:
	    sym_get(sym);
	    switch (sym->code) {
	    case S_openbra:
		if (!user->profile) {
		    user->profile = new_profile(user->mem, user->name, r);
		    user->profile->dynamie = 1;
		}
		if (!user->profile->dynamie)
		    parse_error(sym, "Profile is already set to '%s'", user->profile->name);
		parse_profile_attr(sym, user->profile, user->realm);
		break;
	    case S_equal:
		if (user->profile)
		    parse_error(sym, "Profile is already set to '%s'", user->profile->name);
		sym_get(sym);
		user->profile = lookup_profile(sym->buf, r);
		if (!user->profile)
		    parse_error(sym, "Profile '%s' not found.", sym->buf);
		sym_get(sym);
		continue;
	    default:
		parse_error_expect(sym, S_openbra, S_equal, S_unknown);
	    }
	    continue;
	default:
	    parse_error_expect(sym, S_member, S_valid, S_debug, S_message, S_password, S_enable, S_fallback_only, S_hushlogin, S_ssh_key_id,
#ifdef WITH_PCRE2
			       S_rewritten_only,
#endif
#ifdef WITH_CRYPTO
			       S_ssh_key,
#endif
			       S_alias, S_usertag, S_tag, S_profile, S_unknown);
	}
    }
    sym_get(sym);
}

static void add_host(struct sym *sym, radixtree_t *ht, tac_host *host)
{
    struct in6_addr a;
    int cm;
    if (v6_ptoh(&a, &cm, sym->buf))
	parse_error(sym, "Expected an IP address or network in CIDR notation, but got '%s'.", sym->buf);

    tac_host *h;
    if (ht && (h = radix_add(ht, &a, cm, host)))
	parse_error(sym, "Address '%s' already assigned to host '%s'.", sym->buf, h->name);
}

static void add_net(struct sym *sym, radixtree_t *ht, tac_net *net)
{
    struct in6_addr a;
    int cm;
    if (v6_ptoh(&a, &cm, sym->buf))
	parse_error(sym, "Expected an IP address or network in CIDR notation, but got '%s'.", sym->buf);

    radix_add(ht, &a, cm, net);
}

static void parse_file(char *url, radixtree_t *ht, tac_host *host, tac_net *net)
{
    struct sym sym = {.filename = url,.line = 1 };

    if (setjmp(sym.env))
	tac_exit(EX_CONFIG);

    char *buf;
    int bufsize;

    if (cfg_open_and_read(url, &buf, &bufsize)) {
	report_cfg_error(LOG_ERR, ~0, "Couldn't open %s: %s", url, strerror(errno));
	report_cfg_error(LOG_ERR, ~0, "Exiting.");
	exit(EX_NOINPUT);
    }

    sym.tlen = sym.len = bufsize;
    sym.tin = sym.in = buf;

    sym_init(&sym);

    while (sym.code != S_eof) {
	if (host)
	    add_host(&sym, ht, host);
	if (net)
	    add_net(&sym, ht, net);
	sym_get(&sym);
    }

    cfg_close(url, buf, bufsize);
}

static void parse_rewrite(struct sym *sym, tac_realm *r)
{
    tac_rewrite_expr **e;
    tac_rewrite *rewrite = alloca(sizeof(tac_rewrite));

    if (!r->rewrite)
	r->rewrite = RB_tree_new(compare_name, NULL);

    rewrite->name = sym->buf;
    rewrite = RB_lookup(r->rewrite, rewrite);
    if (!rewrite) {
	rewrite = (tac_rewrite *) calloc(1, sizeof(tac_rewrite));
	rewrite->name = strdup(sym->buf);
	rewrite->name_len = strlen(sym->buf);
	RB_insert(r->rewrite, rewrite);
    }

    sym_get(sym);
    if (sym->code == S_equal)
	sym_get(sym);

    e = &rewrite->expr;
    while (*e)
	e = &(*e)->next;

    parse(sym, S_openbra);
    while (sym->code == S_rewrite) {
#ifdef WITH_PCRE2
	int errcode = 0;
	*e = (tac_rewrite_expr *) calloc(1, sizeof(tac_rewrite_expr));
	sym->flag_parse_pcre = 1;
	sym_get(sym);
	if (sym->code == S_slash) {
	    PCRE2_SIZE erroffset;
	    (*e)->code =
		pcre2_compile((PCRE2_SPTR8) sym->buf, PCRE2_ZERO_TERMINATED, PCRE2_MULTILINE | common_data.regex_pcre_flags, &errcode, &erroffset, NULL);
	    if (!(*e)->code) {
		PCRE2_UCHAR buffer[256];
		pcre2_get_error_message(errcode, buffer, sizeof(buffer));
		parse_error(sym, "In PCRE2 expression /%s/ at offset %d: %s", sym->buf, erroffset, buffer);
	    }
	    (*e)->name = strdup(sym->buf);
	    sym_get(sym);
	    (*e)->replacement = (PCRE2_SPTR) strdup(sym->buf);
	    e = &(*e)->next;
	    sym_get(sym);
	}
#else
	parse_error(sym, "You're using a PCREv2-only feature, but this binary wasn't compiled with PCREv2 support.");
#endif
    }
    sym->flag_parse_pcre = 0;
    parse(sym, S_closebra);
}

static void fixup_banner(struct log_item **li, char *file, int line)
{
    while (*li)
	li = &(*li)->next;
    *li = parse_log_format_inline("\"${message}${umessage}\"", file, line);
}

static void parse_host_attr(struct sym *sym, tac_realm *r, tac_host *host)
{
    mem_t *mem = host->mem;
    if (mem)
	switch (sym->code) {
	case S_name:
	    // dynamic hosts support the "name = <device name>" attribute.
	    sym_get(sym);
	    parse(sym, S_equal);
	    host->name = mem_strdup(host->mem, sym->buf);
	    sym_get(sym);
	    host->name_len = strlen(host->name);
	    return;
	case S_parent:
	case S_authentication:
	case S_permit:
	case S_bug:
	case S_pap:
	case S_key:
	case S_anonenable:
	case S_augmented_enable:
	case S_singleconnection:
	case S_debug:
	case S_connection:
	case S_password:
	case S_context:
	case S_session:
	case S_target_realm:
	case S_maxrounds:
	case S_skip:
	case S_welcome:
	case S_reject:
	case S_failed:
	case S_enable:
	case S_motd:
	case S_script:
	case S_message:
	case S_mavis:
#ifdef WITH_DNS
	case S_dns:
#endif
	case S_tag:
	case S_devicetag:
#if defined(WITH_SSL) && !defined(OPENSSL_NO_PSK)
	case S_tls:
#endif
	    break;
	default:
	    parse_error_expect(sym,
			       S_parent, S_authentication, S_permit, S_bug, S_pap, S_key, S_anonenable, S_augmented_enable,
			       S_singleconnection, S_debug, S_connection, S_password, S_context, S_session, S_target_realm,
			       S_maxrounds, S_skip, S_tag, S_devicetag, S_name, S_welcome, S_reject, S_failed, S_enable, S_motd, S_script, S_message, S_mavis,
#ifdef WITH_DNS
			       S_dns,
#endif
#if defined(WITH_SSL) && !defined(OPENSSL_NO_PSK)
			       S_tls,
#endif
			       S_unknown);
	}

    switch (sym->code) {
    case S_mavis:
	sym_get(sym);
	parse(sym, S_backend);
	parse(sym, S_equal);
	host->try_mavis = parse_tristate(sym);
	return;
    case S_host:
    case S_device:
	parse_host(sym, r, host);
	return;
    case S_parent:
	sym_get(sym);
	parse(sym, S_equal);
	host->parent = lookup_host(sym->buf, r);
	if (!host->parent)
	    parse_error(sym, "Host '%s' not found.", sym->buf);
	if (loopcheck_host(host))
	    parse_error(sym, "'%s': circular reference rejected", sym->buf);
	sym_get(sym);
	return;
    case S_authentication:
	sym_get(sym);
	switch (sym->code) {
	case S_fallback:
	    sym_get(sym);
	    parse(sym, S_equal);
	    host->authfallback = parse_tristate(sym);
	    break;
	default:
	    parse_error_expect(sym, S_fallback, S_unknown);
	}
	return;
    case S_permit:
	sym_get(sym);
	parse(sym, S_ifauthenticated);
	parse(sym, S_equal);
	host->authz_if_authc = parse_tristate(sym);
	return;
    case S_bug:
	sym_get(sym);
	parse(sym, S_compatibility);
	parse(sym, S_equal);
	host->bug_compatibility = parse_int(sym);
	return;
    case S_pap:
	sym_get(sym);
	switch (sym->code) {
	case S_password:
	    parse_host_pap_password(sym, host);
	    return;
	default:
	    parse_error_expect(sym, S_password, S_unknown);
	}
    case S_address:
	sym_get(sym);
	if (sym->code == S_file) {
	    glob_t globbuf = { 0 };

	    sym_get(sym);
	    parse(sym, S_equal);

	    globerror_sym = sym;

	    switch (glob(sym->buf, GLOB_ERR | GLOB_NOESCAPE | GLOB_NOMAGIC | GLOB_BRACE, globerror, &globbuf)) {
	    case 0:
		for (int i = 0; i < (int) globbuf.gl_pathc; i++)
		    parse_file(globbuf.gl_pathv[i], r->hosttree, host, NULL);
		break;
#ifdef GLOB_NOMATCH
	    case GLOB_NOMATCH:
		globerror(sym->buf, ENOENT);
		break;
#endif				/* GLOB_NOMATCH */
	    default:
		parse_file(sym->buf, r->hosttree, host, NULL);
		globfree(&globbuf);
	    }
	    sym_get(sym);
	} else {
	    parse(sym, S_equal);
	    do {
		add_host(sym, r->hosttree, host);
		sym_get(sym);
	    }
	    while (parse_comma(sym));
	}
	return;
    case S_key:
    case S_radius_key:
	parse_key(sym, host);
	return;
    case S_motd:
	sym_get(sym);
	parse(sym, S_banner);
	parse(sym, S_equal);
	host->motd = parse_log_format(sym, mem);
	fixup_banner(&host->motd, __FILE__, __LINE__);
	return;
    case S_welcome:
	sym_get(sym);
	parse(sym, S_banner);
	if (sym->code == S_fallback) {
	    sym_get(sym);
	    parse(sym, S_equal);
	    host->welcome_banner_fallback = parse_log_format(sym, mem);
	    fixup_banner(&host->welcome_banner_fallback, __FILE__, __LINE__);
	} else {
	    parse(sym, S_equal);
	    host->welcome_banner = parse_log_format(sym, mem);
	    fixup_banner(&host->welcome_banner, __FILE__, __LINE__);
	}
	return;
    case S_reject:
	sym_get(sym);
	parse(sym, S_banner);
	parse(sym, S_equal);
	host->reject_banner = parse_log_format(sym, mem);
	fixup_banner(&host->reject_banner, __FILE__, __LINE__);
	return;
    case S_failed:
	sym_get(sym);
	parse(sym, S_authentication);
	parse(sym, S_banner);
	parse(sym, S_equal);
	host->authfail_banner = parse_log_format(sym, mem);
	fixup_banner(&host->authfail_banner, __FILE__, __LINE__);
	return;
    case S_enable:
	sym_get(sym);
	if (!host->enable)
	    host->enable = mem_alloc(host->mem, sizeof(struct pwdat *) * (TAC_PLUS_PRIV_LVL_MAX + 1));
	parse_enable(sym, host->mem, host->enable);
	return;
    case S_anonenable:
	sym_get(sym);
	parse(sym, S_equal);
	host->anon_enable = parse_tristate(sym);
	return;
    case S_augmented_enable:
	sym_get(sym);
	parse(sym, S_equal);
	host->augmented_enable = parse_tristate(sym);
	return;
    case S_singleconnection:
	sym_get(sym);
	switch (sym->code) {
	case S_mayclose:
	    sym_get(sym);
	    parse(sym, S_equal);
	    host->cleanup_when_idle = parse_tristate(sym);
	    break;
	case S_equal:
	    sym_get(sym);
	    host->single_connection = parse_tristate(sym);
	    break;
	default:
	    parse_error_expect(sym, S_mayclose, S_equal, S_unknown);
	}
	return;
    case S_debug:
	sym_get(sym);
	parse(sym, S_equal);
	parse_debug(sym, &host->debug);
	return;
    case S_connection:
	sym_get(sym);
	parse(sym, S_timeout);
	parse(sym, S_equal);
	host->tcp_timeout = parse_seconds(sym);
	return;
    case S_password:
	sym_get(sym);
	switch (sym->code) {
	case S_maxattempts:
	    sym_get(sym);
	    parse(sym, S_equal);
	    host->authen_max_attempts = parse_int(sym);
	    return;
	case S_expiry:
	    sym_get(sym);
	    parse(sym, S_warning);
	    parse(sym, S_equal);
	    host->password_expiry_warning = to_seconds(sym);
	    return;
	default:
	    parse_error_expect(sym, S_maxattempts, S_expiry, S_unknown);
	}
    case S_context:
	sym_get(sym);
	parse(sym, S_timeout);
	parse(sym, S_equal);
	host->context_timeout = parse_seconds(sym);
	return;
    case S_session:
	sym_get(sym);
	parse(sym, S_timeout);
	parse(sym, S_equal);
	host->session_timeout = parse_seconds(sym);
	return;
    case S_target_realm:
	sym_get(sym);
	parse(sym, S_equal);
	host->target_realm = lookup_realm(sym->buf, r);
	if (!host->target_realm)
	    parse_error(sym, "Realm '%s' not found.", sym->buf);
	sym_get(sym);
	return;
    case S_script:
	{
	    struct mavis_action **p = &host->action;
	    sym_get(sym);
	    while (*p)
		p = &(*p)->n;
	    *p = tac_script_parse_r(sym, mem, 0, r);
	    return;
	}
    case S_maxrounds:
	sym_get(sym);
	parse(sym, S_equal);
	host->max_rounds = parse_int(sym);
	if (host->max_rounds < 1 || host->max_rounds > 127)
	    parse_error(sym, "Illegal number of rounds (valid range: 1 ... 127)");
	return;
    case S_skip:
	sym_get(sym);
	parse(sym, S_parent_script);
	parse(sym, S_equal);
	host->skip_parent_script = parse_bistate(sym);
	return;
#ifdef WITH_DNS
    case S_dns:
	sym_get(sym);
	switch (sym->code) {
	case S_timeout:
	case S_reverselookup:
	    parse_host_dns(sym, host);
	    return;
	default:
	    parse_error_expect(sym, S_timeout, S_reverselookup, S_unknown);
	}

#endif
    case S_message:
	{
	    enum user_message_enum um = UM_MAX;
	    sym_get(sym);
	    switch (sym->code) {
	    case S_PASSWORD:
		um = UM_PASSWORD;
		break;
	    case S_RESPONSE:
		um = UM_RESPONSE;
		break;
	    case S_PASSWORD_OLD:
		um = UM_PASSWORD_OLD;
		break;
	    case S_PASSWORD_NEW:
		um = UM_PASSWORD_NEW;
		break;
	    case S_PASSWORD_ABORT:
		um = UM_PASSWORD_ABORT;
		break;
	    case S_PASSWORD_AGAIN:
		um = UM_PASSWORD_AGAIN;
		break;
	    case S_PASSWORD_NOMATCH:
		um = UM_PASSWORD_NOMATCH;
		break;
	    case S_PASSWORD_MINREQ:
		um = UM_PASSWORD_MINREQ;
		break;
	    case S_PERMISSION_DENIED:
		um = UM_PERMISSION_DENIED;
		break;
	    case S_ENABLE_PASSWORD:
		um = UM_ENABLE_PASSWORD;
		break;
	    case S_PASSWORD_CHANGE_DIALOG:
		um = UM_PASSWORD_CHANGE_DIALOG;
		break;
	    case S_PASSWORD_CHANGED:
		um = UM_PASSWORD_CHANGED;
		break;
	    case S_BACKEND_FAILED:
		um = UM_BACKEND_FAILED;
		break;
	    case S_CHANGE_PASSWORD:
		um = UM_CHANGE_PASSWORD;
		break;
	    case S_ACCOUNT_EXPIRES:
		um = UM_ACCOUNT_EXPIRES;
		break;
	    case S_PASSWORD_INCORRECT:
		um = UM_PASSWORD_INCORRECT;
		break;
	    case S_RESPONSE_INCORRECT:
		um = UM_RESPONSE_INCORRECT;
		break;
	    case S_USERNAME:
		um = UM_USERNAME;
		break;
	    case S_USER_ACCESS_VERIFICATION:
		um = UM_USER_ACCESS_VERIFICATION;
		break;
	    case S_DENIED_BY_ACL:
		um = UM_DENIED_BY_ACL;
		break;
	    default:
		parse_error_expect(sym, S_PASSWORD, S_RESPONSE, S_PASSWORD_OLD,
				   S_PASSWORD_NEW, S_PASSWORD_ABORT,
				   S_PASSWORD_AGAIN, S_PASSWORD_NOMATCH,
				   S_PASSWORD_MINREQ, S_PERMISSION_DENIED,
				   S_ENABLE_PASSWORD, S_PASSWORD_CHANGE_DIALOG, S_PASSWORD_CHANGED,
				   S_BACKEND_FAILED, S_CHANGE_PASSWORD,
				   S_ACCOUNT_EXPIRES, S_PASSWORD_INCORRECT,
				   S_RESPONSE_INCORRECT, S_USERNAME, S_USER_ACCESS_VERIFICATION, S_DENIED_BY_ACL, S_unknown);
	    }
	    sym_get(sym);
	    parse(sym, S_equal);
	    if (!host->user_messages)
		host->user_messages = mem_alloc(mem, UM_MAX * sizeof(char *));
	    host->user_messages[um] = mem_strdup(mem, sym->buf);
	    sym_get(sym);
	    return;
	}
    case S_tag:
    case S_devicetag:
	{
	    if (!tags_by_name)
		tags_by_name = RB_tree_new(compare_name, NULL);
	    if (!host->tags)
		host->tags = mem_alloc(host->mem, sizeof(tac_tags));
	    sym_get(sym);
	    parse(sym, S_equal);
	    do
		tac_tag_add(host->mem, tac_tag_parse(sym), host->tags);
	    while (parse_comma(sym));
	    return;
	}
#if defined(WITH_SSL) && !defined(OPENSSL_NO_PSK)
    case S_tls:
	sym_get(sym);
	parse(sym, S_psk);
	switch (sym->code) {
	case S_id:
	    sym_get(sym);
	    parse(sym, S_equal);
	    host->tls_psk_id = mem_strdup(host->mem, sym->buf);
	    break;
	case S_key:
	    sym_get(sym);
	    parse(sym, S_equal);
	    parse_tls_psk_key(sym, host);
	    break;
	default:
	    parse_error_expect(sym, S_id, S_key, S_unknown);
	}
	sym_get(sym);
	break;
#endif
    default:
	parse_error_expect(sym, S_host, S_device, S_parent, S_authentication, S_permit,
			   S_bug, S_pap, S_address, S_key, S_motd, S_welcome, S_skip,
			   S_reject, S_enable, S_anonenable, S_augmented_enable,
			   S_singleconnection, S_debug, S_connection, S_context, S_script, S_target_realm,
#if defined(WITH_SSL) && !defined(OPENSSL_NO_PSK)
			   S_tls,
#endif
			   S_mavis, S_unknown);
    }
}

static void parse_host(struct sym *sym, tac_realm *r, tac_host *parent)
{
    tac_host *host, *hp;
    radixtree_t *ht;
    struct dns_forward_mapping *d;

    if (!r->hosttable) {
	r->hosttable = RB_tree_new(compare_name, NULL);
	r->hosttree = radix_new(NULL, NULL);
    }

    sym_get(sym);

    ht = r->hosttree;

    if (sym->code == S_equal)
	sym_get(sym);

    host = new_host(sym, NULL, parent, r, 0);
    if (strchr(host->name, '=')) {	// likely a certificate subject. Normalize.
	for (size_t i = 0; i < host->name_len; i++)
	    host->name[i] = tolower(host->name[i]);
    }
    if ((hp = RB_lookup(r->hosttable, (void *) host)))
	parse_error(sym, "Host '%s' already defined at line %u", sym->buf, hp->line);

    d = dns_lookup_a(r, sym->buf, 0);
    while (d) {
	tac_host *exists;
	if (ht && (exists = radix_add(ht, &d->a, 128, host)))
	    parse_error(sym, "Address '%s' already assigned to host '%d'", sym->buf, exists->name);
	d = d->next;
    }

    parse(sym, S_openbra);

    while (sym->code != S_closebra)
	parse_host_attr(sym, r, host);
    sym_get(sym);
    RB_insert(r->hosttable, host);
}

static void radix_copy_func(struct in6_addr *addr, int mask, void *payload, void *data)
{
    radix_add((radixtree_t *) data, addr, mask, payload);
}

static void parse_net(struct sym *sym, tac_realm *r, tac_net *parent)
{
    tac_net *net = (tac_net *) calloc(1, sizeof(tac_net)), *np;
    struct dns_forward_mapping *d;

    if (!r->nettable)
	r->nettable = RB_tree_new(compare_name, NULL);

    net->line = sym->line;

    sym_get(sym);

    net->name = strdup(sym->buf);
    net->name_len = strlen(sym->buf);
    net->nettree = radix_new(NULL, NULL);
    if ((np = RB_lookup(r->nettable, (void *) net)))
	parse_error(sym, "Net '%s' already defined at line %u", sym->buf, np->line);
    net->line = sym->line;
    net->parent = parent;
    net->res = S_permit;

    d = dns_lookup_a(r, sym->buf, 1);
    while (d) {
	radix_add(net->nettree, &d->a, 128, net);
	d = d->next;
    }

    sym_get(sym);
    parse(sym, S_openbra);

    while (sym->code != S_closebra)
	switch (sym->code) {
	case S_permit:
	case S_deny:
	    net->res = sym->code;
	    continue;
	case S_address:
	    sym_get(sym);
	    if (sym->code == S_file) {
		glob_t globbuf = { 0 };
		sym_get(sym);
		parse(sym, S_equal);

		globerror_sym = sym;

		switch (glob(sym->buf, GLOB_ERR | GLOB_NOESCAPE | GLOB_NOMAGIC | GLOB_BRACE, globerror, &globbuf)) {
		case 0:
		    for (int i = 0; i < (int) globbuf.gl_pathc; i++)
			parse_file(globbuf.gl_pathv[i], net->nettree, NULL, net);
		    break;
#ifdef GLOB_NOMATCH
		case GLOB_NOMATCH:
		    globerror(sym->buf, ENOENT);
		    break;
#endif				/* GLOB_NOMATCH */
		default:
		    parse_file(sym->buf, net->nettree, NULL, net);
		    globfree(&globbuf);
		}
		sym_get(sym);
	    } else {
		parse(sym, S_equal);
		do {
		    add_net(sym, net->nettree, net);
		    sym_get(sym);
		}
		while (parse_comma(sym));
	    }
	    continue;
	case S_net:
	    parse_net(sym, r, net);
	    break;
	case S_parent:
	    sym_get(sym);
	    parse(sym, S_equal);
	    net->parent = lookup_net(sym->buf, r);
	    if (!net->parent)
		parse_error(sym, "Net '%s' not found.", sym->buf);
	    if (loopcheck_net(net))
		parse_error(sym, "'%s': circular reference rejected", sym->buf);
	    sym_get(sym);
	    continue;
	default:
	    parse_error_expect(sym, S_permit, S_deny, S_address, S_net, S_unknown);
	}
    sym_get(sym);
    RB_insert(r->nettable, net);
    if (net->parent)
	radix_walk(net->nettree, radix_copy_func, net->parent->nettree);
}

enum token eval_tac_acl(tac_session *session, struct tac_acl *acl)
{
    if (acl) {
	char *hint = "";
	enum token res = S_unknown;
	struct mavis_action *action = acl->action;
	report(session, LOG_DEBUG, DEBUG_ACL_FLAG, "evaluating ACL %s", acl->name);
	switch ((res = tac_script_eval_r(session, action))) {
	case S_permit:
	case S_deny:
	    report(session, LOG_DEBUG, DEBUG_ACL_FLAG | DEBUG_REGEX_FLAG, "ACL %s: %smatch%s", acl->name, res == S_permit ? "" : "no ", hint);
	    return res;
	default:
	    action = action->n;
	}

	report(session, LOG_DEBUG, DEBUG_ACL_FLAG | DEBUG_REGEX_FLAG, "ACL %s: %smatch%s", acl->name, res == S_permit ? "" : "no ", hint);
    }
    return S_unknown;
}

// acl = <name> [(permit|deny)] { ... }
static void parse_tac_acl(struct sym *sym, tac_realm *realm)
{
    struct tac_acl *a;
    sym_get(sym);

    if (!realm->acltable)
	realm->acltable = RB_tree_new(compare_name, NULL);

    if (sym->code == S_equal)
	sym_get(sym);

    a = tac_acl_lookup(sym->buf, realm);
    if (!a) {
	size_t l = strlen(sym->buf);
	a = calloc(1, sizeof(struct tac_acl));
	a->name = strdup(sym->buf);
	a->name_len = l;
	RB_insert(realm->acltable, a);
    }
    sym_get(sym);

    parse(sym, S_openbra);

    struct mavis_action **p = &a->action;

    while (*p)
	p = &(*p)->n;

    *p = tac_script_parse_r(sym, NULL, 1, realm);

    parse(sym, S_closebra);
}

static void attr_add_single(tac_session *session, char ***v, int *i, char *attr, size_t attr_len)
{
    if (!*v) {
	*v = mem_alloc(session->mem, 0x100 * sizeof(char *));
	*i = 0;
    }
    if (*i < 256) {
	char *sep = attr;
	while (*sep && *sep != '=' && *sep != '*')
	    sep++;
	// auto-numbered attribute support
	if (*sep && (sep - attr > 2)) {
	    char *pd = sep - 2;
	    if (*pd == '%' && (*(pd + 1) == 'd' || *(pd + 1) == 'n')) {
		int d = (*(pd + 1) == 'n') ? 0 : 1;
		size_t len = pd - attr;
		for (int j = 0; j < *i; j++) {
		    if (!strncmp(attr, (*v)[j], len) && isdigit((*v)[j][len])) {
			int k = 0;
			char *t = (*v)[j] + len;
			while (isdigit(*t)) {
			    k *= 10;
			    k += *t++ - '0';
			}
			if (d <= k)
			    d = k + 1;
		    }
		}

		size_t dlen = 1;
		int d_tmp = d / 10;
		while (d_tmp) {
		    dlen++;
		    d_tmp /= 10;
		}
		size_t a_tmp_len = attr_len - 2 + dlen;
		char *a_tmp = alloca(a_tmp_len);
		char *t = a_tmp;
		memcpy(t, attr, pd - attr);
		t += pd - attr;
		size_t dlen_tmp = dlen;
		while (dlen_tmp) {
		    dlen_tmp--;
		    t[dlen_tmp] = (d % 10) + '0';
		    d /= 10;
		}
		t += dlen;
		memcpy(t, sep, attr + attr_len - sep);
		attr = a_tmp;
		attr_len = a_tmp_len;
	    }
	}
	(*v)[(*i)++] = mem_strndup(session->mem, (u_char *) attr, attr_len);
    }
}

static void attr_add_multi(tac_session *session, char ***v, int *i, char *attr, size_t attr_len)
{
    char *a = alloca(attr_len + 1);
    size_t a_len;

    for (a_len = 0; a_len < attr_len && attr[a_len] != '*' && attr[a_len] != '='; a_len++);
    if (a_len == attr_len)
	return;
    a_len++;

    memcpy(a, attr, a_len);
    a[a_len] = 0;

    attr += a_len;
    attr_len -= a_len;

    while (attr_len) {
	size_t j;
	for (j = 0; j < attr_len && attr[j] != '\n'; j++);
	if (j)
	    memcpy(a + a_len, attr, j);
	else
	    a[a_len] = 0;
	attr_add_single(session, v, i, a, a_len + j);
	attr_len -= j;
	attr += j;
	if (attr_len) {
	    attr_len--;
	    attr++;
	}
    }
}

void attr_add(tac_session *session, char ***v, int *i, char *attr, size_t attr_len)
{
    if (attr && attr_len) {
	size_t j;
	for (j = 0; j < attr_len && attr[j] != '\n'; j++);
	if (j == attr_len)
	    attr_add_single(session, v, i, attr, attr_len);
	else
	    attr_add_multi(session, v, i, attr, attr_len);
    }
}

void cfg_init(void)
{
    init_timespec();
    config.mask = 0644;

    struct utsname utsname = { 0 };
    if (uname(&utsname) || !*(utsname.nodename))
	config.hostname = "amnesiac";
    else
	config.hostname = strdup(utsname.nodename);
    config.hostname_len = strlen(config.hostname);
}

int cfg_get_enable(tac_session *session, struct pwdat **p)
{
    int m = 0;
    struct pwdat **d[3];

    if (!session->profile && (S_permit != eval_ruleset(session, session->ctx->realm)))
	return -1;

    if (session->user && session->user->enable)
	d[m++] = session->user->enable;
    if (session->profile && session->profile->enable)
	d[m++] = session->profile->enable;
    if (session->ctx->host->enable)
	d[m++] = session->ctx->host->enable;

    for (int level = session->priv_lvl; level < TAC_PLUS_PRIV_LVL_MAX + 1; level++) {
	for (int i = 0; i < m; i++)
	    if (d[i][level]) {
		*p = d[i][level];
		return 0;
	    }
    }
    return -1;
}

static struct mavis_cond *tac_script_cond_parse_r(struct sym *sym, mem_t *mem, tac_realm *realm)
{
    struct mavis_cond *m, *p = NULL;

    switch (sym->code) {
    case S_leftbra:
	sym_get(sym);
	m = mavis_cond_add(mavis_cond_new(sym, mem, S_or), mem, tac_script_cond_parse_r(sym, mem, realm));
	if (sym->code == S_and)
	    m->type = S_and;
	while (sym->code == S_and || sym->code == S_or) {
	    sym_get(sym);
	    m = mavis_cond_add(m, mem, tac_script_cond_parse_r(sym, mem, realm));
	}
	parse(sym, S_rightbra);
	return m;
    case S_exclmark:
	sym_get(sym);
	m = mavis_cond_add(mavis_cond_new(sym, mem, S_exclmark), mem, tac_script_cond_parse_r(sym, mem, realm));
	return m;
    case S_acl:
	m = mavis_cond_new(sym, mem, S_acl);

	sym_get(sym);
	switch (sym->code) {
	case S_exclmark:
	    p = mavis_cond_add(mavis_cond_new(sym, mem, S_exclmark), mem, m);
	case S_equal:
	    break;
	default:
	    parse_error_expect(sym, S_exclmark, S_equal, S_unknown);
	}
	sym_get(sym);
	parse(sym, S_equal);

	m->u.s.rhs = tac_acl_lookup(sym->buf, realm);

	if (!m->u.s.rhs)
	    parse_error(sym, "ACL '%s' not found", sym->buf);
	sym_get(sym);
	return m;
    case S_time:
	m = mavis_cond_new(sym, mem, S_time);

	sym_get(sym);
	switch (sym->code) {
	case S_exclmark:
	    p = mavis_cond_add(mavis_cond_new(sym, mem, S_exclmark), mem, m);
	case S_equal:
	    break;
	default:
	    parse_error_expect(sym, S_exclmark, S_equal, S_unknown);
	}
	sym_get(sym);
	parse(sym, S_equal);

	m->u.s.rhs = lookup_timespec(sym->buf, realm);
	if (!m->u.s.rhs)
	    parse_error(sym, "Timespec '%s' not found", sym->buf);
	sym_get(sym);
	return m;
    case S_aaa_protocol:
    case S_arg:
    case S_cmd:
    case S_context:
    case S_client:
    case S_clientaddress:
    case S_clientname:
    case S_clientdns:
    case S_nac:
    case S_nas:
    case S_host:
    case S_device:
    case S_deviceaddress:
    case S_devicename:
    case S_devicedns:
    case S_devicetag:
    case S_nasname:
    case S_nacname:
    case S_deviceport:
    case S_port:
    case S_type:
    case S_user:
    case S_user_original:
    case S_usertag:
    case S_member:
    case S_group:
    case S_dn:
    case S_memberof:
    case S_password:
    case S_service:
    case S_protocol:
    case S_authen_action:
    case S_authen_type:
    case S_authen_service:
    case S_authen_method:
    case S_privlvl:
    case S_realm:
    case S_vrf:
    case S_string:
    case S_identity_source:
    case S_server_name:
    case S_server_port:
    case S_server_address:
    case S_radius:
#if defined(WITH_SSL)
    case S_tls_conn_version:
    case S_tls_conn_cipher:
    case S_tls_peer_cert_issuer:
    case S_tls_peer_cert_subject:
    case S_tls_conn_cipher_strength:
    case S_tls_peer_cn:
    case S_tls_psk_identity:
#endif
	m = mavis_cond_new(sym, mem, S_equal);
	m->u.s.token = sym->code;

	if (m->u.s.token == S_radius) {
	    sym_get(sym);
	    parse(sym, S_leftsquarebra);
	    m->type = m->u.s.token;
	    m->u.s.lhs = rad_dict_attr_lookup(sym);
	    m->u.s.lhs_txt = mem_strdup(mem, sym->buf);
	    sym_get(sym);
	    parse(sym, S_rightsquarebra);
	} else if (m->u.s.token == S_arg) {
	    sym_get(sym);
	    parse(sym, S_leftsquarebra);
	    m->u.s.lhs = mem_strdup(mem, sym->buf);
	    sym_get(sym);
	    parse(sym, S_rightsquarebra);
	} else if (m->u.s.token == S_string) {
	    if (!sym->quoted)
		parse_error(sym, "token %s is not known, please put it in double-quotes if you really want to use it", sym->buf);
	    m->u.s.lhs_txt = mem_strdup(mem, sym->buf);
	    m->u.s.lhs = parse_log_format(sym, mem);
	} else
	    sym_get(sym);

	switch (sym->code) {
	case S_exclmark:
	    p = mavis_cond_add(mavis_cond_new(sym, mem, S_exclmark), mem, m);
	case S_equal:
	    break;
	default:
	    parse_error_expect(sym, S_exclmark, S_equal, S_unknown);
	}
	sym_get(sym);
	switch (sym->code) {
	case S_equal:
	    m->type = S_equal;
	    sym_get(sym);
	    if (m->u.s.token == S_group)
		m->u.s.token = S_member;
	    if (m->u.s.token == S_member) {
		tac_group *g = lookup_group(sym->buf, realm);
		if (!g)
		    parse_error(sym, "Group '%s' not found.", sym->buf);
		m->u.s.rhs_txt = mem_strdup(mem, sym->buf);
		sym_get(sym);
		m->type = S_member;
		m->u.s.rhs = g;
		return p ? p : m;
	    }
	    if (m->u.s.token == S_aaa_protocol) {
		m->type = S_aaa_protocol;
		switch (sym->code) {
		case S_radius:
		case S_radius_tcp:
		case S_radsec:
		case S_tacacs:
		case S_tacacss:
		    break;
		default:
		    parse_error_expect(sym, S_radius, S_radsec, S_tacacs, S_tacacss, S_unknown);
		}
		m->u.s.rhs = codestring[sym->code].txt;
		m->u.s.rhs_txt = codestring[sym->code].txt;
		sym_get(sym);
		return p ? p : m;
	    }
	    if (m->u.s.token == S_device || m->u.s.token == S_devicename || m->u.s.token == S_deviceaddress) {
		tac_host *hp = NULL;
		tac_net *np = NULL;
		if (m->u.s.token == S_device || m->u.s.token == S_devicename) {
		    hp = lookup_host(sym->buf, realm);
		    if (!hp)
			np = lookup_net(sym->buf, realm);
		}
		if (hp) {
		    m->type = S_host;
		    m->u.s.rhs = hp;
		    m->u.s.rhs_txt = hp->name;
		} else if (np) {
		    m->type = S_net;
		    m->u.s.rhs = np;
		    m->u.s.rhs_txt = np->name;
		} else if (m->u.s.token == S_device || m->u.s.token == S_deviceaddress) {
		    struct in6_cidr *c = mem_alloc(mem, sizeof(struct in6_cidr));
		    m->u.s.rhs = c;
		    if (v6_ptoh(&c->addr, &c->mask, sym->buf))
			parse_error(sym, "Expected a %san IP address/network in CIDR notation, but got '%s'.",
				    (m->u.s.token == S_device || m->u.s.token == S_devicename) ? "host or net name or " : "", sym->buf);
		    m->type = S_address;
		    m->u.s.rhs_txt = mem_strdup(mem, sym->buf);
		} else
		    parse_error(sym, "Expected a host or net name, but got '%s'.", sym->buf);
		m->u.s.token = S_nas;
		sym_get(sym);
		return p ? p : m;
	    }
	    if (m->u.s.token == S_client || m->u.s.token == S_clientname || m->u.s.token == S_clientaddress) {
		tac_net *np = NULL;
		if (m->u.s.token == S_client || m->u.s.token == S_clientname)
		    np = lookup_net(sym->buf, realm);
		m->u.s.rhs_txt = np ? np->name : mem_strdup(mem, sym->buf);
		if (np) {
		    m->type = S_net;
		    m->u.s.rhs = np;
		} else if (m->u.s.token == S_client || m->u.s.token == S_clientaddress) {
		    struct in6_cidr *c = mem_alloc(mem, sizeof(struct in6_cidr));
		    m->u.s.rhs = c;
		    if (!v6_ptoh(&c->addr, &c->mask, sym->buf)) {
			m->type = S_address;
			sym_get(sym);
			return p ? p : m;
		    }
		}
		if (!m->u.s.rhs)
		    m->u.s.rhs = m->u.s.rhs_txt;
		sym_get(sym);
		return p ? p : m;
	    }
	    if (m->u.s.token == S_nac || m->u.s.token == S_nas || m->u.s.token == S_host) {
		tac_host *hp;
		tac_net *np;
		if (m->u.s.token == S_host) {
		    hp = lookup_host(sym->buf, realm);
		    if (!hp)
			parse_error(sym, "host %s is not known", sym->buf);
		    m->type = S_host;
		    m->u.s.rhs = hp;
		    m->u.s.rhs_txt = hp->name;
		} else if (m->u.s.token == S_nas && (hp = lookup_host(sym->buf, realm))) {
		    m->type = S_host;
		    m->u.s.rhs = hp;
		    m->u.s.rhs_txt = hp->name;
		} else if (m->u.s.token == S_nas && (np = lookup_net(sym->buf, realm))) {
		    m->type = S_net;
		    m->u.s.rhs = np;
		    m->u.s.rhs_txt = np->name;
		} else if (m->u.s.token == S_nac && (np = lookup_net(sym->buf, realm))) {
		    m->type = S_net;
		    m->u.s.rhs = np;
		    m->u.s.rhs_txt = np->name;
		} else {
		    struct in6_cidr *c = mem_alloc(mem, sizeof(struct in6_cidr));
		    m->u.s.rhs = c;
		    if (v6_ptoh(&c->addr, &c->mask, sym->buf))
			parse_error(sym,
				    "Expected a net%s name or an IP address/network in CIDR notation, but got '%s'.",
				    (m->u.s.token == S_nas) ? " or host" : "", sym->buf);
		    m->type = S_address;
		    m->u.s.rhs_txt = mem_strdup(mem, sym->buf);
		}
		sym_get(sym);
		return p ? p : m;
	    }
	    if (m->u.s.token == S_realm) {
		tac_realm *r = lookup_realm(sym->buf, config.default_realm);
		if (!r)
		    parse_error(sym, "Realm '%s' not found", sym->buf);
		m->u.s.rhs = r;
		m->u.s.rhs_txt = r->name;
		m->type = S_realm;
		sym_get(sym);
		return p ? p : m;
	    }
	    if (m->u.s.token == S_devicetag || m->u.s.token == S_usertag) {
		m->type = m->u.s.token;
		if (sym->code == S_devicetag || sym->code == S_usertag) {
		    m->u.s.rhs_token = sym->code;
		    m->u.s.rhs_txt = codestring[sym->code].txt;
		    sym_get(sym);
		} else {
		    tac_tag *tag = tac_tag_parse(sym);
		    m->u.s.rhs = tag;
		    m->u.s.rhs_txt = tag->name;
		    m->u.s.rhs_token = S_string;
		}
		return p ? p : m;
	    }
	    if (m->u.s.token == S_radius) {
		m->type = m->u.s.token;
		struct rad_dict_attr *attr = (struct rad_dict_attr *) m->u.s.lhs;
		if (attr->type == S_integer) {
		    if (isdigit((int) sym->buf[0])) {
			m->u.s.rhs_txt = mem_strdup(mem, sym->buf);
			m->u.s.rhs = (void *) (long) parse_int(sym);
		    } else {	// non-numeric
			struct rad_dict_val *val = rad_dict_val_lookup_by_name(attr, sym->buf);
			if (attr->val && !val)
			    parse_error(sym, "RADIUS value '$s' not found (attribute: %s)", sym->buf, attr->name);
			sym_get(sym);
			m->u.s.rhs_txt = val->name;
			m->u.s.rhs = (void *) (long) val->id;
		    }
		    return p ? p : m;
		}
	    }
	    m->u.s.rhs = mem_strdup(mem, sym->buf);
	    m->u.s.rhs_txt = m->u.s.rhs;
	    sym_get(sym);
	    return p ? p : m;
	case S_tilde:
	    {			//S_tilde
		int errcode = 0;

		if (m->u.s.token == S_clientname)
		    parse_error(sym, "REGEX matching isn't supported for '%s'", codestring[m->u.s.token].txt);

		if (m->u.s.token == S_group)
		    m->u.s.token = S_member;

		m->type = S_regex;
		sym->flag_parse_pcre = 1;
		sym_get(sym);
		m->u.s.rhs_txt = mem_strdup(mem, sym->buf);
		if (sym->code == S_slash) {
#ifdef WITH_PCRE2
		    PCRE2_SIZE erroffset;
		    m->type = S_slash;
		    m->u.s.rhs =
			pcre2_compile((PCRE2_SPTR8) sym->buf,
				      PCRE2_ZERO_TERMINATED, PCRE2_MULTILINE | common_data.regex_pcre_flags, &errcode, &erroffset, NULL);
		    mem_add_free(mem, pcre2_code_free, m->u.s.rhs);
		    if (!m->u.s.rhs) {
			PCRE2_UCHAR buffer[256];
			pcre2_get_error_message(errcode, buffer, sizeof(buffer));
			parse_error(sym, "In PCRE2 expression /%s/ at offset %d: %s", sym->buf, erroffset, buffer);
		    }
		    sym->flag_parse_pcre = 0;
		    sym_get(sym);
		    return p ? p : m;
#else
		    parse_error(sym, "You're using PCRE2 syntax, but this binary wasn't compiled with PCRE2 support.");
#endif
		}
		m->u.s.rhs = mem_alloc(mem, sizeof(regex_t));
		errcode = regcomp((regex_t *) m->u.s.rhs, sym->buf, REG_EXTENDED | REG_NOSUB | REG_NEWLINE | common_data.regex_posix_flags);
		mem_add_free(mem, regfree, m->u.s.rhs);
		if (errcode) {
		    char e[160];
		    regerror(errcode, (regex_t *) m->u.s.rhs, e, sizeof(e));
		    parse_error(sym, "In regular expression '%s': %s", sym->buf, e);
		}
		sym_get(sym);
		return p ? p : m;
	    }
	default:
	    parse_error_expect(sym, S_equal, S_tilde, S_unknown);
	}

    default:
	parse_error_expect(sym, S_leftbra, S_exclmark, S_acl, S_time, S_arg,
			   S_cmd, S_context, S_nac, S_device, S_nas, S_nasname,
			   S_nacname, S_host, S_port, S_user, S_user_original, S_group, S_member, S_memberof,
			   S_devicename, S_deviceaddress, S_devicedns, S_devicetag, S_deviceport,
			   S_client, S_clientname, S_clientdns, S_clientaddress,
			   S_password, S_service, S_protocol, S_authen_action,
			   S_authen_type, S_authen_service, S_authen_method, S_privlvl, S_vrf, S_dn, S_type, S_identity_source,
			   S_server_name, S_server_address, S_server_port, S_usertag, S_aaa_protocol,
#if defined(WITH_SSL)
			   S_tls_conn_version, S_tls_conn_cipher,
			   S_tls_peer_cert_issuer, S_tls_peer_cert_subject, S_tls_conn_cipher_strength, S_tls_peer_cn, S_tls_psk_identity, S_radius,
#endif
			   S_unknown);
    }
    return NULL;
}

static struct mavis_cond *tac_script_cond_parse(struct sym *sym, mem_t *mem, tac_realm *realm)
{
    struct sym *cond_sym = NULL;
    if (sym_normalize_cond_start(sym, mem, &cond_sym)) {
	struct mavis_cond *m = tac_script_cond_parse_r(cond_sym, mem, realm);
	report(NULL, LOG_DEBUG, DEBUG_PARSE_FLAG, "normalized condition: %s", cond_sym->in);
	sym_normalize_cond_end(&cond_sym, mem);
	mavis_cond_optimize(&m, mem);	//FIXME
	return m;
    }
    return tac_script_cond_parse_r(sym, mem, realm);
}

static int tac_script_cond_eval_res(tac_session *session, struct mavis_cond *m, int res)
{
    char *r = res ? "true" : "false";
    switch (m->type) {
    case S_exclmark:
    case S_and:
    case S_or:
	report(session, LOG_DEBUG, DEBUG_ACL_FLAG, " line %u: [%s] => %s", m->line, codestring[m->type].txt, r);
	break;
    default:
	report(session, LOG_DEBUG, DEBUG_ACL_FLAG,
	       " line %u: [%s] %s%s%s '%s' => %s", m->line,
	       codestring[m->u.s.token].txt, m->u.s.lhs_txt ? m->u.s.lhs_txt : "",
	       m->u.s.lhs_txt ? " " : "", codestring[m->type].txt, m->u.s.rhs_txt ? m->u.s.rhs_txt : "", r);
    }

    return res;
}

static int tac_mavis_cond_compare(tac_session *session, struct mavis_cond *m, char *name, size_t name_len)
{
    char *hint = "regex";
    int res = 0;
    if (m->type == S_equal) {
	res = !strcmp((char *) m->u.s.rhs, name);
	hint = "cmp";
    } else if (m->type == S_slash) {
#ifdef WITH_PCRE2
	pcre2_match_data *match_data = pcre2_match_data_create_from_pattern((pcre2_code *) m->u.s.rhs, NULL);
	res = pcre2_match((pcre2_code *) m->u.s.rhs, (PCRE2_SPTR) name, (PCRE2_SIZE) name_len, 0, 0, match_data, NULL);
	pcre2_match_data_free(match_data);
	hint = "pcre2";
#endif
	res = -1 < res;
    } else
	res = !regexec((regex_t *) m->u.s.rhs, name, 0, NULL, 0);
    if (m->u.s.token == S_password && !(session->debug & DEBUG_USERINPUT_FLAG))
	name = "<hidden>";
    report(session, LOG_DEBUG, DEBUG_REGEX_FLAG, " %s: '%s' <=> '%s' = %d", hint, m->u.s.rhs_txt, name, res);
    return res;
}

static int tac_script_cond_eval(tac_session *session, struct mavis_cond *m)
{
    int res = 0;
    char *v = NULL;
    size_t v_len = 0;
    if (!m)
	return 0;
    switch (m->type) {
    case S_exclmark:
	res = !tac_script_cond_eval(session, m->u.m.e[0]);
	return tac_script_cond_eval_res(session, m, res);
    case S_and:
	res = -1;
	for (int i = 0; res && i < m->u.m.n; i++)
	    res = tac_script_cond_eval(session, m->u.m.e[i]);
	return tac_script_cond_eval_res(session, m, res);
    case S_or:
	for (int i = 0; !res && i < m->u.m.n; i++)
	    res = tac_script_cond_eval(session, m->u.m.e[i]);
	return tac_script_cond_eval_res(session, m, res);
    case S_aaa_protocol:
	res = ((char *) m->u.s.rhs == codestring[session->ctx->aaa_protocol].txt);
	return tac_script_cond_eval_res(session, m, res);
    case S_address:
	switch (m->u.s.token) {
	case S_nac:
	    if (session->nac_addr_valid)
		res = v6_contains(&((struct in6_cidr *) (m->u.s.rhs))->addr, ((struct in6_cidr *) (m->u.s.rhs))->mask, &session->nac_address);
	    break;
	case S_nas:
	    res = v6_contains(&((struct in6_cidr *) (m->u.s.rhs))->addr, ((struct in6_cidr *) (m->u.s.rhs))->mask, &session->ctx->device_addr);
	default:
	    ;
	}
	return tac_script_cond_eval_res(session, m, res);
    case S_host:
	{
	    tac_host *h = session->ctx->host;
	    while (!res && h) {
		res = (h == (tac_host *) (m->u.s.rhs));
		h = h->parent;
	    }
	    return tac_script_cond_eval_res(session, m, res);
	}
    case S_net:
	if (m->u.s.token == S_nas) {
	    tac_net *net = (tac_net *) (m->u.s.rhs);
	    res = radix_lookup(net->nettree, &session->ctx->device_addr, NULL) ? -1 : 0;
	} else if (session->nac_addr_valid) {
	    tac_net *net = (tac_net *) (m->u.s.rhs);
	    res = radix_lookup(net->nettree, &session->nac_address, NULL) ? -1 : 0;
	}
	return tac_script_cond_eval_res(session, m, res);
    case S_time:
	res = eval_timespec((struct mavis_timespec *) m->u.s.rhs, NULL);
	return tac_script_cond_eval_res(session, m, res);
    case S_member:
	if (session->user)
	    res = tac_group_check(m->u.s.rhs, session->user->groups, NULL);
	return tac_script_cond_eval_res(session, m, res);
    case S_devicetag:
	{
	    tac_host *h = session->ctx->host;
	    if (m->u.s.rhs_token == S_string) {
		while (!res && h) {
		    res = tac_tag_check(session, m->u.s.rhs, h->tags);
		    h = h->parent;
		}
	    } else if (m->u.s.rhs_token == S_devicetag)
		res = -1;
	    else if (m->u.s.rhs_token == S_usertag && session && session->user) {
		res = tac_tag_list_check(session, h, session->user);
	    }
	    return tac_script_cond_eval_res(session, m, res);
	}
    case S_usertag:
	if (session && session->user) {
	    if (m->u.s.rhs_token == S_string)
		res = tac_tag_check(session, m->u.s.rhs, session->user->tags);
	    else if (m->u.s.rhs_token == S_usertag)
		res = -1;
	    else if (m->u.s.rhs_token == S_devicetag)
		res = tac_tag_list_check(session, session->ctx->host, session->user);
	}
	return tac_script_cond_eval_res(session, m, res);
    case S_acl:
	res = S_permit == eval_tac_acl(session, (struct tac_acl *) m->u.s.rhs);
	return tac_script_cond_eval_res(session, m, res);
    case S_realm:
	{
	    tac_realm *r = session->ctx->realm;
	    while (!res && r) {
		res = (r == (tac_realm *) m->u.s.rhs);
		r = r->parent;
	    }
	    return tac_script_cond_eval_res(session, m, res);
	}
    case S_radius:
	{
	    struct rad_dict_attr *attr = (struct rad_dict_attr *) m->u.s.lhs;
	    if (attr->type == S_integer) {
		int i;
		int id = (int) (long) m->u.s.rhs;
		res = !rad_get(session, attr->dict->id, attr->id, attr->type, &i, NULL) && (i == id);
	    }
	    return tac_script_cond_eval_res(session, m, res);
	}
    case S_equal:
    case S_regex:
    case S_slash:
	switch (m->u.s.token) {
	case S_authen_action:
	    v = session->authen_action;
	    v_len = session->authen_action_len;
	    break;
	case S_authen_type:
	    v = session->authen_type;
	    v_len = session->authen_type_len;
	    break;
	case S_authen_service:
	    v = session->authen_service;
	    v_len = session->authen_service_len;
	    break;
	case S_authen_method:
	    v = session->authen_method;
	    v_len = session->authen_method_len;
	    break;
	case S_privlvl:
	    v = session->privlvl;
	    v_len = session->privlvl_len;
	    break;
	case S_vrf:
	    v = session->ctx->vrf;
	    v_len = session->ctx->vrf_len;
	    break;
#if defined(WITH_SSL)
	case S_tls_conn_version:
	    v = (char *) session->ctx->tls_conn_version;
	    v_len = session->ctx->tls_conn_version_len;
	    break;
	case S_tls_conn_cipher:
	    v = (char *) session->ctx->tls_conn_cipher;
	    v_len = session->ctx->tls_conn_cipher_len;
	    break;
	case S_tls_peer_cert_issuer:
	    v = (char *) session->ctx->tls_peer_cert_issuer;
	    v_len = session->ctx->tls_peer_cert_issuer_len;
	    break;
	case S_tls_peer_cert_subject:
	    v = (char *) session->ctx->tls_peer_cert_subject;
	    v_len = session->ctx->tls_peer_cert_subject_len;
	    break;
	case S_tls_conn_cipher_strength:
	    v = session->ctx->tls_conn_cipher_strength;
	    v_len = session->ctx->tls_conn_cipher_strength_len;
	    break;
	case S_tls_peer_cn:
	    v = session->ctx->tls_peer_cn;
	    v_len = session->ctx->tls_peer_cn_len;
	    break;
	case S_tls_psk_identity:
	    v = session->ctx->tls_psk_identity;
	    v_len = session->ctx->tls_psk_identity_len;
	    break;
#endif
	case S_context:
	    v = tac_script_get_exec_context(session);
	    break;
	case S_cmd:
	    v = session->cmdline;
	    v_len = session->cmdline_len;
	    break;
	case S_nac:
	case S_clientaddress:
	    v = session->nac_addr_ascii;
	    v_len = session->nac_addr_ascii_len;
	    break;
	case S_nas:
	case S_deviceaddress:
	    v = session->ctx->device_addr_ascii;
	    v_len = session->ctx->device_addr_ascii_len;
	    break;
	case S_clientdns:
	case S_nacname:
	    if (session->nac_dns_name && *session->nac_dns_name) {
		v = session->nac_dns_name;
		v_len = session->nac_dns_name_len;
	    }
	    break;
	case S_devicedns:
	case S_nasname:
	    if (session->ctx->device_dns_name && *session->ctx->device_dns_name) {
		v = session->ctx->device_dns_name;
		v_len = session->ctx->device_dns_name_len;
	    }
	    break;
	case S_deviceport:
	case S_port:
	    v = session->port;
	    v_len = session->port_len;
	    break;
	case S_type:
	    v = session->type;
	    v_len = session->type_len;
	    break;
	case S_user:
	    v = session->username;
	    v_len = session->username_len;
	    break;
	case S_user_original:
	    v = session->username_orig;
	    v_len = session->username_orig_len;
	    break;
	case S_password:
	    v = session->password_new ? session->password_new : session->password;
	    break;
	case S_service:
	    v = session->service;
	    v_len = session->service_len;
	    break;
	case S_protocol:
	    v = session->protocol;
	    v_len = session->protocol_len;
	    break;
	case S_dn:
	    if (session->user && session->user->avc && session->user->avc->arr[AV_A_DN])
		v = session->user->avc->arr[AV_A_DN];
	    break;
	case S_identity_source:
	    if (session->user && session->user->avc && session->user->avc->arr[AV_A_IDENTITY_SOURCE])
		v = session->user->avc->arr[AV_A_IDENTITY_SOURCE];
	    break;
	case S_server_name:
	    v = config.hostname;
	    v_len = config.hostname_len;
	    break;
	case S_server_port:
	    v = session->ctx->server_port_ascii;
	    v_len = session->ctx->server_port_ascii_len;
	    break;
	case S_server_address:
	    v = session->ctx->server_addr_ascii;
	    v_len = session->ctx->server_addr_ascii_len;
	    break;
	case S_string:
	    v = eval_log_format(session, session->ctx, NULL, (struct log_item *) m->u.s.lhs, io_now.tv_sec, &v_len);
	    break;
	case S_member:
	    if (session->user)
		res = tac_group_regex_check(session, m, session->user->groups, NULL);
	    return tac_script_cond_eval_res(session, m, res);
	case S_devicetag:
	    {
		tac_host *h = session->ctx->host;
		while (!res && h) {
		    res = tac_tag_regex_check(session, m, h->tags);
		    h = h->parent;
		}
		return tac_script_cond_eval_res(session, m, res);
	    }
	case S_devicename:
	case S_host:
	    {
		tac_host *h = session->ctx->host;
		while (!res && h) {
		    res = tac_mavis_cond_compare(session, m, h->name, h->name_len);
		    h = h->parent;
		}
		return tac_script_cond_eval_res(session, m, res);
	    }
	case S_realm:
	    {
		tac_realm *r = session->ctx->realm;
		while (!res && r) {
		    res = tac_mavis_cond_compare(session, m, r->name, r->name_len);
		    r = r->parent;
		}
		return tac_script_cond_eval_res(session, m, res);
	    }
	case S_memberof:
	    if (session->user && session->user->avc && session->user->avc->arr[AV_A_MEMBEROF]) {
		size_t l = strlen(session->user->avc->arr[AV_A_MEMBEROF]) + 1;
		v = alloca(l);
		memcpy(v, session->user->avc->arr[AV_A_MEMBEROF], l);
		while (*v) {
		    char *e;
		    if (*v != '"') {
			report(session, LOG_DEBUG, DEBUG_ACL_FLAG,
			       " memberof attribute '%s' is malformed (missing '\"')", session->user->avc->arr[AV_A_MEMBEROF]);
			return tac_script_cond_eval_res(session, m, 0);
		    }
		    v++;
		    for (e = v; *e && *e != '"'; e++);
		    if (*e != '"') {
			report(session, LOG_DEBUG, DEBUG_ACL_FLAG,
			       " memberof attribute '%s' is malformed (missing '\"')", session->user->avc->arr[AV_A_MEMBEROF]);
			return tac_script_cond_eval_res(session, m, 0);
		    }
		    *e++ = 0;
		    // perform checks
		    res = tac_mavis_cond_compare(session, m, v, strlen(v));
		    if (res)
			return tac_script_cond_eval_res(session, m, res);
		    v = e;
		    if (!*v)
			return tac_script_cond_eval_res(session, m, 0);
		    if (*v != ',') {
			report(session, LOG_DEBUG, DEBUG_ACL_FLAG,
			       " memberof attribute '%s' is malformed (expected a ',')", session->user->avc->arr[AV_A_MEMBEROF]);
			return tac_script_cond_eval_res(session, m, 0);
		    }
		    v++;
		}
	    }
	    return 0;
	case S_radius:
	    if (session->radius_data) {
		struct rad_dict_attr *attr = (struct rad_dict_attr *) m->u.s.lhs;
		if (attr->type == S_integer) {
		    int i;
		    int id = (int) (long) m->u.s.rhs;
		    int res = !rad_get(session, attr->dict->id, attr->id, S_integer, &i, NULL) && (i == id);
		    return tac_script_cond_eval_res(session, m, res);
		}
		if (attr->type == S_ipaddr) {
		    char buf[256];
		    sockaddr_union from = { 0 };
		    from.sin.sin_family = AF_INET;
		    if (!rad_get(session, attr->dict->id, attr->id, S_ipaddr, &from.sin.sin_addr, NULL)
			&& su_ntop(&from, buf, sizeof(buf)))
			v = mem_strdup(session->mem, buf);
		} else if (attr->type == S_ipv6addr) {
		    char buf[256];
		    sockaddr_union from = { 0 };
		    from.sin.sin_family = AF_INET6;
		    if (!rad_get(session, attr->dict->id, attr->id, S_ipv6addr, &from.sin6.sin6_addr, NULL)
			&& su_ntop(&from, buf, sizeof(buf)))
			v = mem_strdup(session->mem, buf);
		    if (!res)
			v = mem_strdup(session->mem, su_ntop(&from, buf, sizeof(buf)) ? buf : "<unknown>");
		} else if (attr->type == S_string_keyword) {
		    rad_get(session, attr->dict->id, attr->id, S_string_keyword, &v, &v_len);
		}
	    }
	    break;
	case S_arg:
	    if (session->argp) {
		u_char *arg_len = session->arg_len;
		u_char *argp = session->argp;
		for (u_char arg_cnt = session->arg_cnt; arg_cnt; arg_cnt--, arg_len++) {
		    size_t len = strlen(m->u.s.lhs);
		    size_t l;
		    char *s = (char *) argp;
		    l = (size_t) *arg_len;
		    if ((l > len) && !strncmp(s, m->u.s.lhs, len)
			&& (*(argp + len) == '=' || *(argp + len) == '*')) {
			v = mem_strndup(session->mem, argp + len + 1, l - len - 1);
			break;
		    }
		    argp += (size_t) *arg_len;
		}
	    }
	    break;
	default:;
	}
	if (!v)
	    return 0;
	if (!v_len)
	    v_len = strlen(v);
	res = tac_mavis_cond_compare(session, m, v, v_len);
	return tac_script_cond_eval_res(session, m, res);
    default:;
    }
    return 0;
}

#ifdef WITH_PCRE2
void tac_rewrite_user(tac_session *, tac_rewrite *);
#endif

struct rad_dict_attr;

union rad_action_union {
    int i;
    u_int ipv4[4];
    u_int ipv6[16];
    char *s;
};

struct rad_action {
    struct rad_dict_attr *attr;
    union rad_action_union u;
    struct log_item *li;	// overrides u if set
    struct tac_action *next;
};

static void rad_attr_add(tac_session *session, struct rad_action *a, union rad_action_union *u, char *code, unsigned int line)
{
    if (!session->radius_data)
	return;
    size_t data_len = session->radius_data->data_len;
    size_t data_len_orig = data_len;
    u_char *data = session->radius_data->data + data_len;
    u_char *data_orig = data;
    u_char *data_end = session->radius_data->data + sizeof(session->radius_data->data);

    void *val = NULL;
    size_t val_len = 0;
    switch (a->attr->type) {
    case S_string_keyword:
	val = u->s;
	val_len = strlen(u->s);
	break;
    case S_integer:
	val = &u->i;
	val_len = 4;
	break;
    case S_ipaddr:
	val = &u->ipv4;
	val_len = 4;
	break;
    case S_ipv6addr:
	val = &u->ipv6;
	val_len = 16;
	break;
    default:			// just skip, likely OCTETS or VSA
	return;
    }

    int len = 2 + val_len;
    if (a->attr->dict->id > -1)
	len += 6;
    if (data + len >= data_end || len > 255)
	return;

    if (a->attr->dict->id > -1) {
	*data++ = RADIUS_A_VENDOR_SPECIFIC;
	*data++ = len;
	*data++ = (u_char) (0xff & (a->attr->dict->id >> 24));
	*data++ = (u_char) (0xff & (a->attr->dict->id >> 16));
	*data++ = (u_char) (0xff & (a->attr->dict->id >> 8));
	*data++ = (u_char) (0xff & (a->attr->dict->id >> 0));
	data_len += 6;
	len = 2 + val_len;
    }

    *data++ = a->attr->id;
    *data++ = 2 + val_len;
    data_len += 2;
    memcpy(data, val, val_len);
    data += val_len;
    data_len += val_len;
    session->radius_data->data_len = data_len;

    char *buf = NULL;
    size_t buf_len = 0;
    rad_attr_val_dump(session->mem, data_orig, data_len - data_len_orig, &buf, &buf_len, NULL, NULL, 0);
    report(session, LOG_DEBUG, DEBUG_ACL_FLAG, " line %u: [%s] set '%s'", line, code, buf ? buf : "<empty>");
}

enum token tac_script_eval_r(tac_session *session, struct mavis_action *m)
{
    enum token r;
    char *v;
    if (!m)
	return S_unknown;
    switch (m->code) {
    case S_return:
    case S_permit:
    case S_deny:
	report(session, LOG_DEBUG, DEBUG_ACL_FLAG, " line %u: [%s]", m->line, codestring[m->code].txt);
	return m->code;
    case S_context:
	tac_script_set_exec_context(session, m->b.v);
	report(session, LOG_DEBUG, DEBUG_ACL_FLAG, " line %u: [%s]", m->line, codestring[m->code].txt);
	break;
    case S_message:
	session->message = eval_log_format(session, session->ctx, NULL, (struct log_item *) m->b.v, io_now.tv_sec, &session->message_len);
	report(session, LOG_DEBUG, DEBUG_ACL_FLAG, " line %u: [%s] '%s'", m->line, codestring[m->code].txt, session->message ? session->message : "");
	break;
#ifdef WITH_PCRE2
    case S_rewrite:
	tac_rewrite_user(session, (tac_rewrite *) m->b.v);
	report(session, LOG_DEBUG, DEBUG_ACL_FLAG, " line %u: [%s]", m->line, codestring[m->code].txt);
	break;
#endif
    case S_label:
	session->label = eval_log_format(session, session->ctx, NULL, (struct log_item *) m->b.v, io_now.tv_sec, &session->label_len);
	report(session, LOG_DEBUG, DEBUG_ACL_FLAG, " line %u: [%s] '%s'", m->line, codestring[m->code].txt, session->label ? session->label : "");
	break;
    case S_profile:
	session->profile = (tac_profile *) (m->b.v);
	report(session, LOG_DEBUG, DEBUG_ACL_FLAG, " line %u: [%s] '%s'",
	       m->line, codestring[m->code].txt, (session->profile && session->profile->name) ? session->profile->name : "");
	break;
    case S_attr:
	session->attr_dflt = (enum token) (long) (m->b.v);
	report(session, LOG_DEBUG, DEBUG_ACL_FLAG, " line %u: [%s] '%s'", m->line, codestring[m->code].txt, codestring[session->attr_dflt].txt);
	break;
    case S_radius:
	{
	    struct rad_action *a = (struct rad_action *) m->b.v;
	    union rad_action_union u;
	    memcpy(&u, &a->u, sizeof(u));
	    if (a->li) {
		char *s = eval_log_format(session, session->ctx, NULL, a->li, io_now.tv_sec, NULL);
		switch (a->attr->type) {
		case S_integer:
		    u.i = htonl(atoi(s));
		    break;
		case S_ipaddr:
		    inet_pton(AF_INET, s, &u.ipv4);
		    break;
		case S_ipv6addr:
		    inet_pton(AF_INET6, s, &u.ipv6);
		    break;
		case S_string_keyword:
		    u.s = s;
		    break;
		default:	// unsupported type
		    break;
		}
	    }
	    rad_attr_add(session, a, &u, codestring[m->code].txt, m->line);
	    break;
	}
    case S_add:
    case S_set:
    case S_optional:
	session->eval_log_raw = 1;
	v = eval_log_format(session, session->ctx, NULL, (struct log_item *) m->b.v, io_now.tv_sec, NULL);
	session->eval_log_raw = 0;
	if (m->code == S_set)
	    attr_add(session, &session->attrs_m, &session->cnt_m, v, strlen(v));
	else if (m->code == S_add)
	    attr_add(session, &session->attrs_a, &session->cnt_a, v, strlen(v));
	else			// S_optional
	    attr_add(session, &session->attrs_o, &session->cnt_o, v, strlen(v));
	report(session, LOG_DEBUG, DEBUG_ACL_FLAG, " line %u: [%s] '%s'", m->line, codestring[m->code].txt, v ? v : "");
	break;
    case S_if:
	if (tac_script_cond_eval(session, m->a.c)) {
	    r = tac_script_eval_r(session, m->b.a);
	    if (r != S_unknown)
		return r;
	} else if (m->c.a) {
	    report(session, LOG_DEBUG, DEBUG_ACL_FLAG, " line %u: [%s]", m->line, codestring[S_else].txt);
	    r = tac_script_eval_r(session, m->c.a);
	    if (r != S_unknown)
		return r;
	}
	break;
    default:
	return S_unknown;
    }
    return m->n ? tac_script_eval_r(session, m->n) : S_unknown;
}

struct rad_action *new_rad_action(mem_t *mem, struct rad_dict_attr *attr, union rad_action_union *u, struct log_item *li)
{
    struct rad_action *a = mem_alloc(mem, sizeof(struct rad_action));
    a->attr = attr;
    a->li = li;
    memcpy(&a->u, u, sizeof(a->u));
    return a;
}

static struct mavis_action *tac_script_parse_r(struct sym *sym, mem_t *mem, int section, tac_realm *realm)
{
    struct mavis_action *m = NULL;
    char *sep = "=";
    char buf[0x10000];

    switch (sym->code) {
    case S_closebra:
	return m;
    case S_openbra:
	sym_get(sym);
	m = tac_script_parse_r(sym, mem, 1, realm);
	parse(sym, S_closebra);
	break;
    case S_return:
    case S_permit:
    case S_deny:
	m = mavis_action_new(sym, mem);
	break;
    case S_profile:
	m = mavis_action_new(sym, mem);
	parse(sym, S_equal);
	m->b.v = (char *) lookup_profile(sym->buf, realm);
	if (!m->b.v)
	    parse_error(sym, "Profile '%s' not found.", sym->buf);
	sym_get(sym);
	break;
    case S_context:
	m = mavis_action_new(sym, mem);
	parse(sym, S_equal);
	m->b.v = mem_strdup(mem, sym->buf);
	sym_get(sym);
	break;
    case S_message:
	m = mavis_action_new(sym, mem);
	parse(sym, S_equal);
	m->b.v = (char *) parse_log_format(sym, mem);
	break;
#ifdef WITH_PCRE2
    case S_rewrite:
	m = mavis_action_new(sym, mem);
	parse(sym, S_user);
	parse(sym, S_equal);
	m->b.v = (char *) lookup_rewrite(sym->buf, realm);
	if (!m->b.v)
	    parse_error(sym, "Rewrite '%s' not found.", sym->buf);
	sym_get(sym);
	break;
#endif
    case S_label:
	m = mavis_action_new(sym, mem);
	parse(sym, S_equal);
	m->b.v = (char *) parse_log_format(sym, mem);
	break;
    case S_attr:
	sym_get(sym);
	parse(sym, S_default);
	m = mavis_action_new(sym, mem);
	m->b.v = (char *) (keycode(sym->buf));
	sym_get(sym);
	break;
    case S_if:
	m = mavis_action_new(sym, mem);
	m->a.c = tac_script_cond_parse(sym, mem, realm);
	m->b.a = tac_script_parse_r(sym, mem, 0, realm);
	if (sym->code == S_else) {
	    sym_get(sym);
	    m->c.a = tac_script_parse_r(sym, mem, 0, realm);
	}
	break;
    case S_add:
    case S_optional:
	sep = "*";
    case S_set:
	m = mavis_action_new(sym, mem);
	if (sym->code == S_radius) {
	    m->code = sym->code;
	    sym_get(sym);
	    parse(sym, S_leftsquarebra);
	    struct rad_dict_attr *attr = rad_dict_attr_lookup(sym);
	    if (!attr)
		parse_error(sym, "RADIUS attribute %s not recognized", sym->buf);
	    sym_get(sym);
	    parse(sym, S_rightsquarebra);
	    parse(sym, S_equal);
	    struct rad_dict_val *val = NULL;
	    union rad_action_union u;
	    if (attr->val && attr->type == S_integer && !isdigit(*sym->buf)) {
		val = rad_dict_val_lookup_by_name(attr, sym->buf);
		if (!val)
		    parse_error(sym, "RADIUS value '$s' not found (attribute: %s)", sym->buf, attr->name);
		u.i = htonl(val->id);
		m->b.v = (void *) new_rad_action(mem, attr, &u, NULL);
	    } else if (attr->type == S_integer) {
		u.i = htonl(parse_int(sym));
		m->b.v = (void *) new_rad_action(mem, attr, &u, NULL);
	    } else if (attr->type == S_ipaddr) {
		if (1 == inet_pton(AF_INET, sym->buf, &u.ipv4))
		    m->b.v = (void *) new_rad_action(mem, attr, &u, NULL);
	    } else if (attr->type == S_ipv6addr) {
		if (1 == inet_pton(AF_INET6, sym->buf, &u.ipv6))
		    m->b.v = (void *) new_rad_action(mem, attr, &u, NULL);
	    }
	    if (!m->b.v)
		m->b.v = (void *) new_rad_action(mem, attr, &u, parse_log_format(sym, mem));
	    break;
	}
	snprintf(buf, sizeof(buf), "\"%s%s\"", sym->buf, sep);
	sym_get(sym);
	m->b.v = (char *) parse_log_format_inline(buf, sym->filename, sym->line);
	parse(sym, S_equal);
	((struct log_item *) m->b.v)->next = parse_log_format(sym, mem);
	break;
    default:
	parse_error_expect(sym, S_openbra, S_closebra, S_return, S_permit, S_deny, S_context, S_message, S_if, S_unknown);
    }
    if (section && sym->code != S_closebra && sym->code != S_eof)
	m->n = tac_script_parse_r(sym, mem, section, realm);
    return m;
}

#ifdef WITH_PCRE2
void tac_rewrite_user(tac_session *session, tac_rewrite *rewrite)
{
    if (!session->username_orig) {
	session->username_orig = session->username;
	session->username_orig_len = session->username_len;
    }
    if (!session->username_rewritten) {
	tac_rewrite_expr *e = rewrite->expr;

	if (e) {
	    for (int rc = -1; e && rc < 1; e = e->next) {
		PCRE2_SPTR replacement = e->replacement;
		PCRE2_UCHAR outbuf[1024];
		PCRE2_SIZE outlen = sizeof(outbuf);
		pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(e->code, NULL);
		rc = pcre2_substitute(e->code, (PCRE2_SPTR8) session->username,
				      PCRE2_ZERO_TERMINATED, 0,
				      PCRE2_SUBSTITUTE_EXTENDED, match_data, NULL, replacement, PCRE2_ZERO_TERMINATED, outbuf, &outlen);
		pcre2_match_data_free(match_data);
		report(session, LOG_DEBUG, DEBUG_REGEX_FLAG, "pcre2: '%s' <=> '%s' = %d", e->name, session->username, rc);
		if (rc > 0) {
		    session->username = mem_strndup(session->mem, outbuf, outlen);
		    session->username_len = outlen;
		    session->username_rewritten = strcmp(session->username_orig, session->username) ? 1 : 0;
		    report(session, LOG_DEBUG, DEBUG_REGEX_FLAG, "pcre2: setting username to '%s'", session->username);
		}
	    }
	}
    }
}
#endif

static tac_group *lookup_group(char *name, tac_realm *r)
{
    tac_group g = {.name = name,.name_len = strlen(name) };

    while (r) {
	if (r->groups_by_name) {
	    tac_group *res;
	    if ((res = RB_lookup(r->groups_by_name, &g)))
		return res;
	}
	r = r->parent;
    }
    return 0;
}

mavis_ctx *lookup_mcx(tac_realm *r)
{
    while (r) {
	if (r->mcx)
	    return r->mcx;
	r = r->parent;
    }
    return NULL;
}

/* add name to tree, return id (globally unique) */
static tac_group *tac_group_new(struct sym *sym, char *name, tac_realm *r)
{
    if (!r->groups_by_name)
	r->groups_by_name = RB_tree_new(compare_name, NULL);

    tac_group *gp, g = {.name = name,.name_len = strlen(name) };
    rb_node_t *rbn = RB_search(r->groups_by_name, &g);
    if (rbn) {
	gp = RB_payload(rbn, tac_group *);
	parse_error(sym, "Group %s already defined at line %u", sym->buf, gp->line);
    }
    gp = calloc(1, sizeof(tac_group));
    gp->name = strdup(name);
    gp->name_len = strlen(name);
    RB_insert(r->groups_by_name, gp);

    return gp;
}

/* add id to groups struct */
static int tac_group_add(tac_group *add, tac_groups *g, mem_t *mem)
{
    if (g->count == g->allocated) {
	g->allocated += 32;
	g->groups = (tac_group **) mem_realloc(mem, g->groups, g->allocated * sizeof(tac_group));
    }
    g->groups[g->count] = add;
    g->count++;
    return 0;
}

static int tac_group_check(tac_group *g, tac_groups *gids, tac_group *parent)
{
    if (gids) {
	for (u_int i = 0; i < gids->count; i++) {
	    tac_group *a = gids->groups[i];
	    if ((g == a)
		|| tac_group_check(g, a->groups, a->parent)
		|| (a->parent && (g == a->parent)))
		return -1;
	}
    }
    if (parent)
	return (g == parent) || tac_group_check(g, parent->groups, parent->parent);
    return 0;
}

static int tac_group_regex_check(tac_session *session, struct mavis_cond *m, tac_groups *gids, tac_group *parent)
{
    if (gids) {
	for (u_int i = 0; i < gids->count; i++) {
	    tac_group *a = gids->groups[i];
	    if (tac_mavis_cond_compare(session, m, a->name, a->name_len)
		|| tac_group_regex_check(session, m, a->groups, a->parent)
		|| (a->parent && tac_mavis_cond_compare(session, m, a->parent->name, a->parent->name_len)))
		return -1;
	}
    }
    if (parent)
	return tac_mavis_cond_compare(session, m, parent->name, parent->name_len)
	    || tac_group_regex_check(session, m, parent->groups, parent->parent);
    return 0;
}

/* add id to tags struct */
static int tac_tag_add(mem_t *mem, tac_tag *add, tac_tags *g)
{
    if (g->count == g->allocated) {
	g->allocated += 32;
	g->tags = (tac_tag **) mem_realloc(mem, g->tags, g->allocated * sizeof(tac_tag));
    }
    g->tags[g->count] = add;
    g->count++;
    return 0;
}

static tac_tag *tac_tag_parse(struct sym *sym)
{
    tac_tag t = {.name = sym->buf,.name_len = strlen(sym->buf) };
    if (!tags_by_name)
	tags_by_name = RB_tree_new(compare_name, NULL);
    tac_tag *tag = RB_lookup(tags_by_name, &t);
    if (!tag) {
	tag = calloc(1, sizeof(tac_tag));
	tag->name = strdup(sym->buf);
	tag->name_len = strlen(sym->buf);
	RB_insert(tags_by_name, tag);
    }
    sym_get(sym);
    return tag;
}

static int tac_tag_check(tac_session *session, tac_tag *tag, tac_tags *tags)
{
    if (tags)
	for (u_int i = 0; i < tags->count; i++)
	    if (tag == tags->tags[i]) {
		report(session, LOG_DEBUG, DEBUG_ACL_FLAG, " tag %s matched", tag->name);
		return -1;
	    }
    return 0;
}

static int tac_tag_list_check(tac_session *session, tac_host *h, tac_user *u)
{
    for (; h; h = h->parent)
	if (h->tags && u->tags)
	    for (u_int i = 0; i < u->tags->count; i++)
		if (tac_tag_check(session, u->tags->tags[i], h->tags))
		    return -1;
    return 0;
}

static int tac_tag_regex_check(tac_session *session, struct mavis_cond *m, tac_tags *tags)
{
    if (tags) {
	for (u_int i = 0; i < tags->count; i++) {
	    tac_tag *a = tags->tags[i];
	    if (tac_mavis_cond_compare(session, m, a->name, a->name_len)) {
		report(session, LOG_DEBUG, DEBUG_ACL_FLAG, " tag %s matched", a->name);
		return -1;
	    }
	}
    }
    return 0;
}

#ifdef WITH_SSL
#ifndef OPENSSL_NO_PSK
static int cfg_get_tls_psk(struct context *ctx, char *identity, u_char **key, size_t *keylen)
{
    char *t = identity;
    // host may have key set:
    if (ctx->host->tls_psk_id && !strcmp(identity, ctx->host->tls_psk_id)
	&& ctx->host->tls_psk_key_len) {
	*key = ctx->host->tls_psk_key;
	*keylen = ctx->host->tls_psk_key_len;
	return 0;
    }

    // no key set for host, possibly because host is a parent and/or the NAC has
    // a dynamic IP. Try to map identity to hostname
    while (t) {
	tac_host *h = lookup_host(t, ctx->realm);
	if (h) {
	    complete_host(h);
	    if (h->tls_psk_key_len) {
		ctx->host = h;
		*key = h->tls_psk_key;
		*keylen = h->tls_psk_key_len;
		return 0;
	    }
	}
	t = strchr(t, ',');
	if (t)
	    t++;
    }

    return -1;
}

static int psk_find_session_cb(SSL *ssl, const unsigned char *identity, size_t identity_len, SSL_SESSION **sess)
{
    SSL_SESSION *nsession = NULL;
    const SSL_CIPHER *cipher = NULL;
    u_char *key;
    size_t key_len;
    struct context *ctx;

    // FIXME -- use SSL_CTX_get_app_data instead of SSL_get_fd/io_get_ctx?

    int fd = SSL_get_fd(ssl);

    if (fd < -1) {
	report(NULL, LOG_ERR, ~0, "%s:%d SSL_get_fd() = %d", __FILE__, __LINE__, fd);
	return 0;
    }

    ctx = io_get_ctx(common_data.io, fd);
    if (!ctx) {
	report(NULL, LOG_ERR, ~0, "%s:%d io_get_ctx()", __FILE__, __LINE__);
	return 0;
    }

    if (strlen((char *) identity) != identity_len) {
	report(NULL, LOG_ERR, ~0, "%s:%d identity length mismatch (got=%lu expected=%lu)", __FILE__, __LINE__, strlen((char *) identity), identity_len);
	return 0;
    }

    if (cfg_get_tls_psk(ctx, (char *) identity, &key, &key_len)) {
	report(NULL, LOG_ERR, ~0, "%s:%d psk not found", __FILE__, __LINE__);
	return 0;
    }

    // FIXME Use PSK session file?

    // Constants from https://www.iana.org/assignments/tls-parameters/tls-parameters.xhtml
    // and RFC8446, 8.4.
    // FIXME. There's probably a way to map some standard string to the iana values, somewhere.
    const unsigned char TLS_AES_128_GCM_SHA256[] = { 0x13, 0x01 };	// that's what s_client uses
    // const unsigned char TLS_AES_256_GCM_SHA384[] = { 0x13, 0x02 };
    // const unsigned char TLS_CHACHA20_POLY1305_SHA256[] = { 0x13,0x03 };
    // const unsigned char TLS_AES_128_CCM_SHA256[] = { 0x13,0x04 };
    // const unsigned char TLS_AES_128_CCM_8_SHA256[] = { 0x13,0x05 };

    cipher = SSL_CIPHER_find(ssl, TLS_AES_128_GCM_SHA256);
    if (!cipher) {
	report(NULL, LOG_ERR, ~0, "%s:%d SSL_CIPHER_find() failed", __FILE__, __LINE__);
	return 0;
    }

    nsession = SSL_SESSION_new();
    if (!nsession) {
	report(NULL, LOG_ERR, ~0, "%s:%d SSL_SESSION_new() failed", __FILE__, __LINE__);
	return 0;
    }

    if (!SSL_SESSION_set1_master_key(nsession, key, key_len)) {
	report(NULL, LOG_ERR, ~0, "%s:%d SSL_SESSION_set1_master_key() failed", __FILE__, __LINE__);
	SSL_SESSION_free(nsession);
	return 0;
    }

    if (!SSL_SESSION_set_cipher(nsession, cipher)) {
	report(NULL, LOG_ERR, ~0, "%s:%d SSL_SESSION_set_cipher() failed", __FILE__, __LINE__);
	SSL_SESSION_free(nsession);
	return 0;
    }

    //if (!SSL_SESSION_set_protocol_version(nsession, TLS1_3_VERSION)) {
    if (!SSL_SESSION_set_protocol_version(nsession, SSL_version(ssl))) {
	report(NULL, LOG_ERR, ~0, "%s:%d SSL_SESSION_set_protocol_version() failed", __FILE__, __LINE__);
	SSL_SESSION_free(nsession);
	return 0;
    }

    *sess = nsession;

    ctx->tls_psk_identity = mem_strdup(ctx->mem, (char *) identity);
    ctx->tls_psk_identity_len = strlen((char *) identity);

    return 1;
}
#endif

static int ssl_pem_phrase_cb(char *buf, int size, int rwflag __attribute__((unused)), void *userdata)
{
    int i = (int) strlen((char *) userdata);

    if (i >= size) {
	report(NULL, LOG_ERR, ~0, "ssl_pem_phrase_cb");
	return 0;
    }
    strcpy(buf, (char *) userdata);
    return i;
}

static SSL_CTX *ssl_init(char *cert_file, char *key_file, char *pem_phrase, char *ciphers)
{
    SSL_CTX *ctx = SSL_CTX_new(TLS_server_method());
    if (!ctx) {
	report(NULL, LOG_ERR, ~0, "SSL_CTX_new");
    } else {
	if (!SSL_CTX_set_min_proto_version(ctx, TLS1_3_VERSION))
	    report(NULL, LOG_ERR, ~0, "SSL_CTX_set_min_proto_version");
	if (ciphers && !SSL_CTX_set_cipher_list(ctx, ciphers))
	    report(NULL, LOG_ERR, ~0, "SSL_CTX_set_cipher_list");
	if (pem_phrase) {
	    SSL_CTX_set_default_passwd_cb(ctx, ssl_pem_phrase_cb);
	    SSL_CTX_set_default_passwd_cb_userdata(ctx, pem_phrase);
	}
	if (cert_file && !SSL_CTX_use_certificate_chain_file(ctx, cert_file))
	    report(NULL, LOG_ERR, ~0, "SSL_CTX_use_certificate_chain_file");
	if ((key_file || cert_file)
	    && !SSL_CTX_use_PrivateKey_file(ctx, key_file ? key_file : cert_file, SSL_FILETYPE_PEM))
	    report(NULL, LOG_ERR, ~0, "SSL_CTX_use_PrivateKey_file");
	if ((key_file || cert_file) && !SSL_CTX_check_private_key(ctx))
	    report(NULL, LOG_ERR, ~0, "SSL_CTX_check_private_key");
	SSL_CTX_set_session_cache_mode(ctx, SSL_SESS_CACHE_OFF);
    }
    return ctx;
}
#endif
