/*
 * fdbmonitor.cpp
 *
 * This source file is part of the FoundationDB open source project
 *
 * Copyright 2013-2018 Apple Inc. and the FoundationDB project authors
 * 
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *     http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <signal.h>
#include <stdio.h>
#include <unistd.h>
#include <random>

#ifdef __linux__
#include <sys/prctl.h>
#endif

#include <sys/wait.h>

#ifdef __linux__
#include <sys/inotify.h>
#include <time.h>
#include <linux/limits.h>
#endif

#ifdef __APPLE__
#include <sys/event.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#endif

#include <sys/time.h>
#include <stdlib.h>

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <sstream>
#include <iterator>

#include <string.h>
#include <errno.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include <syslog.h>

#include <stdarg.h>

#include <pwd.h>
#include <grp.h>

#include "flow/SimpleOpt.h"
#include "SimpleIni.h"

#include "../versions.h"

#ifdef __linux__
typedef fd_set* fdb_fd_set;
#elif defined __APPLE__
typedef int fdb_fd_set;
#endif

#define CANONICAL_PATH_SEPARATOR '/'

void monitor_fd( fdb_fd_set list, int fd, int* maxfd, void* cmd ) {
#ifdef __linux__
	FD_SET( fd, list );
	if ( fd > *maxfd )
		*maxfd = fd;
#elif defined __APPLE__
	/* ignore maxfd */
	struct kevent ev;
	EV_SET( &ev, fd, EVFILT_READ, EV_ADD, 0, 0, cmd );
	kevent( list, &ev, 1, NULL, 0, NULL ); // FIXME: check?
#endif
}

void unmonitor_fd( fdb_fd_set list, int fd ) {
#ifdef __linux__
	FD_CLR( fd, list );
#elif defined __APPLE__
	struct kevent ev;
	EV_SET( &ev, fd, EVFILT_READ, EV_DELETE, 0, 0, NULL );
	kevent( list, &ev, 1, NULL, 0, NULL ); // FIXME: check?
#endif
}

void get_cur_timestamp(char *buf, int len) {
	if(len <= 0)
		return;
	struct tm tm_info;
	struct timeval tv;
	gettimeofday(&tv, NULL);
	localtime_r(&tv.tv_sec, &tm_info);
	char *end = buf + len;
	buf += strftime(buf, end - buf, "%Z %Y-%m-%d %H:%M:%S", &tm_info);
	// Add fractional seconds
	if(buf < end)
		buf += snprintf(buf, end - buf, ".%06d", tv.tv_usec);
	// Add epoch seconds after timestamp
	if(buf < end)
		buf += snprintf(buf, end - buf, " (%lld.%06d)", (long long int)tv.tv_sec, tv.tv_usec);
}

bool daemonize = false;

void log_msg(int priority, const char* format, ...) {
	va_list args;
	va_start(args, format);

	if (daemonize) {
		vsyslog(priority, format, args);
	} else {
		char timebuf[64];
		get_cur_timestamp(timebuf, 64);
		fprintf(stderr, "%s: ", timebuf);
		vfprintf(stderr, format, args);
	}

	va_end(args);
}

void log_err(const char* func, int err, const char* format, ...) {
	va_list args;
	va_start(args, format);

	char buf[4096];

	int len = vsnprintf( buf, 4096, format, args );

	log_msg( LOG_ERR, "%.*s (%s error %d: %s)\n", len, buf, func, err, strerror(err) );
}

const char* get_value_multi(const CSimpleIni& ini, const char* key, ...) {
	const char* ret = NULL;
	const char* section = NULL;

	va_list ap;
	va_start(ap, key);

	while (!ret && (section = va_arg(ap, const char *)))
		ret = ini.GetValue(section, key, NULL);

	va_end(ap);

	return ret;
}

double timer() {
#if defined(__linux__)
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return double(ts.tv_sec) + (ts.tv_nsec * 1e-9);
#elif defined(__APPLE__)
	mach_timebase_info_data_t timebase_info;
	mach_timebase_info(&timebase_info);
	return ((mach_absolute_time() * timebase_info.numer) / timebase_info.denom) * 1e-9;
#else
#error Port me!
#endif
};

int randomInt(int min, int max) {
	static std::random_device rd;
	static std::mt19937 gen(rd());
	std::uniform_int_distribution<> dis(min, max);

	return dis(gen);
}

std::string joinPath(std::string const& directory, std::string const& filename) {
	auto d = directory;
	auto f = filename;
	while (f.size() && (f[0] == '/' || f[0] == CANONICAL_PATH_SEPARATOR))
		f = f.substr(1);
	while (d.size() && (d.back() == '/' || d.back() == CANONICAL_PATH_SEPARATOR))
		d = d.substr(0, d.size()-1);
	return d + CANONICAL_PATH_SEPARATOR + f;
}

std::string abspath(std::string const& filename) {
	// Returns an absolute path canonicalized to use only CANONICAL_PATH_SEPARATOR
	char result[PATH_MAX];
	auto r = realpath( filename.c_str(), result );
	if (!r) {
		if (errno == ENOENT) {
			int sep = filename.find_last_of( CANONICAL_PATH_SEPARATOR );
			if (sep != std::string::npos) {
				return joinPath( abspath( filename.substr(0, sep) ), filename.substr(sep) );
			}
			else if (filename.find("~") == std::string::npos) {
				return joinPath( abspath( "." ), filename );
			}
		}

		perror("abspath");
		return "";
	}
	return std::string(r);
}

std::string parentDirectory(std::string const& filename) {
	size_t sep = filename.find_last_of( CANONICAL_PATH_SEPARATOR );
	if (sep == std::string::npos) {
		return "";
	}

	return filename.substr(0, sep);
}

int mkdir(std::string const& directory) {
	size_t sep = 0;
	do {
		sep = directory.find_first_of('/', sep + 1);
		if ( mkdir( directory.substr(0, sep).c_str(), 0755 ) != 0 ) {
			if (errno == EEXIST)
				continue;

			return -1;
		}
	} while (sep != std::string::npos && sep != directory.length() - 1);

	return 0;
}

struct Command {
private:
	std::vector<std::string> commands;
	fdb_fd_set fds;
public:
	const char** argv;
	std::string section, ssection;
	uint32_t initial_restart_delay;
	uint32_t max_restart_delay;
	double current_restart_delay;
	double restart_backoff;
	uint32_t restart_delay_reset_interval;
	double last_start;
	bool quiet;
	bool delete_wd40_env;
	bool deconfigured;
	bool kill_on_configuration_change;

	// one pair for each of stdout and stderr
	int pipes[2][2];

	Command() : argv(NULL) { }
	Command(const CSimpleIni& ini, std::string _section, uint64_t id, fdb_fd_set fds, int* maxfd) : section(_section), argv(NULL), quiet(false), delete_wd40_env(false), fds(fds), deconfigured(false), kill_on_configuration_change(true) {
		char _ssection[strlen(section.c_str()) + 22];
		snprintf(_ssection, strlen(section.c_str()) + 22, "%s.%llu", section.c_str(), id);
		ssection = _ssection;

		for ( auto p : pipes ) {
			if ( (pipe(p) == 0) ) {
				monitor_fd( fds, p[0], maxfd, this );
			} else {
				log_err( "pipe", errno, "Unable to construct pipe for %s", ssection.c_str() );
				p[0] = -1;
				p[1] = -1;
			}
		}

		CSimpleIniA::TNamesDepend keys, skeys, gkeys;

		ini.GetAllKeys(section.c_str(), keys);
		ini.GetAllKeys(ssection.c_str(), skeys);
		ini.GetAllKeys("general", gkeys);

		keys.splice(keys.end(), skeys, skeys.begin(), skeys.end());
		keys.splice(keys.end(), gkeys, gkeys.begin(), gkeys.end());
		keys.sort(CSimpleIniA::Entry::KeyOrder());
		keys.unique( [](const CSimpleIniA::Entry& lhs, const CSimpleIniA::Entry& rhs) -> bool {
				return !CSimpleIniA::Entry::KeyOrder()(lhs, rhs);
			} );

		last_start = 0;

		char* endptr;
		const char* rd = get_value_multi(ini, "restart_delay", ssection.c_str(), section.c_str(), "general", "fdbmonitor", NULL);
		if (!rd) {
			log_msg(LOG_ERR, "Unable to resolve restart delay for %s\n", ssection.c_str());
			return;
		}
		else {
			max_restart_delay = strtoul(rd, &endptr, 10);
			if (*endptr != '\0') {
				log_msg(LOG_ERR, "Unable to parse restart delay for %s\n", ssection.c_str());
				return;
			}
		}

		const char* mrd = get_value_multi(ini, "initial_restart_delay", ssection.c_str(), section.c_str(), "general", "fdbmonitor", NULL);
		if (!mrd) {
			initial_restart_delay = 0;
		}
		else {
			initial_restart_delay = std::min<uint32_t>(max_restart_delay, strtoul(mrd, &endptr, 10));
			if (*endptr != '\0') {
				log_msg(LOG_ERR, "Unable to parse initial restart delay for %s\n", ssection.c_str());
				return;
			}
		}

		current_restart_delay = initial_restart_delay;

		const char* rbo = get_value_multi(ini, "restart_backoff", ssection.c_str(), section.c_str(), "general", "fdbmonitor", NULL);
		if(!rbo) {
			restart_backoff = max_restart_delay;
		}
		else {
			restart_backoff = strtod(rbo, &endptr);
			if (*endptr != '\0') {
				log_msg(LOG_ERR, "Unable to parse restart backoff for %s\n", ssection.c_str());
				return;
			}
			if (restart_backoff < 1.0) {
				log_msg(LOG_ERR, "Invalid restart backoff value %lf for %s\n", restart_backoff, ssection.c_str());
				return;
			}
		}

		const char* rdri = get_value_multi(ini, "restart_delay_reset_interval", ssection.c_str(), section.c_str(), "general", "fdbmonitor", NULL);
		if (!rdri) {
			restart_delay_reset_interval = max_restart_delay;
		}
		else {
			restart_delay_reset_interval = strtoul(rdri, &endptr, 10);
			if (*endptr != '\0') {
				log_msg(LOG_ERR, "Unable to parse restart delay reset interval for %s\n", ssection.c_str());
				return;
			}
		}

		const char* q = get_value_multi(ini, "disable_lifecycle_logging", ssection.c_str(), section.c_str(), "general", NULL);
		if (q && !strcmp(q, "true"))
			quiet = true;

		const char* dwe = get_value_multi(ini, "delete_wd40_env", ssection.c_str(), section.c_str(), "general", NULL);
		if(dwe && !strcmp(dwe, "true")) {
			delete_wd40_env = true;
		}

		const char* kocc = get_value_multi(ini, "kill_on_configuration_change", ssection.c_str(), section.c_str(), "general", NULL);
		if(kocc && strcmp(kocc, "true")) {
			kill_on_configuration_change = false;
		}

		const char* binary = get_value_multi(ini, "command", ssection.c_str(), section.c_str(), "general", NULL);
		if (!binary) {
			log_msg(LOG_ERR, "Unable to resolve command for %s\n", ssection.c_str());
			return;
		}
		std::stringstream ss(binary);
		std::copy(std::istream_iterator<std::string> (ss), std::istream_iterator<std::string>(), std::back_inserter<std::vector<std::string>>(commands));

		const char* id_s = ssection.c_str() + strlen(section.c_str()) + 1;

		for (auto i : keys) {
			if (!strcmp(i.pItem, "command") || !strcmp(i.pItem, "restart_delay") || !strcmp(i.pItem, "initial_restart_delay") || !strcmp(i.pItem, "restart_backoff") ||
				!strcmp(i.pItem, "restart_delay_reset_interval") || !strcmp(i.pItem, "disable_lifecycle_logging") || !strcmp(i.pItem, "delete_wd40_env") ||
				!strcmp(i.pItem, "kill_on_configuration_change"))
			{
				continue;
			}

			std::string opt = get_value_multi(ini, i.pItem, ssection.c_str(), section.c_str(), "general", NULL);

			std::size_t pos = 0;

			while ((pos = opt.find("$ID", pos)) != opt.npos)
				opt.replace(pos, 3, id_s, strlen(id_s));

			commands.push_back(std::string("--").append(i.pItem).append("=").append(opt));
		}

		argv = new const char* [commands.size() + 1];
		int i = 0;
		for (auto itr : commands) {
			argv[i++] = strdup(itr.c_str());
		}
		argv[i] = NULL;
	}
	~Command() {
		delete[] argv;
		for ( auto p : pipes ) {
			if ( p[0] >= 0 && p[1] >= 0 ) {
				unmonitor_fd( fds, p[0] );
				close( p[0] );
				close( p[1] );
			}
		}
	}
	void update(const Command& other) {
		quiet = other.quiet;
		delete_wd40_env = other.delete_wd40_env;
		initial_restart_delay = other.initial_restart_delay;
		max_restart_delay = other.max_restart_delay;
		restart_backoff = other.restart_backoff;
		restart_delay_reset_interval = other.restart_delay_reset_interval;
		deconfigured = other.deconfigured;
		kill_on_configuration_change = other.kill_on_configuration_change;

		current_restart_delay = std::min<double>(max_restart_delay, current_restart_delay);
		current_restart_delay = std::max<double>(initial_restart_delay, current_restart_delay);
	}
	bool operator!=(const Command& rhs) {
		if (rhs.commands.size() != commands.size())
			return true;

		for (size_t i = 0; i < commands.size(); i++) {
			if (commands[i].compare(rhs.commands[i]) != 0)
				return true;
		}

		return false;
	}

	int get_and_update_current_restart_delay() {
		if(timer() - last_start >= restart_delay_reset_interval) {
			current_restart_delay = initial_restart_delay;
		}

		int jitter = randomInt(floor(-0.1 * current_restart_delay), ceil(0.1 * current_restart_delay));
		int delay = std::max<int>(0, round(current_restart_delay) + jitter);
		current_restart_delay = std::min<double>(max_restart_delay, restart_backoff * std::max(1.0, current_restart_delay));
		return delay;
	}
};

std::unordered_map<uint64_t, Command*> id_command;
std::unordered_map<pid_t, uint64_t> pid_id;
std::unordered_map<uint64_t, pid_t> id_pid;

enum { OPT_CONFFILE, OPT_LOCKFILE, OPT_DAEMONIZE, OPT_HELP };

CSimpleOpt::SOption g_rgOptions[] = {
	{ OPT_CONFFILE, "--conffile", SO_REQ_SEP },
	{ OPT_LOCKFILE, "--lockfile", SO_REQ_SEP },
	{ OPT_DAEMONIZE, "--daemonize", SO_NONE },
	{ OPT_HELP, "-?", SO_NONE },
	{ OPT_HELP, "-h", SO_NONE },
	{ OPT_HELP, "--help", SO_NONE },
	SO_END_OF_OPTIONS
};

void start_process(Command* cmd, uint64_t id, uid_t uid, gid_t gid, int delay, sigset_t* mask) {
	if (!cmd->argv)
		return;

	pid_t pid = fork();

	if (pid < 0) { /* fork error */
		log_err("fork", errno, "Failed to launch new %s process", cmd->argv[0]);
		return;
	} else if (pid == 0) { /* we are the child */
		/* remove signal handlers from parent */
		signal(SIGHUP, SIG_DFL);
		signal(SIGINT, SIG_DFL);
		signal(SIGTERM, SIG_DFL);

		if(cmd->delete_wd40_env) {
			/* remove WD40 environment variables */
			if(unsetenv("WD40_BV") || unsetenv("WD40_IS_MY_DADDY") || unsetenv("CONF_BUILD_VERSION")) {
				log_err("unsetenv", errno, "Failed to remove parent environment variables");
				exit(1);
			}
		}

		dup2( cmd->pipes[0][1], fileno(stdout) );
		dup2( cmd->pipes[1][1], fileno(stderr) );

#ifdef __linux__
		signal(SIGCHLD, SIG_DFL);

		sigprocmask(SIG_SETMASK, mask, NULL);

		/* death of our parent raises SIGHUP */
		prctl(PR_SET_PDEATHSIG, SIGHUP);
		if (getppid() == 1) /* parent already died before prctl */
			exit(0);
#endif

		if (delay)
			while ((delay = sleep(delay)) > 0) {}

		if (getegid() != gid)
			if (setgid(gid) != 0) {
				log_err("setgid", errno, "Failed to set GID to %d", gid);
				exit(1);
			}
		if (geteuid() != uid)
			if (setuid(uid) != 0) {
				log_err("setuid", errno, "Failed to set UID to %d", uid);
				exit(1);
			}

#ifdef __linux__
		/* death of our parent raises SIGHUP */
		/* although not documented to this effect, setting uid/gid
		   appears to reset PDEATHSIG */
		prctl(PR_SET_PDEATHSIG, SIGHUP);
		if (getppid() == 1) /* parent already died before prctl */
			exit(0);
#endif

		if (!cmd->quiet)
			log_msg(LOG_INFO, "Launching %s (%d) for %s\n", cmd->argv[0], getpid(), cmd->ssection.c_str());
		execv(cmd->argv[0], (char* const*)cmd->argv);
		log_err("execv", errno, "Failed to launch %s for %s", cmd->argv[0], cmd->ssection.c_str());
		_exit(0);
	}

	cmd->last_start = timer() + delay;
	pid_id[pid] = id;
	id_pid[id] = pid;
}

volatile int exit_signal = 0;

#ifdef __linux__
void signal_handler(int sig) {
	if (sig > exit_signal)
		exit_signal = sig;
}
#endif

volatile bool child_exited = false;

#ifdef __linux__
void child_handler(int sig) {
	child_exited = true;
}
#endif

void print_usage(const char* name) {
	printf(
		"FoundationDB Process Monitor " FDB_VT_PACKAGE_NAME " (v" FDB_VT_VERSION ")\n"
		"Usage: %s [OPTIONS]\n"
		"\n"
		"  --conffile CONFFILE\n"
		"                 The path of a file containing the connection string for the\n"
		"                 FoundationDB cluster. The default is\n"
		"                 `/etc/foundationdb/foundationdb.conf'.\n"
		"  --lockfile LOCKFILE\n"
		"                 The path of the mutual exclusion file for this instance of\n"
		"                 fdbmonitor. The default is `/var/run/fdbmonitor.pid'.\n"
		"  --daemonize    Background the fdbmonitor process.\n"
		"  -h, --help     Display this help and exit.\n", name);
}

bool argv_equal(const char** a1, const char** a2)
{
	int i = 0;

	while (a1[i] && a2[i]) {
		if (strcmp(a1[i], a2[i]))
			return false;
		i++;
	}

	if (a1[i] != NULL || a2[i] != NULL)
		return false;
	return true;
}

void kill_process(uint64_t id) {
	pid_t pid = id_pid[id];

	log_msg(LOG_INFO, "Killing process %d\n", pid);

	kill(pid, SIGTERM);
	waitpid(pid, NULL, 0);

	pid_id.erase(pid);
	id_pid.erase(id);
}

void load_conf(const char* confpath, uid_t &uid, gid_t &gid, sigset_t* mask, fdb_fd_set rfds, int* maxfd)
{
	log_msg(LOG_INFO, "Loading configuration %s\n", confpath);

	CSimpleIniA ini;
	ini.SetUnicode();

	SI_Error err = ini.LoadFile(confpath);
	if (err<0) {
		log_msg(LOG_ERR, "Unable to load configuration file %s (SI_Error: %d, errno: %d)\n", confpath, err, errno);
		return;
	}

	uid_t _uid;
	gid_t _gid;

	const char* user = ini.GetValue("fdbmonitor", "user", NULL);
	const char* group = ini.GetValue("fdbmonitor", "group", NULL);

	if (user) {
		errno = 0;
		struct passwd* pw = getpwnam(user);
		if (!pw) {
			log_err( "getpwnam", errno, "Unable to lookup user %s", user );
			return;
		}
		_uid = pw->pw_uid;
	} else
		_uid = geteuid();

	if (group) {
		errno = 0;
		struct group* gr = getgrnam(group);
		if (!gr) {
			log_err( "getgrnam", errno, "Unable to lookup group %s", group );
			return;
		}
		_gid = gr->gr_gid;
	} else
		_gid = getegid();

	/* Any change to uid or gid requires the process to be restarted to take effect */
	if (uid != _uid || gid != _gid) {
		std::vector<uint64_t> kill_ids;
		for (auto i : id_pid) {
			if(id_command[i.first]->kill_on_configuration_change) {
				kill_ids.push_back(i.first);
			}
		}
		for (auto i : kill_ids) {
			kill_process(i);
			delete id_command[i];
			id_command.erase(i);
		}
	}

	uid = _uid;
	gid = _gid;

	std::list<uint64_t> kill_ids;
	std::list<std::pair<uint64_t, Command*>> start_ids;

	for (auto i : id_pid) {
		if (ini.GetSectionSize(id_command[i.first]->ssection.c_str()) == -1) {
			/* Server on this port no longer configured; deconfigure it and kill it if required */
			log_msg(LOG_INFO, "Deconfigured %s\n", id_command[i.first]->ssection.c_str());

			id_command[i.first]->deconfigured = true;

			if(id_command[i.first]->kill_on_configuration_change) {
				kill_ids.push_back(i.first);
				delete id_command[i.first];
				id_command.erase(i.first);
			}
		} else {
			Command* cmd = new Command(ini, id_command[i.first]->section, i.first, rfds, maxfd);

			// If we just turned on 'kill_on_configuration_change', then kill the process to make sure we pick up any of its pending config changes
			if (*(id_command[i.first]) != *cmd || (cmd->kill_on_configuration_change && !id_command[i.first]->kill_on_configuration_change)) {
				log_msg(LOG_INFO, "Found new configuration for %s\n", id_command[i.first]->ssection.c_str());
				delete id_command[i.first];
				id_command[i.first] = cmd;

				if(id_command[i.first]->kill_on_configuration_change) {
					kill_ids.push_back(i.first);
					start_ids.push_back(std::make_pair(i.first, cmd));
				}
			} else {
				log_msg(LOG_INFO, "Updated configuration for %s\n", id_command[i.first]->ssection.c_str());
				id_command[i.first]->update(*cmd);
				delete cmd;
			}
		}
	}

	for (auto i : kill_ids)
		kill_process(i);

	for (auto i : start_ids) {
		start_process(i.second, i.first, uid, gid, 0, mask);
	}

	/* We've handled deconfigured sections, now look for newly
	   configured sections */
	CSimpleIniA::TNamesDepend sections;
	ini.GetAllSections(sections);
	for (auto i : sections) {
		if (auto dot = strrchr(i.pItem, '.')) {
			char* strtol_end;

			uint64_t id = strtoull(dot + 1, &strtol_end, 10);

			if (*strtol_end != '\0' || !(id > 0)) {
				log_msg(LOG_ERR, "Found bogus id in %s\n", i.pItem);
			} else {
				if (!id_pid.count(id)) {
					/* Found something we haven't yet started */
					log_msg(LOG_INFO, "Starting %s\n", i.pItem);
					std::string section(i.pItem, dot - i.pItem);
					id_command[id] = new Command(ini, section, id, rfds, maxfd);
					start_process(id_command[id], id, uid, gid, 0, mask);
				}
			}
		}
	}
}

/* cmd->pipes[pipe_idx] *must* be ready to read without blocking */
void read_child_output( Command* cmd, int pipe_idx, fdb_fd_set fds ) {
	char buf[4096];

	int len = read( cmd->pipes[pipe_idx][0], buf, 4096 );
	if ( len == -1 ) {
		if ( errno != EINTR ) {
			/* We shouldn't get EAGAIN or EWOULDBLOCK
			   here, and if it's not EINTR then all of
			   the other alternatives seem "bad". */
			log_err( "read", errno, "Error while reading from %s, no longer logging output", cmd->ssection.c_str() );
			unmonitor_fd( fds, cmd->pipes[pipe_idx][0] );
		}
		return;
	}

	// pipe_idx == 0 is stdout, pipe_idx == 1 is stderr
	int priority = (pipe_idx == 0) ? LOG_INFO : LOG_ERR;

	int start = 0;
	for ( int i = 0; i < len; i++ ) {
		if ( buf[i] == '\n' ) {
			log_msg( priority, "%s: %.*s", cmd->ssection.c_str(), i - start + 1, buf + start );
			start = i + 1;
		}
	}

	if ( start < len ) {
		log_msg( priority, "%s: %.*s\n", cmd->ssection.c_str(), len - start, buf + start );
	}
}

#ifdef __APPLE__
void watch_conf_file( int kq, int* conff_fd, const char* confpath ) {
	struct kevent ev;

	/* If already watching, drop it and close */
	if ( *conff_fd >= 0 ) {
		EV_SET( &ev, *conff_fd, EVFILT_VNODE, EV_DELETE, NOTE_WRITE | NOTE_ATTRIB, 0, NULL );
		kevent( kq, &ev, 1, NULL, 0, NULL );
		close( *conff_fd );
	}

	/* Open and watch */
	*conff_fd = open( confpath, O_EVTONLY );
	if ( *conff_fd >= 0 ) {
		EV_SET( &ev, *conff_fd, EVFILT_VNODE, EV_ADD | EV_CLEAR, NOTE_WRITE | NOTE_ATTRIB, 0, NULL );
		kevent( kq, &ev, 1, NULL, 0, NULL );
	}
}
#endif

#ifdef __linux__
void fdbmon_stat(const char *path, struct stat *path_stat, bool is_link) {
	int result = is_link ? lstat(path, path_stat) : stat(path, path_stat);
	if(result) {
		perror(is_link ? "lstat" : "stat");
		exit(1);
	}
}

std::unordered_map<int, std::unordered_set<std::string>> set_watches(std::string path, int ifd) {
	std::unordered_map<int, std::unordered_set<std::string>> additional_watch_wds;
	struct stat path_stat;

	if(path.size() < 2)
		return additional_watch_wds;

	int idx = 1;
	while(idx != std::string::npos) {
		idx = path.find_first_of('/', idx+1);
		std::string subpath = path.substr(0, idx);

		int level = 0;
		while(true) {
			if(level++ == 100) {
				log_msg(LOG_ERR, "Too many nested symlinks in path %s\n", path.c_str());
				exit(1);
			}

			fdbmon_stat(subpath.c_str(), &path_stat, true);
			if(!S_ISLNK(path_stat.st_mode)) {
				break;
			}

			std::string parent = parentDirectory(subpath);

			int wd = inotify_add_watch(ifd, parent.c_str(), IN_CREATE | IN_MOVED_TO);
			if (wd < 0) {
				perror("inotify_add_watch link");
				exit(1);
			}

			log_msg(LOG_INFO, "Watching parent directory of symlink %s (%d)\n", subpath.c_str(), wd);
			additional_watch_wds[wd].insert(subpath.substr(parent.size()+1));

			char buf[PATH_MAX+1];
			ssize_t len = readlink(subpath.c_str(), buf, PATH_MAX);
			if(len < 0) {
				perror("readlink");
				exit(1);
			}

			buf[len] = '\0';
			if(buf[0] == '/') {
				subpath = buf;
			}
			else {
				subpath = joinPath(parent, buf);
			}
		}
	}

	return additional_watch_wds;
}
#endif

int main(int argc, char** argv) {
	std::string lockfile = "/var/run/fdbmonitor.pid";
	std::string _confpath = "/etc/foundationdb/foundationdb.conf";

	std::vector<const char *> additional_watch_paths;

	CSimpleOpt args(argc, argv, g_rgOptions, SO_O_NOERR);

	while (args.Next()) {
		if (args.LastError() == SO_SUCCESS) {
			switch (args.OptionId()) {
				case OPT_CONFFILE:
					_confpath = args.OptionArg();
					break;
				case OPT_LOCKFILE:
					lockfile = args.OptionArg();
					break;
				case OPT_DAEMONIZE:
					daemonize = true;
					break;
				case OPT_HELP:
					print_usage(argv[0]);
					exit(0);
			}
		} else {
			print_usage(argv[0]);
			exit(1);
		}
	}

	log_msg(LOG_INFO, "Started FoundationDB Process Monitor " FDB_VT_PACKAGE_NAME " (v" FDB_VT_VERSION ")\n");

	// Modify _confpath to be absolute for further traversals
	if(!_confpath.empty() && _confpath[0] != '/') {
		char buf[PATH_MAX];
		if( !getcwd(buf, PATH_MAX) ) {
			perror("getcwd");
			exit(1);
		}

		_confpath = joinPath(buf, _confpath);
	}

	// Guaranteed (if non-NULL) to be an absolute path with no
	// symbolic link, /./ or /../ components
	const char *p = realpath(_confpath.c_str(), NULL);
	if (!p) {
		log_msg(LOG_ERR, "No configuration file at %s\n", _confpath.c_str());
		exit(1);
	}

	std::string confpath = p;

	// Will always succeed given an absolute path
	std::string confdir = parentDirectory(confpath);
	std::string conffile = confpath.substr(confdir.size()+1);

#ifdef __linux__
	// Watch for changes to our configuration file
	int ifd = inotify_init();
	if (ifd < 0) {
		perror("inotify_init");
		exit(1);
	}

	int conffile_wd = inotify_add_watch(ifd, confpath.c_str(), IN_CLOSE_WRITE);
	if (conffile_wd < 0) {
		perror("inotify_add_watch conf file");
		exit(1);
	} else {
		log_msg(LOG_INFO, "Watching config file %s\n", confpath.c_str());
	}

	int confdir_wd = inotify_add_watch(ifd, confdir.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO);
	if (confdir_wd < 0) {
		perror("inotify_add_watch conf dir");
		exit(1);
	} else {
		log_msg(LOG_INFO, "Watching config dir %s\n", confdir.c_str());
	}

	auto additional_watch_wds = set_watches(_confpath, ifd);
#endif

	/* fds we're blocking on via pselect or kevent */
	fdb_fd_set watched_fds;
	/* only linux needs this, but... */
	int maxfd = 0;

#ifdef __linux__
	fd_set rfds;
	watched_fds = &rfds;

	FD_ZERO(&rfds);
	FD_SET(ifd, &rfds);
	maxfd = ifd;

	int nfds = 0;
	fd_set srfds;
#endif

	CSimpleIniA* ini = NULL;

	if (daemonize) {
#ifdef __APPLE__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif
		if (daemon(0, 0)) {
#ifdef __APPLE__
#pragma GCC diagnostic pop
#endif
			perror("daemon");
			exit(1);
		}

		/* open syslog connection immediately, to be inherited by
		   forked children */
		openlog("fdbmonitor", LOG_PID | LOG_NDELAY, LOG_DAEMON);

		signal(SIGTSTP, SIG_IGN);
		signal(SIGTTOU, SIG_IGN);
		signal(SIGTTIN, SIG_IGN);

		/* new process group, no controlling terminal */
		/* unchecked since the only failure indicates we're already a
		 process group leader */
		setsid();
	}

	/* open and lock our lockfile for mutual exclusion */
	std::string lockfileDir = parentDirectory(abspath(lockfile));
	if(lockfileDir.size() == 0) {
		log_msg(LOG_ERR, "Unable to determine parent directory of lockfile %s\n", lockfile.c_str());
		exit(1);
	}

	if(mkdir(lockfileDir) < 0) {
		log_err("mkdir", errno, "Unable to create parent directory for lockfile %s", lockfile.c_str());
		exit(1);
	}

	int lockfile_fd = open(lockfile.c_str(), O_RDWR|O_CREAT, 0640);
	if (lockfile_fd < 0) {
		log_err("open", errno, "Unable to open fdbmonitor lockfile %s", lockfile.c_str());
		exit(1);
	}
	if (lockf(lockfile_fd, F_LOCK, 0) < 0) {
		log_err("lockf", errno, "Unable to lock fdbmonitor lockfile %s (is fdbmonitor already running?)", lockfile.c_str());
		exit(0);
	}

	if (chdir("/") < 0) {
		log_err("chdir", errno, "Unable to change working directory");
		exit(1);
	}

	/* write our pid to the lockfile for convenience */
	char pid_buf[16];
	snprintf(pid_buf, sizeof(pid_buf), "%d\n", getpid());
	ssize_t ign = write(lockfile_fd, pid_buf, strlen(pid_buf));

#ifdef __linux__
	/* attempt to do clean shutdown and remove lockfile when killed */
	signal(SIGHUP, signal_handler);
	signal(SIGINT, signal_handler);
	signal(SIGTERM, signal_handler);
#elif defined(__APPLE__)
	int kq = kqueue();
	if ( kq < 0 ) {
		log_err( "kqueue", errno, "Unable to create kqueue" );
		exit(1);
	}
	watched_fds = kq;

	signal(SIGHUP, SIG_IGN);
	signal(SIGINT, SIG_IGN);
	signal(SIGTERM, SIG_IGN);

	struct kevent ev;

	EV_SET( &ev, SIGHUP, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
	kevent( kq, &ev, 1, NULL, 0, NULL );
	EV_SET( &ev, SIGINT, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
	kevent( kq, &ev, 1, NULL, 0, NULL );
	EV_SET( &ev, SIGTERM, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
	kevent( kq, &ev, 1, NULL, 0, NULL );
	EV_SET( &ev, SIGCHLD, EVFILT_SIGNAL, EV_ADD, 0, 0, NULL);
	kevent( kq, &ev, 1, NULL, 0, NULL );

	int confd_fd = open(confdir.c_str(), O_EVTONLY);

	// Watch the directory holding the configuration file
	EV_SET( &ev, confd_fd, EVFILT_VNODE, EV_ADD | EV_CLEAR, NOTE_WRITE, 0, NULL );
	kevent( kq, &ev, 1, NULL, 0, NULL );

	int conff_fd = -1;
	watch_conf_file( kq, &conff_fd, confpath.c_str() );
#endif

#ifdef __linux__
	signal(SIGCHLD, child_handler);
#endif

	uid_t uid = 0;
	gid_t gid = 0;

	sigset_t normal_mask, full_mask;
	sigfillset(&full_mask);

#ifdef __linux__
	/* normal will be restored in our main loop in the call to
	   pselect, but none blocks all signals while processing events */
	sigprocmask(SIG_SETMASK, &full_mask, &normal_mask);
#elif defined(__APPLE__)
	sigprocmask(0, NULL, &normal_mask);
#endif

#ifdef __APPLE__
	struct stat st_buf;
	struct timespec mtimespec;

	if (stat(confpath.c_str(), &st_buf) < 0)
		perror("stat");

	memcpy(&mtimespec, &(st_buf.st_mtimespec), sizeof(struct timespec));
#endif

	load_conf(confpath.c_str(), uid, gid, &normal_mask, watched_fds, &maxfd);

	while (1) {
#ifdef __APPLE__
		int nev = kevent( kq, NULL, 0, &ev, 1, NULL );

		if (nev > 0) {
			switch (ev.filter) {
				case EVFILT_VNODE:
					struct kevent timeout;
					// This could be the conf dir or conf file
					if ( ev.ident == confd_fd ) {
						/* Changes in the directory holding the conf file; schedule a future timeout to reset watches and reload the conf */
						EV_SET( &timeout, 1, EVFILT_TIMER, EV_ADD | EV_ONESHOT, 0, 200, NULL );
						kevent( kq, &timeout, 1, NULL, 0, NULL );
					} else {
						/* Direct writes to the conf file; reload! */
						load_conf( confpath.c_str(), uid, gid, &normal_mask, watched_fds, &maxfd );
						watch_conf_file( kq, &conff_fd, confpath.c_str() );
					}
					break;
				case EVFILT_TIMER:
					watch_conf_file( kq, &conff_fd, confpath.c_str() );
					load_conf( confpath.c_str(), uid, gid, &normal_mask, watched_fds, &maxfd );
					break;
				case EVFILT_SIGNAL:
					switch (ev.ident) {
						case SIGHUP:
						case SIGINT:
						case SIGTERM:
							exit_signal = ev.ident;
							break;
						case SIGCHLD:
							child_exited = true;
							break;
						default:
							break;
					}
					break;
				case EVFILT_READ:
					Command* cmd = (Command*)ev.udata;
					for ( int i = 0; i < 2 ; i++ ) {
						if ( ev.ident == cmd->pipes[i][0] ) {
							read_child_output( cmd, i, watched_fds );
						}
					}
					break;
			}
		}
#endif
		/* select() could have returned because received an exit signal */
		if (exit_signal > 0) {
			switch(exit_signal) {
				case SIGHUP:
					log_msg(LOG_INFO, "Received signal %d (%s), doing nothing\n", exit_signal, strsignal(exit_signal));
					break;
				case SIGINT:
				case SIGTERM:
					log_msg(LOG_NOTICE, "Received signal %d (%s), shutting down\n", exit_signal, strsignal(exit_signal));

					/* Unblock signals */
					signal(SIGCHLD, SIG_IGN);
					sigprocmask(SIG_SETMASK, &normal_mask, NULL);

					/* Send SIGHUP to all child processes */
					kill(0, SIGHUP);

					/* Wait for all child processes (says POSIX.1-2001) */
					/* POSIX.1-2001 specifies that if the disposition of SIGCHLD is set to SIG_IGN, then children that terminate do not become zombies and a call to wait()
					   will block until all children have terminated, and then fail with errno set to ECHILD */
					wait(NULL);

					unlink(lockfile.c_str());
					exit(0);
				default:
					break;
			}
			exit_signal = 0;
		}

#ifdef __linux__
		/* select() could have returned because we have a fd ready to
		   read (child output or inotify on conf file) */
		if (nfds > 0) {
			int len, i = 0;

			char buf[4096];

			for ( auto itr : id_command ) {
				for ( int i = 0; i < 2; i++ ) {
					if ( FD_ISSET( (itr.second)->pipes[i][0], &srfds ) ) {
						read_child_output( itr.second, i, watched_fds );
					}
				}
			}

			if ( FD_ISSET( ifd, &srfds ) ) {
				len = read(ifd, buf, 4096);
				if (len < 0)
					log_err("read", errno, "Error reading inotify message");

				bool reload = false;
				bool reload_additional_watches = false;

				while (i < len) {
					struct inotify_event* event = (struct inotify_event*) &buf[i];

					auto search = additional_watch_wds.find(event->wd);
					if(event->wd != conffile_wd) {
						if(search != additional_watch_wds.end() && event->len && search->second.count(event->name)) {
							log_msg(LOG_INFO, "Changes detected on watched symlink `%s': (%d, %#010x)\n", event->name, event->wd, event->mask);

							char *redone_confpath = realpath(_confpath.c_str(), NULL);
							if(!redone_confpath) {
								log_msg(LOG_INFO, "Error calling realpath on `%s', continuing...\n", _confpath.c_str());
								perror("realpath");
								// exit(1);
								i += sizeof(struct inotify_event) + event->len;
								continue;
							}

							confpath = redone_confpath;

							// Will always succeed given an absolute path
							confdir = parentDirectory(confpath);
							conffile = confpath.substr(confdir.size());

							// Remove all the old watches
							for(auto wd : additional_watch_wds) {
								if(inotify_rm_watch(ifd, wd.first) < 0) {
									// perror("inotify_rm_watch symlink");
									// exit(1);
									log_msg(LOG_INFO, "Could not remove inotify watch %d, continuing...\n", wd.first);
								}
							}

							reload = true;
							reload_additional_watches = true;
							break;
						}
						else if(event->wd == confdir_wd && event->len && conffile == event->name) {
							reload = true;
						}
					}

					else if (event->wd == conffile_wd) {
						reload = true;
					}

					i += sizeof(struct inotify_event) + event->len;
				}

				if (reload) {
					if(inotify_rm_watch(ifd, confdir_wd) < 0) {
						log_msg(LOG_INFO, "Could not remove inotify conf dir watch, continuing...\n");
					}
					if(inotify_rm_watch(ifd, conffile_wd) < 0) {
						log_msg(LOG_INFO, "Could not remove inotify conf file watch, continuing...\n");
					}
					conffile_wd = inotify_add_watch(ifd, confpath.c_str(), IN_CLOSE_WRITE);
					if (conffile_wd < 0) {
						perror("inotify_add_watch conf file");
						exit(1); // Deleting the conf file causes fdbmonitor to terminate
					} else {
						log_msg(LOG_INFO, "Watching config file %s\n", confpath.c_str());
					}

					confdir_wd = inotify_add_watch(ifd, confdir.c_str(), IN_CLOSE_WRITE | IN_MOVED_TO);
					if (confdir_wd < 0) {
						perror("inotify_add_watch conf dir");
						exit(1);
					} else {
						log_msg(LOG_INFO, "Watching config dir %s (%d)\n", confdir.c_str(), confdir_wd);
					}

					if(reload_additional_watches) {
						additional_watch_wds = set_watches(_confpath, ifd);
					}

					load_conf(confpath.c_str(), uid, gid, &normal_mask, &rfds, &maxfd);
				}
			}
		}
#endif

		/* select() could have returned because of one or more
		   SIGCHLDs */
		if (child_exited) {
			pid_t pid;
			int child_status;
			while ((pid = waitpid(-1, &child_status, WNOHANG))) {
				if (pid < 0) {
					if (errno != ECHILD)
						log_err("waitpid", errno, "Error while waiting for child process");
					break;
				}

				uint64_t id = pid_id[pid];
				Command* cmd = id_command[id];

				pid_id.erase(pid);
				id_pid.erase(id);

				if(cmd->deconfigured) {
					delete cmd;
					id_command.erase(id);
				}
				else {
					int delay = cmd->get_and_update_current_restart_delay();
					if (!cmd->quiet) {
						if (WIFEXITED(child_status)) {
							int priority = (WEXITSTATUS(child_status) == 0) ? LOG_NOTICE : LOG_ERR;
							log_msg(priority, "Process %d exited %d, restarting %s in %d seconds\n", pid, WEXITSTATUS(child_status), cmd->ssection.c_str(), delay);
						} else if (WIFSIGNALED(child_status))
							log_msg(LOG_NOTICE, "Process %d terminated by signal %d, restarting %s in %d seconds\n", pid, WTERMSIG(child_status), cmd->ssection.c_str(), delay);
						else
							log_msg(LOG_WARNING, "Process %d exited for unknown reason, restarting %s in %d seconds\n", pid, cmd->ssection.c_str(), delay);
					}

					start_process(cmd, id, uid, gid, delay, &normal_mask);
				}
			}
			child_exited = false;
		}

#ifdef __linux__
		/* Block until something interesting happens (while atomically
		   unblocking signals) */
		srfds = rfds;
		nfds = pselect(maxfd+1, &srfds, NULL, NULL, NULL, &normal_mask);
#endif
	}
}
