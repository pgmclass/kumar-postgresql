/*-------------------------------------------------------------------------
 *
 * pg_lsclusters.c
 *		List the PostgreSQL clusters running on this machine (Kumar Server).
 *
 * A running cluster is represented by its postmaster: a process named
 * "postgres" whose parent is not itself a "postgres" process (the children
 * are backends and auxiliary processes). The postmaster changes its working
 * directory to the data directory at startup, so /proc/<pid>/cwd leads to
 * the data directory, where postmaster.pid and PG_VERSION describe it.
 *
 * This relies on the /proc file system and therefore works on Linux only.
 * Reading another user's /proc/<pid>/cwd needs root or the same user; such
 * clusters are still listed, marked "permission denied".
 *
 * src/bin/pg_lsclusters/pg_lsclusters.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres_fe.h"

#include <dirent.h>
#include <time.h>
#include <unistd.h>

#include "common/logging.h"
#include "getopt_long.h"
#include "utils/pidfile.h"

typedef struct Cluster
{
	int			pid;
	bool		denied;			/* could not read /proc/<pid>/cwd */
	char		datadir[MAXPGPATH];
	char		version[32];
	char		port[16];
	char		status[32];
	time_t		start_time;		/* 0 if unknown */
} Cluster;

static const char *progname;
static bool no_header = false;

static void
usage(void)
{
	printf(_("%s lists the PostgreSQL clusters running on this machine.\n\n"), progname);
	printf(_("Usage:\n"));
	printf(_("  %s [OPTION]...\n"), progname);
	printf(_("\nOptions:\n"));
	printf(_("  -H, --no-header    do not print the column header\n"));
	printf(_("  -v, --verbose      explain each step (which processes were checked and why)\n"));
	printf(_("  -V, --version      output version information, then exit\n"));
	printf(_("  -?, --help         show this help, then exit\n"));
	printf(_("\nClusters of any PostgreSQL version are listed. Run as root to see the\n"
			 "data directories of clusters owned by other users. Linux only (/proc).\n"));
}

/*
 * Read the first line of a small file into buf, without the trailing
 * newline. Returns false if the file cannot be read.
 */
static bool
read_first_line(const char *path, char *buf, size_t size)
{
	FILE	   *f = fopen(path, "r");
	bool		ok;

	if (f == NULL)
		return false;
	ok = (fgets(buf, size, f) != NULL);
	fclose(f);
	if (ok)
		buf[strcspn(buf, "\n")] = '\0';
	return ok;
}

/* Is process <pid> named "postgres"? */
static bool
is_postgres_process(int pid)
{
	char		path[MAXPGPATH];
	char		comm[64];

	snprintf(path, sizeof(path), "/proc/%d/comm", pid);
	return read_first_line(path, comm, sizeof(comm)) &&
		strcmp(comm, "postgres") == 0;
}

/*
 * Parent PID from /proc/<pid>/stat. The second field is the command name in
 * parentheses and may itself contain spaces or parentheses, so parse from
 * the last ')'.
 */
static int
parent_pid(int pid)
{
	char		path[MAXPGPATH];
	char		line[1024];
	char	   *p;
	char		state;
	int			ppid;

	snprintf(path, sizeof(path), "/proc/%d/stat", pid);
	if (!read_first_line(path, line, sizeof(line)))
		return -1;
	p = strrchr(line, ')');
	if (p == NULL || sscanf(p + 1, " %c %d", &state, &ppid) != 2)
		return -1;
	return ppid;
}

static void
trim_trailing_spaces(char *s)
{
	int			i = (int) strlen(s) - 1;

	while (i >= 0 && s[i] == ' ')
		s[i--] = '\0';
}

/* Fill in version, port, status and start time from the data directory. */
static void
describe_datadir(Cluster *c)
{
	char		path[MAXPGPATH];
	FILE	   *f;
	char		line[MAXPGPATH];
	int			lineno = 0;

	snprintf(path, sizeof(path), "%s/PG_VERSION", c->datadir);
	if (!read_first_line(path, c->version, sizeof(c->version)))
		strlcpy(c->version, "?", sizeof(c->version));

	snprintf(path, sizeof(path), "%s/postmaster.pid", c->datadir);
	f = fopen(path, "r");
	if (f == NULL)
	{
		pg_log_debug("PID %d: cannot read \"%s\": %m", c->pid, path);
		return;
	}
	while (fgets(line, sizeof(line), f) != NULL)
	{
		lineno++;
		line[strcspn(line, "\n")] = '\0';
		switch (lineno)
		{
			case LOCK_FILE_LINE_PID:
				/* a negative PID marks a single-user backend */
				if (atoi(line) == -c->pid)
					strlcpy(c->status, "single-user", sizeof(c->status));
				else if (atoi(line) != c->pid)
					pg_log_debug("PID %d: postmaster.pid names PID %s instead",
								 c->pid, line);
				break;
			case LOCK_FILE_LINE_START_TIME:
				c->start_time = (time_t) atoll(line);
				break;
			case LOCK_FILE_LINE_PORT:
				strlcpy(c->port, line, sizeof(c->port));
				break;
			case LOCK_FILE_LINE_PM_STATUS:
				if (c->status[0] == '\0')
				{
					strlcpy(c->status, line, sizeof(c->status));
					trim_trailing_spaces(c->status);
				}
				break;
		}
	}
	fclose(f);
	pg_log_debug("PID %d: read postmaster.pid (%d lines)", c->pid, lineno);
}

static void
format_uptime(time_t start, char *buf, size_t size)
{
	long		up;

	if (start == 0)
	{
		strlcpy(buf, "?", size);
		return;
	}
	up = (long) (time(NULL) - start);
	if (up >= 86400)
		snprintf(buf, size, "%ldd %02ld:%02ld:%02ld", up / 86400,
				 (up % 86400) / 3600, (up % 3600) / 60, up % 60);
	else
		snprintf(buf, size, "%02ld:%02ld:%02ld",
				 up / 3600, (up % 3600) / 60, up % 60);
}

static int
cluster_cmp(const void *a, const void *b)
{
	const Cluster *ca = (const Cluster *) a;
	const Cluster *cb = (const Cluster *) b;
	int			pa = atoi(ca->port);
	int			pb = atoi(cb->port);

	if (pa != pb)
		return (pa > pb) - (pa < pb);
	return (ca->pid > cb->pid) - (ca->pid < cb->pid);
}

int
main(int argc, char *argv[])
{
	static struct option long_options[] = {
		{"no-header", no_argument, NULL, 'H'},
		{"verbose", no_argument, NULL, 'v'},
		{NULL, 0, NULL, 0}
	};
	DIR		   *proc;
	struct dirent *de;
	Cluster    *clusters = NULL;
	int			nclusters = 0;
	int			maxclusters = 0;
	int			c;
	int			i;

	pg_logging_init(argv[0]);
	set_pglocale_pgservice(argv[0], PG_TEXTDOMAIN("pg_lsclusters"));
	progname = get_progname(argv[0]);

	if (argc > 1)
	{
		if (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)
		{
			usage();
			exit(0);
		}
		if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)
		{
			puts("pg_lsclusters (PostgreSQL) " PG_VERSION);
			exit(0);
		}
	}

	while ((c = getopt_long(argc, argv, "Hv", long_options, NULL)) != -1)
	{
		switch (c)
		{
			case 'H':
				no_header = true;
				break;
			case 'v':
				pg_logging_increase_verbosity();
				break;
			default:
				pg_log_error_hint("Try \"%s --help\" for more information.", progname);
				exit(1);
		}
	}
	if (optind < argc)
	{
		pg_log_error("too many command-line arguments (first is \"%s\")", argv[optind]);
		pg_log_error_hint("Try \"%s --help\" for more information.", progname);
		exit(1);
	}

	proc = opendir("/proc");
	if (proc == NULL)
		pg_fatal("could not open directory \"/proc\": %m (this program needs the Linux /proc file system)");

	while ((de = readdir(proc)) != NULL)
	{
		char		path[MAXPGPATH];
		Cluster    *cl;
		int			pid;
		int			ppid;
		ssize_t		len;

		if (strspn(de->d_name, "0123456789") != strlen(de->d_name))
			continue;			/* not a process directory */
		pid = atoi(de->d_name);

		if (!is_postgres_process(pid))
			continue;

		ppid = parent_pid(pid);
		if (ppid > 0 && is_postgres_process(ppid))
		{
			pg_log_debug("PID %d: child of postgres process %d, skipped", pid, ppid);
			continue;
		}
		pg_log_debug("PID %d: postgres process with parent %d: a postmaster", pid, ppid);

		if (nclusters == maxclusters)
		{
			maxclusters = maxclusters ? maxclusters * 2 : 8;
			clusters = pg_realloc(clusters, maxclusters * sizeof(Cluster));
		}
		cl = &clusters[nclusters++];
		memset(cl, 0, sizeof(Cluster));
		cl->pid = pid;

		snprintf(path, sizeof(path), "/proc/%d/cwd", pid);
		len = readlink(path, cl->datadir, sizeof(cl->datadir) - 1);
		if (len < 0)
		{
			pg_log_debug("PID %d: cannot read \"%s\": %m", pid, path);
			cl->denied = true;
			continue;
		}
		cl->datadir[len] = '\0';
		pg_log_debug("PID %d: data directory \"%s\"", pid, cl->datadir);
		describe_datadir(cl);
	}
	closedir(proc);

	if (nclusters == 0)
	{
		printf(_("no running PostgreSQL clusters found\n"));
		return 0;
	}

	qsort(clusters, nclusters, sizeof(Cluster), cluster_cmp);

	if (!no_header)
		printf("%-8s %-7s %-6s %-11s %-19s %-13s %s\n",
			   _("PID"), _("VERSION"), _("PORT"), _("STATUS"),
			   _("STARTED"), _("UPTIME"), _("DATA DIRECTORY"));

	for (i = 0; i < nclusters; i++)
	{
		Cluster    *cl = &clusters[i];
		char		started[32] = "?";
		char		uptime[32];

		if (cl->denied)
		{
			printf("%-8d %-7s %-6s %-11s %-19s %-13s %s\n",
				   cl->pid, "?", "?", "?", "?", "?",
				   _("(permission denied: run as root or as the cluster owner)"));
			continue;
		}
		if (cl->start_time != 0)
			strftime(started, sizeof(started), "%Y-%m-%d %H:%M:%S",
					 localtime(&cl->start_time));
		format_uptime(cl->start_time, uptime, sizeof(uptime));

		printf("%-8d %-7s %-6s %-11s %-19s %-13s %s\n",
			   cl->pid, cl->version,
			   cl->port[0] ? cl->port : "?",
			   cl->status[0] ? cl->status : "?",
			   started, uptime, cl->datadir);
	}

	pg_free(clusters);
	return 0;
}
