/*
 * QUOTA    An implementation of the diskquota system for the LINUX operating
 *          system. QUOTA is implemented using the BSD systemcall interface
 *          as the means of communication with the user level. Should work for
 *          all filesystems because of integration into the VFS layer of the
 *          operating system. This is based on the Melbourne quota system wich
 *          uses both user and group quota files.
 * 
 *          Program to mail to users that they are over there quota.
 * 
 * Author:  Marco van Wieringen <mvw@planets.elm.net>
 *
 *          This program is free software; you can redistribute it and/or
 *          modify it under the terms of the GNU General Public License as
 *          published by the Free Software Foundation; either version 2 of
 *          the License, or (at your option) any later version.
 */

#include "config.h"

#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#include <signal.h>
#include <grp.h>
#include <time.h>
#include <getopt.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/utsname.h>
#include <pwd.h>

#ifdef USE_LDAP_MAIL_LOOKUP
#include <ldap.h>
#endif

#include <linux/quota.h>
 #include <stdarg.h>
#include <libconfig.h>
#include <src/exportd/rozofs_quota.h>
#include <src/exportd/rozofs_quota_api.h>
#include <rozofs/core/disk_table_service.h>
#include "quotasys.h"
#include "config.h"
#include "export.h"
#include "monitor.h"
#include "econfig.h"
#include "rozofs_quota.h"
#include <rozofs/rpc/export_profiler.h>


/* these are just defaults, overridden in the WARNQUOTA_CONF file */
#define MAIL_CMD "/usr/lib/sendmail -t"
#define FROM     "support@localhost"
#define SUBJECT  "Disk Quota usage on system"
//#define CC_TO    "root"
#define CC_TO    "john.doe@example.com"
#define SUPPORT  "support@localhost"
#define PHONE    "(xxx) xxx-xxxx or (yyy) yyy-yyyy"

#define DEF_USER_MESSAGE	_("Hi,\n\nWe noticed that you are in violation with the quotasystem\n" \
                          "used on this system. We have found the following violations:\n\n")
#define DEF_USER_SIGNATURE	_("\nWe hope that you will cleanup before your grace period expires.\n" \
	                  "\nBasically, this means that the system thinks you are using more disk space\n" \
	                  "on the above partition(s) than you are allowed.  If you do not delete files\n" \
	                  "and get below your quota before the grace period expires, the system will\n" \
	                  "prevent you from creating new files.\n\n" \
                          "For additional assistance, please contact us at %s\nor via " \
                          "phone at %s.\n")
#define DEF_GROUP_MESSAGE	_("Hi,\n\nWe noticed that the group %s you are member of violates the quotasystem\n" \
                          "used on this system. We have found the following violations:\n\n")
#define DEF_GROUP_SIGNATURE	_("\nPlease cleanup the group data before the grace period expires.\n" \
	                  "\nBasically, this means that the system thinks group is using more disk space\n" \
	                  "on the above partition(s) than it is allowed.  If you do not delete files\n" \
	                  "and get below group quota before the grace period expires, the system will\n" \
	                  "prevent you and other members of the group from creating new files owned by\n" \
			  "the group.\n\n" \
                          "For additional assistance, please contact us at %s\nor via " \
                          "phone at %s.\n")

#define SHELL "/bin/sh"
#define QUOTATAB "/etc/quotatab"
#define CNF_BUFFER 2048
#define IOBUF_SIZE 16384		/* Size of buffer for line in config files */
#define ADMIN_TAB_ALLOC 256		/* How many entries to admins table should we allocate at once? */
#define WARNQUOTA_CONF "/etc/warnquota.conf"
#define ADMINSFILE "/etc/quotagrpadmins"
#define EXPORT_DEFAULT_PATH "/etc/rozofs/export.conf"
#define MY_EMAIL "john.doe@example.com"

#define FL_USER 1
#define FL_GROUP 2
#define FL_NOAUTOFS 4
#define FL_SHORTNUMS 8
#define FL_NODETAILS 16


struct util_dqblk {
	qsize_t dqb_ihardlimit;
	qsize_t dqb_isoftlimit;
	qsize_t dqb_curinodes;
	qsize_t dqb_bhardlimit;
	qsize_t dqb_bsoftlimit;
	qsize_t dqb_curspace;
	time_t dqb_btime;
	time_t dqb_itime;			/* Format specific dquot information */
};

struct usage {
	char *devicename;
	struct util_dqblk dq_dqb;
	struct usage *next;
};

#ifdef USE_LDAP_MAIL_LOOKUP
static LDAP *ldapconn = NULL;
#endif


struct configparams {
	char mail_cmd[CNF_BUFFER];
	char from[CNF_BUFFER];
	char subject[CNF_BUFFER];
	char cc_to[CNF_BUFFER];
	char support[CNF_BUFFER];
	char phone[CNF_BUFFER];
	char charset[CNF_BUFFER];
	char *user_message;
	char *user_signature;
	char *group_message;
	char *group_signature;
	int use_ldap_mail; /* 0 */
	time_t cc_before;
#ifdef USE_LDAP_MAIL_LOOKUP
	int ldap_is_setup; /* 0 */
	char ldap_host[CNF_BUFFER];
	int ldap_port;
	char ldap_uri[CNF_BUFFER];
	char ldap_binddn[CNF_BUFFER];
	char ldap_bindpw[CNF_BUFFER];
	char ldap_basedn[CNF_BUFFER];
	char ldap_search_attr[CNF_BUFFER];
	char ldap_mail_attr[CNF_BUFFER];
	char default_domain[CNF_BUFFER];
#endif /* USE_LDAP_MAIL_LOOKUP */
};

struct offenderlist {
	int offender_type;
	int offender_id;
	char *offender_name;
	struct usage *usage;
	struct offenderlist *next;
};

typedef struct quotatable {
	char *devname;
	char *devdesc;
} quotatable_t;

struct adminstable {
	char *grpname;
	char *adminname;
};
#define MAX_FSTYPE_LEN 16		/* Maximum length of filesystem type name */

/* Generic information about quotafile */
struct util_dqinfo {
	time_t dqi_bgrace;	/* Block grace time for given quotafile */
	time_t dqi_igrace;	/* Inode grace time for given quotafile */
	uint64_t dqi_max_b_limit;	/* Maximal block limit storable in current format */
	uint64_t dqi_max_i_limit;	/* Maximal inode limit storable in current format */
	uint64_t dqi_max_b_usage;	/* Maximal block usage storable in current format */
	uint64_t dqi_max_i_usage;	/* Maximal inode usage storable in current format */
};

/* Structure for one opened quota file */
struct quota_handle {
	int qh_fd;		/* Handle of file (-1 when IOFL_QUOTAON) */
	int qh_io_flags;	/* IO flags for file */
	char qh_quotadev[PATH_MAX];	/* Device file is for */
	char qh_dir[PATH_MAX];		/* Directory filesystem is mounted at */
	char qh_fstype[MAX_FSTYPE_LEN];	/* Type of the filesystem on qh_quotadev */
	int qh_type;		/* Type of quotafile */
	int qh_fmt;		/* Quotafile format */
//	struct stat qh_stat;	/* stat(2) for qh_quotadev */
//	struct quotafile_ops *qh_ops;	/* Operations on quotafile */
	struct util_dqinfo qh_info;	/* Generic quotafile info */
};


static int qtab_i = 0, flags;
static char maildev[CNF_BUFFER];
static struct quota_handle *maildev_handle;
static char *configfile = WARNQUOTA_CONF, *quotatabfile = QUOTATAB, *adminsfile = ADMINSFILE;
char *progname;
static char *hostname, *domainname;
static quotatable_t *quotatable;
static int adminscnt, adminsalloc;
static struct adminstable *adminstable;

char *confname=NULL;
econfig_t exportd_config;
int rozofs_no_site_file = 0;

export_one_profiler_t * export_profiler[1];
uint32_t                export_profiler_eid;

/*
 * Global pointers to list.
 */
static struct offenderlist *offenders = (struct offenderlist *)0;

/*
 * add any cleanup functions here
 */
static void wc_exit(int ex_stat)
{
#ifdef USE_LDAP_MAIL_LOOKUP
	if(ldapconn != NULL)
#ifdef USE_LDAP_23
		ldap_unbind_ext(ldapconn, NULL, NULL);
#else
		ldap_unbind(ldapconn);
#endif
#endif
	exit(ex_stat);
}

#ifdef USE_LDAP_MAIL_LOOKUP
#ifdef NEED_LDAP_PERROR
static void ldap_perror(LDAP *ld, LDAP_CONST char *s)
{
	int err;

	ldap_get_option(ld, LDAP_OPT_RESULT_CODE, &err);
	errstr(_("%s: %s\n"), s, ldap_err2string(err));
}
#endif

static int setup_ldap(struct configparams *config)
{
	int ret;
#ifdef USE_LDAP_23
	struct berval cred = { .bv_val = config->ldap_bindpw,
			       .bv_len = strlen(config->ldap_bindpw) };
#endif

#ifdef USE_LDAP_23
	ldap_initialize(&ldapconn, config->ldap_uri);
#else
	ldapconn = ldap_init(config->ldap_host, config->ldap_port);
#endif

	if(ldapconn == NULL) {
		ldap_perror(ldapconn, "ldap_init");
		return -1;
	}

#ifdef USE_LDAP_23
	ret = ldap_sasl_bind_s(ldapconn, config->ldap_binddn, LDAP_SASL_SIMPLE, &cred, NULL, NULL, NULL);
#else
	ret = ldap_bind_s(ldapconn, config->ldap_binddn, config->ldap_bindpw, LDAP_AUTH_SIMPLE);
#endif
	if(ret < 0) {
		ldap_perror(ldapconn, "ldap_bind");
		return -1;
	}
	return 0;
}
		
#endif

static struct offenderlist *add_offender(int type, int id, char *name)
{
	struct offenderlist *offender;
	char namebuf[MAXNAMELEN];
	
	if (!name) {
		if (id2name(id, type, namebuf)) {
			errstr(_("Cannot get name for uid/gid %u.\n"), id);
			return NULL;
		}
		name = namebuf;
	}
	offender = (struct offenderlist *)smalloc(sizeof(struct offenderlist));
	offender->offender_type = type;
	offender->offender_id = id;
	offender->offender_name = sstrdup(name);
	offender->usage = (struct usage *)NULL;
	offender->next = offenders;
	offenders = offender;
	return offender;
}

static void add_offence(rozofs_dquot_t *dquot, char *name)
{
	struct offenderlist *lptr;
	struct usage *usage;

	for (lptr = offenders; lptr; lptr = lptr->next)
		if (dquot->key.s.type == lptr->offender_type && lptr->offender_id == dquot->key.s.qid)
			break;

	if (!lptr)
		if (!(lptr = add_offender(dquot->key.s.type, dquot->key.s.qid, name)))
			return;

	usage = (struct usage *)smalloc(sizeof(struct usage));

	usage->dq_dqb.dqb_ihardlimit  = dquot->quota.dqb_ihardlimit ;
	usage->dq_dqb.dqb_isoftlimit  = dquot->quota.dqb_isoftlimit  ;
	usage->dq_dqb.dqb_curinodes   = dquot->quota.dqb_curinodes  ;
	usage->dq_dqb.dqb_bhardlimit  = dquot->quota.dqb_bhardlimit  ;
	usage->dq_dqb.dqb_bsoftlimit  = dquot->quota.dqb_bsoftlimit  ;
	usage->dq_dqb.dqb_curspace    = dquot->quota.dqb_curspace  ;
	usage->dq_dqb.dqb_btime       = dquot->quota.dqb_btime  ;
	usage->dq_dqb.dqb_itime       = dquot->quota.dqb_itime  ;	
        char bufall[128];
	sprintf(bufall,"eid_%d",dquot->key.s.eid);
	usage->devicename = sstrdup(bufall);
	/*
	 * Stuff it in front
	 */
	usage->next = lptr->usage;
	lptr->usage = usage;
}

static int deliverable(rozofs_dquot_t *dquot)
{
	time_t now;
	
	if (!maildev[0])
		return 1;

	time(&now);
	
	if (!strcasecmp(maildev, "any") && 
	   ((dquot->quota.dqb_bhardlimit && rozofs_toqb(dquot->quota.dqb_curspace) >= dquot->quota.dqb_bhardlimit)
	   || ((dquot->quota.dqb_bsoftlimit && rozofs_toqb(dquot->quota.dqb_curspace) >= dquot->quota.dqb_bsoftlimit)
	   && (dquot->quota.dqb_btime && dquot->quota.dqb_btime <= now))))
		return 0;
	if (!maildev_handle)
		return 1;
#if 0
	mdquot = maildev_handle->qh_ops->read_dquot(maildev_handle, dquot->quota.dq_id);
	if (mdquot &&
	   ((mdquot->quota.dqb_bhardlimit && rozofs_toqb(mdquot->quota.dqb_curspace) >= mdquot->quota.dqb_bhardlimit)
	   || ((mdquot->quota.dqb_bsoftlimit && rozofs_toqb(mdquot->quota.dqb_curspace) >= mdquot->quota.dqb_bsoftlimit)
	   && (mdquot->quota.dqb_btime && mdquot->quota.dqb_btime <= now)))) {
		free(mdquot);
		return 0;
	}
	free(mdquot);
#endif
	return 1;
}

static int check_offence(rozofs_dquot_t *dquot, char *name)
{
	if ((dquot->quota.dqb_bsoftlimit && rozofs_toqb(dquot->quota.dqb_curspace) >= dquot->quota.dqb_bsoftlimit)
	    || (dquot->quota.dqb_isoftlimit && dquot->quota.dqb_curinodes >= dquot->quota.dqb_isoftlimit)) {
 
		if(deliverable(dquot))
			add_offence(dquot, name);
	}
	return 0;
}

static FILE *run_mailer(char *command)
{
	int pipefd[2];
	FILE *f;
	if (pipe(pipefd) < 0) {
		errstr(_("Cannot create pipe: %s\n"), strerror(errno));
		return NULL;
	}
	signal(SIGPIPE, SIG_IGN);
	switch(fork()) {
		case -1:
			errstr(_("Cannot fork: %s\n"), strerror(errno));
			return NULL;
		case 0:
			close(pipefd[1]);
			if (dup2(pipefd[0], 0) < 0) {
				errstr(_("Cannot duplicate descriptor: %s\n"), strerror(errno));
				wc_exit(1);
			}			
			execl(SHELL, SHELL, "-c", command, NULL);
			errstr(_("Cannot execute '%s': %s\n"), command, strerror(errno));
			wc_exit(1);
		default:
			close(pipefd[0]);
			if (!(f = fdopen(pipefd[1], "w")))
				errstr(_("Cannot open pipe: %s\n"), strerror(errno));
			return f;
	}
}

static int admin_name_cmp(const void *key, const void *mem)
{
	return strcmp(key, ((struct adminstable *)mem)->grpname);
}

static int should_cc(struct offenderlist *offender, struct configparams *config)
{
	struct usage *lptr;
	struct util_dqblk *dqb;
	time_t atime;

	if (config->cc_before == -1)
		return 1;
	time(&atime);
	for (lptr = offender->usage; lptr; lptr = lptr->next) {
		dqb = &lptr->dq_dqb;
		if (dqb->dqb_bsoftlimit && dqb->dqb_bsoftlimit <= rozofs_toqb(dqb->dqb_curspace) && dqb->dqb_btime-config->cc_before <= atime)
			return 1;
		if (dqb->dqb_isoftlimit && dqb->dqb_isoftlimit <= dqb->dqb_curinodes && dqb->dqb_itime-config->cc_before <= atime)
			return 1;
	}
	return 0;
}

/* Substitute %s and %i for 'name' and %h for hostname */
static void format_print(FILE *fp, char *fmt, char *name)
{
	char *ch, *lastch = fmt;

	for (ch = strchr(fmt, '%'); ch; lastch = ch+2, ch = strchr(ch+2, '%')) {
		*ch = 0;
		fputs(lastch, fp);
		*ch = '%';
		switch (*(ch+1)) {
			case 's':
			case 'i':
				fputs(name, fp);
				break;
			case 'h':
				fputs(hostname, fp);
				break;
			case 'd':
				fputs(domainname, fp);
				break;
			case '%':
				fputc('%', fp);
				break;
		}
	}
	fputs(lastch, fp);
}

static int mail_user(struct offenderlist *offender, struct configparams *config)
{
	struct usage *lptr;
	FILE *fp;
	int cnt, status;
	char timebuf[MAXTIMELEN];
	char numbuf[3][MAXNUMLEN];
	struct util_dqblk *dqb;
	char *to = NULL;
#ifdef USE_LDAP_MAIL_LOOKUP
       	char searchbuf[256];
	LDAPMessage *result, *entry;
	BerElement     *ber = NULL;
	struct berval  **bvals = NULL;
	int ret;
	char *a;
#endif

	if (offender->offender_type == USRQUOTA) {
#ifdef USE_LDAP_MAIL_LOOKUP
		if(config->use_ldap_mail != 0) {
			if((ldapconn == NULL) && (config->ldap_is_setup == 0)) {
				/* need init */
				if(setup_ldap(config)) {
					errstr(_("Could not setup ldap connection, returning.\n"));
					return -1;
				}
				config->ldap_is_setup = 1;
			}

			if(ldapconn == NULL) {
				/* ldap was never setup correctly so just use the offender_name */
				to = sstrdup(offender->offender_name);
			} else {
				/* search for the offender_name in ldap */
				snprintf(searchbuf, 256, "(%s=%s)", config->ldap_search_attr, 
					offender->offender_name);
#ifdef USE_LDAP_23
				ret = ldap_search_ext_s(ldapconn, config->ldap_basedn, 
					LDAP_SCOPE_SUBTREE, searchbuf,
					NULL, 0, NULL, NULL, NULL, 0, &result);
#else
				ret = ldap_search_s(ldapconn, config->ldap_basedn, 
					LDAP_SCOPE_SUBTREE, searchbuf,
					NULL, 0, &result);
#endif
				if(ret < 0) {
					errstr(_("Error with %s.\n"), offender->offender_name);
					ldap_perror(ldapconn, "ldap_search");
					return 0;
				}
					
				cnt = ldap_count_entries(ldapconn, result);

				if(cnt > 1) {
					errstr(_("Multiple entries found for client %s (%d). Not sending mail.\n"), 
						offender->offender_name, cnt);
					return 0;
				} else if(cnt == 0) {
					errstr(_("Entry not found for client %s. Not sending mail.\n"), 
						offender->offender_name);
					return 0;
				} else {
					/* get the attr */
					entry = ldap_first_entry(ldapconn, result);
					for(a = ldap_first_attribute(ldapconn, entry, &ber); a != NULL;
						a = ldap_next_attribute( ldapconn, entry, ber)) {
						if(strcasecmp(a, config->ldap_mail_attr) == 0) {
							bvals = ldap_get_values_len(ldapconn, entry, a);
							if(bvals == NULL) {
								errstr(_("Could not get values for %s.\n"), 
									offender->offender_name);
								return 0;
							}
							to = sstrdup(bvals[0]->bv_val);
							break;
						} 
					} 

					ber_bvecfree(bvals);
					if(to == NULL) {
						/* 
						 * use just the name and default domain as we didn't find the
						 * attribute we wanted in this entry
						 */
						to = malloc(strlen(offender->offender_name)+
							strlen(config->default_domain)+2);
						sprintf(to, "%s@%s", offender->offender_name,
							config->default_domain);
					}
				}
			}
		} else {
			to = sstrdup(offender->offender_name);
		}
#else
		to = sstrdup(offender->offender_name);
#endif
	} else {
		struct adminstable *admin;

		if (!(admin = bsearch(offender->offender_name, adminstable, adminscnt, sizeof(struct adminstable), admin_name_cmp))) {
			errstr(_("Administrator for a group %s not found. Cancelling mail.\n"), offender->offender_name);
			return -1;
		}
		to = sstrdup(admin->adminname);
	}
	if (!(fp = run_mailer(config->mail_cmd))) {
		if(to)
			free(to);
		return -1;
	}
	fprintf(fp, "From: %s\n", config->from);
	fprintf(fp, "Reply-To: %s\n", config->support);
	fprintf(fp, "Subject: %s\n", config->subject);
        fprintf(fp, "To: %s\n", CC_TO);
//	fprintf(fp, "To: %s\n", to);
	if (should_cc(offender, config))
		fprintf(fp, "Cc: %s\n", config->cc_to);
	if ((config->charset)[0] != '\0') { /* are we supposed to set the encoding */
		fprintf(fp, "Content-Type: text/plain; charset=%s\n", config->charset);
		fprintf(fp, "Content-Disposition: inline\n");
		fprintf(fp, "Content-Transfer-Encoding: 8bit\n");
	}
	fprintf(fp, "\n");
	free(to);

	if (offender->offender_type == USRQUOTA)
		if (config->user_message)
			format_print(fp, config->user_message, offender->offender_name);
		else
			fputs(DEF_USER_MESSAGE, fp);
	else
		if (config->group_message)
			format_print(fp, config->group_message, offender->offender_name);
		else
			fprintf(fp, DEF_GROUP_MESSAGE, offender->offender_name);

	if (!(flags & FL_NODETAILS)) {
		for (lptr = offender->usage; lptr; lptr = lptr->next) {
			dqb = &lptr->dq_dqb;
			for (cnt = 0; cnt < qtab_i; cnt++)
				if (!strcmp(quotatable[cnt].devname, lptr->devicename)) {
					fprintf(fp, "\n%s (%s)\n", quotatable[cnt].devdesc, quotatable[cnt].devname);
					break;
				}
			if (cnt == qtab_i)	/* Description not found? */
				fprintf(fp, "\n%s\n", lptr->devicename);
			fprintf(fp, _("\n                        Block limits               File limits\n"));
			fprintf(fp, _("Filesystem           used    soft    hard  grace    used  soft  hard  grace\n"));
			if (strlen(lptr->devicename) > 15)
				fprintf(fp, "%s\n%15s", lptr->devicename, "");
			else
				fprintf(fp, "%-15s", lptr->devicename);
			if (dqb->dqb_bsoftlimit && dqb->dqb_bsoftlimit <= rozofs_toqb(dqb->dqb_curspace))
				difftime2str(dqb->dqb_btime, timebuf);
			else
				timebuf[0] = '\0';
			space2str(rozofs_toqb(dqb->dqb_curspace), numbuf[0], flags & FL_SHORTNUMS);
			space2str(dqb->dqb_bsoftlimit, numbuf[1], flags & FL_SHORTNUMS);
			space2str(dqb->dqb_bhardlimit, numbuf[2], flags & FL_SHORTNUMS);
			fprintf(fp, "%c%c %7s %7s %7s %6s",
			        dqb->dqb_bsoftlimit && rozofs_toqb(dqb->dqb_curspace) >= dqb->dqb_bsoftlimit ? '+' : '-',
				dqb->dqb_isoftlimit && dqb->dqb_curinodes >= dqb->dqb_isoftlimit ? '+' : '-',
				numbuf[0], numbuf[1], numbuf[2], timebuf);
			if (dqb->dqb_isoftlimit && dqb->dqb_isoftlimit <= dqb->dqb_curinodes)
				difftime2str(dqb->dqb_itime, timebuf);
			else
				timebuf[0] = '\0';
			number2str(dqb->dqb_curinodes, numbuf[0], flags & FL_SHORTNUMS);
			number2str(dqb->dqb_isoftlimit, numbuf[1], flags & FL_SHORTNUMS);
			number2str(dqb->dqb_ihardlimit, numbuf[2], flags & FL_SHORTNUMS);
			fprintf(fp, " %7s %5s %5s %6s\n\n", numbuf[0], numbuf[1], numbuf[2], timebuf);
		}
	}


	if (offender->offender_type == USRQUOTA)
		if (config->user_signature)
			format_print(fp, config->user_signature, offender->offender_name);
		else
			fprintf(fp, DEF_USER_SIGNATURE, config->support, config->phone);
	else
		if (config->group_signature)
			format_print(fp, config->group_signature, offender->offender_name);
		else
			fprintf(fp, DEF_GROUP_SIGNATURE, config->support, config->phone);
	fclose(fp);
	if (wait(&status) < 0)	/* Wait for mailer */
		errstr(_("Cannot wait for mailer: %s\n"), strerror(errno));
	else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
		errstr(_("Warning: Mailer exited abnormally.\n"));

	return 0;
}

static int mail_to_offenders(struct configparams *config)
{
	struct offenderlist *lptr;
	int ret = 0;

	/*
	 * Dump offenderlist.
	 */

	for (lptr = offenders; lptr; lptr = lptr->next)
		ret |= mail_user(lptr, config);
	return ret;
}

/*
 * Wipe spaces, tabs, quotes and newlines from beginning and end of string
 */
static void stripstring(char **buff)
{
	int i;

	/* first put a \0 at the tight place to end the string */
	for (i = strlen(*buff) - 1; i >= 0 && (isspace((*buff)[i]) || (*buff)[i] == '"'
	     || (*buff)[i] == '\''); i--);
	(*buff)[i+1] = 0;

	/* then determine the position to start */
	for (i = 0; (*buff)[i] && (isspace((*buff)[i]) || (*buff)[i] == '"' || (*buff)[i] == '\''); i++);
	*buff += i;
}

/*
 * Substitute '|' with end of lines
 */
static void create_eoln(char *buf)
{
	char *colpos = buf;

	while ((colpos = strchr(colpos, '|')))
		*colpos = '\n';
}

/*
 * Read /etc/quotatab (description of devices for users)
 */
static int get_quotatable(void)
{
	FILE *fp;
	char buffer[IOBUF_SIZE], *colpos, *devname, *devdesc;
	int line;
	struct stat st;

	if (!(fp = fopen(quotatabfile, "r"))) {
		errstr(_("Cannot open %s: %s\nWill use device names.\n"), quotatabfile, strerror(errno));
		qtab_i = 0;
		return 0;
	}

	line = 0;
	for (qtab_i = 0; quotatable = srealloc(quotatable, sizeof(quotatable_t) * (qtab_i + 1)),
	     fgets(buffer, sizeof(buffer), fp); qtab_i++) {
		line++;
		quotatable[qtab_i].devname = NULL;
		quotatable[qtab_i].devdesc = NULL;
		if (buffer[0] == '#' || buffer[0] == ';') {	/* Comment? */
			qtab_i--;
			continue;
		}
		/* Empty line? */
		for (colpos = buffer; isspace(*colpos); colpos++);
		if (!*colpos) {
			qtab_i--;
			continue;
		}
		/* Parse line */
		if (!(colpos = strchr(buffer, ':'))) {
			errstr(_("Cannot parse line %d in quotatab (missing ':')\n"), line);
			qtab_i--;
			continue;
		}
		*colpos = 0;
		devname = buffer;
		devdesc = colpos+1;
		stripstring(&devname);
		stripstring(&devdesc);
		quotatable[qtab_i].devname = sstrdup(devname);
		quotatable[qtab_i].devdesc = sstrdup(devdesc);
		create_eoln(quotatable[qtab_i].devdesc);

		if (stat(quotatable[qtab_i].devname, &st) < 0)
			errstr(_("Cannot stat device %s (maybe typo in quotatab)\n"), quotatable[qtab_i].devname);
	}
	fclose(fp);
	return 0;
}

/* Check correctness of the given format */
static void verify_format(char *fmt, char *varname)
{
	char *ch;

	for (ch = strchr(fmt, '%'); ch; ch = strchr(ch+2, '%')) {
		switch (*(ch+1)) {
			case 's':
			case 'i':
			case 'h':
			case 'd':
			case '%':
				continue;
			default:
				die(1, _("Incorrect format string for variable %s.\n\
Unrecognized expression %%%c.\n"), varname, *(ch+1));
		}
	}
}

/*
 * Reads config parameters from configfile
 * uses default values if errstr occurs
 */
static int readconfigfile(const char *filename, struct configparams *config)
{
	FILE *fp;
	char buff[IOBUF_SIZE];
	char *var;
	char *value;
	char *pos;
	int line, len, bufpos;

	/* set default values */
	sstrncpy(config->mail_cmd, MAIL_CMD, CNF_BUFFER);
	sstrncpy(config->from, FROM, CNF_BUFFER);
	sstrncpy(config->subject, SUBJECT, CNF_BUFFER);
	sstrncpy(config->cc_to, CC_TO, CNF_BUFFER);
	sstrncpy(config->support, SUPPORT, CNF_BUFFER);
	sstrncpy(config->phone, PHONE, CNF_BUFFER);
	(config->charset)[0] = '\0';
	maildev[0] = 0;
	config->user_signature = config->user_message = config->group_signature = config->group_message = NULL;
	config->use_ldap_mail = 0;
	config->cc_before = -1;

#ifdef USE_LDAP_MAIL_LOOKUP
	config->ldap_port = config->ldap_is_setup = 0;
	config->ldap_host[0] = 0;
	config->ldap_uri[0] = 0;
#endif

	if (!(fp = fopen(filename, "r"))) {
		errstr(_("Cannot open %s: %s\n"), filename, strerror(errno));
		return -1;
	}

	line = 0;
	bufpos = 0;
	while (fgets(buff + bufpos, sizeof(buff) - bufpos, fp)) {	/* start reading lines */
		line++;

		if (!bufpos) {
			/* check for comments or empty lines */
			if (buff[0] == '#' || buff[0] == ';')
				continue;
			/* Is line empty? */
			for (pos = buff; isspace(*pos); pos++);
			if (!*pos)			/* Nothing else was on the line */
				continue;
		}
		len = bufpos + strlen(buff+bufpos);
		if (buff[len-1] != '\n')
			errstr(_("Line %d too long. Truncating.\n"), line);
		else {
			len--;
			if (buff[len-1] == '\\') {	/* Should join with next line? */
				bufpos = len-1;
				continue;
			}
		}
		buff[len] = 0;
		bufpos = 0;
		
		/* check for a '=' char */
		if ((pos = strchr(buff, '='))) {
			*pos = 0;	/* split buff in two parts: var and value */
			var = buff;
			value = pos + 1;

			stripstring(&var);
			stripstring(&value);

			/* check if var matches anything */
			if (!strcmp(var, "MAIL_CMD"))
				sstrncpy(config->mail_cmd, value, CNF_BUFFER);
			else if (!strcmp(var, "FROM"))
				sstrncpy(config->from, value, CNF_BUFFER);
			else if (!strcmp(var, "SUBJECT"))
				sstrncpy(config->subject, value, CNF_BUFFER);
			else if (!strcmp(var, "CC_TO"))
				sstrncpy(config->cc_to, value, CNF_BUFFER);
			else if (!strcmp(var, "SUPPORT"))
				sstrncpy(config->support, value, CNF_BUFFER);
			else if (!strcmp(var, "PHONE"))
				sstrncpy(config->phone, value, CNF_BUFFER);
			else if (!strcmp(var, "CHARSET"))
				sstrncpy(config->charset, value, CNF_BUFFER);
			else if (!strcmp(var, "MAILDEV"))
				/* set the global */
				sstrncpy(maildev, value, CNF_BUFFER);
			else if (!strcmp(var, "MESSAGE")) {
				config->user_message = sstrdup(value);
				create_eoln(config->user_message);
				verify_format(config->user_message, "MESSAGE");
			}
			else if (!strcmp(var, "SIGNATURE")) {
				config->user_signature = sstrdup(value);
				create_eoln(config->user_signature);
				verify_format(config->user_signature, "SIGNATURE");
			}
			else if (!strcmp(var, "GROUP_MESSAGE")) {
				config->group_message = sstrdup(value);
				create_eoln(config->group_message);
				verify_format(config->group_message, "GROUP_MESSAGE");
			}
			else if (!strcmp(var, "GROUP_SIGNATURE")) {
				config->group_signature = sstrdup(value);
				create_eoln(config->group_signature);
				verify_format(config->group_signature, "GROUP_SIGNATURE");
			}
			else if (!strcmp(var, "LDAP_MAIL")) {
				if(strcasecmp(value, "true") == 0) 
					config->use_ldap_mail = 1;
				else
					config->use_ldap_mail = 0;
			}
			else if (!strcmp(var, "CC_BEFORE")) {
				int num;
				char unit[10];

				if (sscanf(value, "%d%s", &num, unit) != 2)
					goto cc_parse_err;
				if (str2timeunits(num, unit, &config->cc_before) < 0) {
cc_parse_err:
					die(1, _("Cannot parse time at CC_BEFORE variable (line %d).\n"), line);
				}
			}
#ifdef USE_LDAP_MAIL_LOOKUP
			else if (!strcmp(var, "LDAP_HOST"))
				sstrncpy(config->ldap_host, value, CNF_BUFFER);
			else if (!strcmp(var, "LDAP_PORT"))
				config->ldap_port = (int)strtol(value, NULL, 10);
			else if (!strcmp(var, "LDAP_URI"))
				sstrncpy(config->ldap_uri, value, CNF_BUFFER);
			else if(!strcmp(var, "LDAP_BINDDN"))
				sstrncpy(config->ldap_binddn, value, CNF_BUFFER);
			else if(!strcmp(var, "LDAP_BINDPW"))
				sstrncpy(config->ldap_bindpw, value, CNF_BUFFER);
			else if(!strcmp(var, "LDAP_BASEDN"))
				sstrncpy(config->ldap_basedn, value, CNF_BUFFER);
			else if(!strcmp(var, "LDAP_SEARCH_ATTRIBUTE"))
				sstrncpy(config->ldap_search_attr, value, CNF_BUFFER);
			else if(!strcmp(var, "LDAP_MAIL_ATTRIBUTE"))
				sstrncpy(config->ldap_mail_attr, value, CNF_BUFFER);
			else if(!strcmp(var, "LDAP_DEFAULT_MAIL_DOMAIN"))
				sstrncpy(config->default_domain, value, CNF_BUFFER);
#endif
			else	/* not matched at all */
				errstr(_("Error in config file (line %d), ignoring\n"), line);
		}
		else		/* no '=' char in this line */
			errstr(_("Possible error in config file (line %d), ignoring\n"), line);
	}
	if (bufpos)
		errstr(_("Unterminated last line, ignoring\n"));
#ifdef USE_LDAP_MAIL_LOOKUP
	if (config->use_ldap_mail)
	{
#ifdef USE_LDAP_23
		if (!config->ldap_uri[0]) {
			snprintf(config->ldap_uri, CNF_BUFFER, "ldap://%s:%d", config->ldap_host, config->ldap_port);
			errstr(_("LDAP library version >= 2.3 detected. Please use LDAP_URI instead of hostname and port.\nGenerated URI %s\n"), config->ldap_uri);
		}
#else
		if (config->ldap_uri[0])
			die(1, _("LDAP library does not support ldap_initialize() but URI is specified."));
#endif
	}
#endif
	fclose(fp);

	return 0;
}

static int admin_cmp(const void *a1, const void *a2)
{
	return strcmp(((struct adminstable *)a1)->grpname, ((struct adminstable *)a2)->grpname);
}

/* Get administrators of the groups */
static int get_groupadmins(void)
{
	FILE *f;
	int line = 0;
	char buffer[IOBUF_SIZE], *colpos, *grouppos, *endname, *adminpos;

	if (!(f = fopen(adminsfile, "r"))) {
		errstr(_("Cannot open file with group administrators: %s\n"), strerror(errno));
		return -1;
	}
	
	while (fgets(buffer, IOBUF_SIZE, f)) {
		line++;
		if (buffer[0] == ';' || buffer[0] == '#')
			continue;
		/* Skip initial spaces */
		for (colpos = buffer; isspace(*colpos); colpos++);
		if (!*colpos)	/* Empty line? */
			continue;
		/* Find splitting colon */
		for (grouppos = colpos; *colpos && *colpos != ':'; colpos++);
		if (!*colpos || grouppos == colpos) {
			errstr(_("Parse error at line %d. Cannot find end of group name.\n"), line);
			continue;
		}
		/* Cut trailing spaces */
		for (endname = colpos-1; isspace(*endname); endname--);
		*(++endname) = 0;
		/* Skip initial spaces at admins name */
		for (colpos++; isspace(*colpos); colpos++);
		if (!*colpos) {
			errstr(_("Parse error at line %d. Cannot find administrators name.\n"), line);
			continue;
		}
		/* Go through admins name */
		for (adminpos = colpos; !isspace(*colpos); colpos++);
		if (*colpos) {	/* Some characters after name? */
			*colpos = 0;
			/* Skip trailing spaces */
			for (colpos++; isspace(*colpos); colpos++);
			if (*colpos) {
				errstr(_("Parse error at line %d. Trailing characters after administrators name.\n"), line);
				continue;
			}
		}
		if (adminscnt >= adminsalloc)
			adminstable = srealloc(adminstable, sizeof(struct adminstable)*(adminsalloc+=ADMIN_TAB_ALLOC));
		adminstable[adminscnt].grpname = sstrdup(grouppos);
		adminstable[adminscnt++].adminname = sstrdup(adminpos);
	}

	fclose(f);
	qsort(adminstable, adminscnt, sizeof(struct adminstable), admin_cmp);
	return 0;
}

#if 0

static struct quota_handle *find_handle_dev(char *dev, struct quota_handle **handles)
{
	int i;

	for (i = 0; handles[i] && strcmp(dev, handles[i]->qh_quotadev); i++);
	return handles[i];
}
#endif
/*
**__________________________________________________________________
*/
/**

    Dump quota information relative to a given eid for a given type (USER or GROUP)
    
    @param : ctx_p : pointer to the disk table associated with the type
    @param  type: type of the quota
    
*/   
void rozofs_warn_quota(rozofs_qt_export_t *quota_ctx_p,int type,int eid,char *path)
{

   int file_idx_next= 0;
   int file_idx = 0;
   int fd = -1;
   int nb_records;
   int record;
   int count;
   rozofs_dquot_t data;
   struct group *grp_p;
   struct passwd *usr_p;  
  char name[256];
  disk_table_header_t *ctx_p;
  
   ctx_p = quota_ctx_p->quota_inode[type];
   /*
   ** check if quota is enable for the fs
   */
   if (quota_ctx_p->quota_super[type].enable == 0) return;
          
   while((file_idx = disk_tb_get_next_file_entry(ctx_p,(uint32_t*)&file_idx_next)) >= 0)
   {
      /*
      ** we get one file, so now get the entries
      */
      while((nb_records = disk_tb_get_nb_records(ctx_p,file_idx,&fd)) > 0)
      {
         /*
	 ** read the file records
	 */
	 for (record = 0; record < nb_records; record++)
	 {
	    count = disk_tb_get_next_record(ctx_p,record,fd,&data);
	    if (count != ctx_p->entry_sz)
	    {
	      break;
	    }
	    if (type == USRQUOTA)
	    {
	        usr_p= getpwuid(data.key.s.qid);
		if (usr_p != NULL)
		{
		  strcpy(name,usr_p->pw_name);
		}
		else
		{
		  sprintf(name, "#%u",data.key.s.qid); 
		}	    	    
	    }
	    else
	    {
	        grp_p= getgrgid(data.key.s.qid);
		if (grp_p != NULL)
		{
		  strcpy(name,grp_p->gr_name);
		}
		else
		{
		  sprintf(name, "#%u",data.key.s.qid); 
		}
	    
	    }
	    /*
	    ** check offence
	    */
	    check_offence(&data,name);	 
	 }
	 close(fd);
	 break;      
      }   
   }
}
/*
**__________________________________________________________________
*/
static void warn_quota(int fs_count, char **fs)
{

	struct configparams config;
	int i;
	int eid;
	char *errch=NULL;
        rozofs_qt_export_t *quota_ctx_p = NULL;
	char *pathname;
	
	if (readconfigfile(configfile, &config) < 0)
		wc_exit(1);
	if (get_quotatable() < 0)
		wc_exit(1);
	for (i = 0; i <fs_count; i++)
	{
	   errch = NULL;
	   eid = strtoul(fs[i], &errch, 0);
           if (!*errch)		/* Is name number - we got directly gid? */
	   if (errch != NULL) 
	   {
	      if (*errch != 0)
	      {
	        continue;
	      }
	   }
	   pathname = econfig_get_export_path(&exportd_config,eid);
	   if (pathname == NULL)
	   {
	      printf("export %d does not exist\n",eid);
	      continue;	   
	   }
	  
	  quota_ctx_p = rozofs_qt_alloc_context(eid,pathname,1);
	  if (quota_ctx_p == NULL)
	  {
	     printf("fail to create quota data for exportd %d\n",eid);
	     exit(-1);
	  }

	  if (flags & FL_USER)
	  {
	     if (!maildev[0] || !strcasecmp(maildev, "any"))
		     maildev_handle = NULL;
	     else
		     maildev_handle = NULL ; // find_handle_dev(maildev, handles);
	     rozofs_warn_quota(quota_ctx_p,USRQUOTA,eid,pathname);
	  }

	  if (flags & FL_GROUP)
	  {
	    if (get_groupadmins() < 0)
	    {
	      wc_exit(1);
	     }
	     if (!maildev[0] || !strcasecmp(maildev, "any"))
	     {
	       maildev_handle = NULL;
	     }
	     else
	     {
	       maildev_handle = NULL; //find_handle_dev(maildev, handles);
	     }
	     rozofs_warn_quota(quota_ctx_p,USRQUOTA,eid,pathname);
	  }
	}

	if (mail_to_offenders(&config) < 0)
		wc_exit(1);


}
#if 0
static void warn_quota(int fs_count, char **fs)
{
	struct quota_handle **handles;
	struct configparams config;
	int i;

	if (readconfigfile(configfile, &config) < 0)
		wc_exit(1);
	if (get_quotatable() < 0)
		wc_exit(1);

	if (flags & FL_USER) {
		handles = create_handle_list(fs_count, fs, USRQUOTA, -1, IOI_READONLY | IOI_INITSCAN, MS_LOCALONLY | (flags & FL_NOAUTOFS ? MS_NO_AUTOFS : 0));
		if (!maildev[0] || !strcasecmp(maildev, "any"))
			maildev_handle = NULL;
		else
			maildev_handle = find_handle_dev(maildev, handles);
		for (i = 0; handles[i]; i++)
			handles[i]->qh_ops->scan_dquots(handles[i], check_offence);
		dispose_handle_list(handles);
	}
	if (flags & FL_GROUP) {
		if (get_groupadmins() < 0)
			wc_exit(1);
		handles = create_handle_list(fs_count, fs, GRPQUOTA, -1, IOI_READONLY | IOI_INITSCAN, MS_LOCALONLY | (flags & FL_NOAUTOFS ? MS_NO_AUTOFS : 0));
		if (!maildev[0] || !strcasecmp(maildev, "any"))
			maildev_handle = NULL;
		else
			maildev_handle = find_handle_dev(maildev, handles);
		for (i = 0; handles[i]; i++)
			handles[i]->qh_ops->scan_dquots(handles[i], check_offence);
		dispose_handle_list(handles);
	}
	if (mail_to_offenders(&config) < 0)
		wc_exit(1);
}
#endif

/* Print usage information */
static void usage(void)
{
	errstr(_("Usage:\n  rozo_warnquota [-ugsid] [-f exportconf] [-c configfile] [-q quotatabfile] [-a adminsfile] [filesystem...]\n\n\
-u, --user                      warn users\n\
-g, --group                     warn groups\n\
-s, --human-readable            send information in more human friendly units\n\
-i, --no-autofs                 avoid autofs mountpoints\n\
-d, --no-details                do not send quota information itself\n\
-f, --exportconf=path           pathname of the export configuration\n\
-c, --config=config-file        non-default config file\n\
-q, --quota-tab=quotatab-file   non-default quotatab\n\
-a, --admins-file=admins-file   non-default admins file\n\
-h, --help                      display this help message and exit\n\
-v, --version                   display version information and exit\n\n"));
	fprintf(stderr, _("Bugs to %s\n"), MY_EMAIL);
	wc_exit(1);
}
 
static void parse_options(int argcnt, char **argstr)
{
	int ret;
	struct option long_opts[] = {
		{ "user", 0, NULL, 'u' },
		{ "group", 0, NULL, 'g' },
		{ "version", 0, NULL, 'V' },
		{ "help", 0, NULL, 'h' },
		{ "exportconf", 1, NULL, 'f' },
		{ "config", 1, NULL, 'c' },
		{ "quota-tab", 1, NULL, 'q' },
		{ "admins-file", 1, NULL, 'a' },
		{ "no-autofs", 0, NULL, 'i' },
		{ "human-readable", 0, NULL, 's' },
		{ "no-details", 0, NULL, 'd' },
		{ NULL, 0, NULL, 0 }
	};
 
	while ((ret = getopt_long(argcnt, argstr, "ugVf:hc:q:a:isd", long_opts, NULL)) != -1) {
		switch (ret) {
		  case '?':
		  case 'h':
			usage();
		  case 'V':
			version();
			exit(0);
		  case 'f':
		        confname = optarg;
			break;
		  case 'c':
			configfile = optarg;
			break;
		  case 'q':
			quotatabfile = optarg;
			break;
		  case 'a':
			adminsfile = optarg;
			break;
		  case 'u':
			flags |= FL_USER;
			break;
		  case 'g':
			flags |= FL_GROUP;
			break;
		  case 'i':
			flags |= FL_NOAUTOFS;
			break;
		  case 's':
			flags |= FL_SHORTNUMS;
			break;
		  case 'd':
			flags |= FL_NODETAILS;
			break;
		}
	}
	if (!(flags & FL_USER) && !(flags & FL_GROUP))
		flags |= FL_USER;
}
 
static void get_host_name(void)
{
	struct utsname uts;

	if (uname(&uts))
		die(1, _("Cannot get host name: %s\n"), strerror(errno));
	hostname = sstrdup(uts.nodename);
	domainname = sstrdup(uts.domainname);
}

int main(int argc, char **argv)
{
        int ret;
	
	gettexton();
	progname = basename(argv[0]);
	get_host_name();
        confname = strdup(EXPORT_DEFAULT_PATH);

	parse_options(argc, argv);
	/*
	** read the configuration file of the exports
	*/
	ret = export_config_read(&exportd_config,confname);
	if (ret < 0)
	{
	   printf("Error on reading exportd configuration: %s -> %s\n",confname,strerror(errno));
	   exit(0);
	}
	/*
	** init of the data strcuture needed by quota manager
	*/
	rozofs_qt_init();
	warn_quota(argc - optind, argc > optind ? argv + optind : NULL);

	wc_exit(0);
	return 0;
}