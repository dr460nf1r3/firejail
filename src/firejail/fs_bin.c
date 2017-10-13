/*
 * Copyright (C) 2014-2017 Firejail Authors
 *
 * This file is part of firejail project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/
#include "firejail.h"
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <glob.h>

static char *paths[] = {
	"/usr/local/bin",
	"/usr/bin",
	"/bin",
	"/usr/games",
	"/usr/local/games",
	"/usr/local/sbin",
	"/usr/sbin",
	"/sbin",
	NULL
};

// return 1 if found, 0 if not found
static char *check_dir_or_file(const char *name) {
	assert(name);

	struct stat s;
	char *fname = NULL;

	int i = 0;
	while (paths[i]) {
		// private-bin-no-local can be disabled in /etc/firejail/firejail.config
		if (checkcfg(CFG_PRIVATE_BIN_NO_LOCAL) && strstr(paths[i], "local/")) {
			i++;
			continue;
		}

		// check file
		if (asprintf(&fname, "%s/%s", paths[i], name) == -1)
			errExit("asprintf");
		if (arg_debug)
			printf("Checking %s/%s\n", paths[i], name);
		if (stat(fname, &s) == 0 && !S_ISDIR(s.st_mode)) { // do not allow directories
			// check symlink to firejail executable in /usr/local/bin
			if (strcmp(paths[i], "/usr/local/bin") == 0 && is_link(fname)) {
				/* coverity[toctou] */
				char *actual_path = realpath(fname, NULL);
				if (actual_path) {
					char *ptr = strstr(actual_path, "/firejail");
					if (ptr && strlen(ptr) == strlen("/firejail")) {
						if (arg_debug)
							printf("firejail exec symlink detected\n");
						free(actual_path);
						free(fname);
						fname = NULL;
						i++;
						continue;
					}
					free(actual_path);
				}

			}
			break; // file found
		}

		free(fname);
		fname = NULL;
		i++;
	}

	if (!fname) {
		if (arg_debug)
			fwarning("file %s not found\n", name);
		return NULL;
	}

	free(fname);
	return paths[i];
}

// return 1 if the file is in paths[]
static int valid_full_path_file(const char *name) {
	assert(name);

	char *full_name = realpath(name, NULL);
	if (!full_name)
		goto errexit;
	char *fname = strrchr(full_name, '/');
	if (!fname)
		goto errexit;
	if (*(++fname) == '\0')
		goto errexit;

	int i = 0;
	int found = 0;
	while (paths[i]) {
		// private-bin-no-local can be disabled in /etc/firejail/firejail.config
		if (checkcfg(CFG_PRIVATE_BIN_NO_LOCAL) && strstr(paths[i], "local/")) {
			i++;
			continue;
		}

		// check file
		char *full_name2;
		if (asprintf(&full_name2, "%s/%s", paths[i], fname) == -1)
			errExit("asprintf");

		if (strcmp(full_name, full_name2) == 0) {
			free(full_name2);
			found = 1;
			break;
		}

		free(full_name2);
		i++;
	}

	if (!found)
		goto errexit;

	free(full_name);
	return 1;

errexit:
	if (arg_debug)
		fwarning("file %s not found\n", name);
	if (full_name)
		free(full_name);
	return 0;
}

static void duplicate(char *fname, FILE *fplist) {
	assert(fname);

	if (*fname == '~' || strstr(fname, "..")) {
		fprintf(stderr, "Error: \"%s\" is an invalid filename\n", fname);
		exit(1);
	}
	invalid_filename(fname);

	char *full_path;
	if (*fname == '/') {
		// If the absolute filename is indicated, directly use it. This
		// is required for the following three cases:
		//  - if user's $PATH order is not the same as the above
		//    paths[] variable order
		//  - if for example /usr/bin/which is a symlink to /bin/which,
		//    because in this case the result is a symlink pointing to
		//    itself due to the file name being the same.

		if (!valid_full_path_file(fname))
			return;

		full_path = strdup(fname);
		if (!full_path)
			errExit("strdup");
	}
	else {
		// Find the standard directory (by looping through paths[])
		// where the filename fname is located
		char *path = check_dir_or_file(fname);
		if (!path)
			return;
		if (asprintf(&full_path, "%s/%s", path, fname) == -1)
			errExit("asprintf");
	}

	if (fplist)
		fprintf(fplist, "%s\n", full_path);

	// copy the file
	if (checkcfg(CFG_FOLLOW_SYMLINK_PRIVATE_BIN))
		sbox_run(SBOX_ROOT| SBOX_SECCOMP, 4, PATH_FCOPY, "--follow-link", full_path, RUN_BIN_DIR);
	else {
		// if full_path is simlink, and the link is in our path, copy both
		if (is_link(full_path)) {
			char *actual_path = realpath(full_path, NULL);
			if (actual_path) {
				if (valid_full_path_file(actual_path))
					sbox_run(SBOX_ROOT| SBOX_SECCOMP, 3, PATH_FCOPY, actual_path, RUN_BIN_DIR);
				free(actual_path);
			}
		}

		sbox_run(SBOX_ROOT| SBOX_SECCOMP, 3, PATH_FCOPY, full_path, RUN_BIN_DIR);
	}

	fs_logger2("clone", fname);
	free(full_path);
}

static void globbing(char *fname, FILE *fplist) {
	assert(fname);

	// go directly to duplicate() if no globbing char is present - see man 7 glob
	if (strrchr(fname, '*') == NULL &&
	    strrchr(fname, '[') == NULL &&
	    strrchr(fname, '?') == NULL)
		return duplicate(fname, fplist);

	// loop through paths[]
	int i = 0;
	while (paths[i]) {
		// private-bin-no-local can be disabled in /etc/firejail/firejail.config
		if (checkcfg(CFG_PRIVATE_BIN_NO_LOCAL) && strstr(paths[i], "local/")) {
			i++;
			continue;
		}

		// check file
		char *pattern;
		if (asprintf(&pattern, "%s/%s", paths[i], fname) == -1)
			errExit("asprintf");

		// globbing
		glob_t globbuf;
		int globerr = glob(pattern, GLOB_NOCHECK | GLOB_NOSORT | GLOB_PERIOD, NULL, &globbuf);
		if (globerr) {
			fprintf(stderr, "Error: failed to glob private-bin pattern %s\n", pattern);
			exit(1);
		}

		size_t j;
		for (j = 0; j < globbuf.gl_pathc; j++) {
			assert(globbuf.gl_pathv[j]);
			// testing for GLOB_NOCHECK - no pattern mached returns the origingal pattern
			if (strcmp(globbuf.gl_pathv[j], pattern) == 0)
				continue;

			duplicate(globbuf.gl_pathv[j], fplist);
		}

		globfree(&globbuf);
		free(pattern);
		i++;
	}
}

void fs_private_bin_list(void) {
	char *private_list = cfg.bin_private_keep;
	assert(private_list);

	// create /run/firejail/mnt/bin directory
	mkdir_attr(RUN_BIN_DIR, 0755, 0, 0);

	if (arg_debug)
		printf("Copying files in the new bin directory\n");

	// copy the list of files in the new home directory
	char *dlist = strdup(private_list);
	if (!dlist)
		errExit("strdup");

	// save a list of private-bin files in order to bring in private-libs later
	FILE *fplist = NULL;
	if (arg_private_lib) {
		fplist = fopen(RUN_LIB_BIN, "w");
		if (!fplist)
			errExit("fopen");
	}

	char *ptr = strtok(dlist, ",");
	globbing(ptr, fplist);
	while ((ptr = strtok(NULL, ",")) != NULL)
		globbing(ptr, fplist);
	free(dlist);
	fs_logger_print();
	if (fplist)
		fclose(fplist);

	// mount-bind
	int i = 0;
	while (paths[i]) {
		struct stat s;
		if (stat(paths[i], &s) == 0) {
			if (arg_debug)
				printf("Mount-bind %s on top of %s\n", RUN_BIN_DIR, paths[i]);
			if (mount(RUN_BIN_DIR, paths[i], NULL, MS_BIND|MS_REC, NULL) < 0)
				errExit("mount bind");
			fs_logger2("tmpfs", paths[i]);
			fs_logger2("mount", paths[i]);
		}
		i++;
	}
}
