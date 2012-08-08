/* -*- c-set-style: "K&R"; c-basic-offset: 8 -*-
 *
 * This file is part of PRoot.
 *
 * Copyright (C) 2010, 2011, 2012 STMicroelectronics
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA.
 */

#define _GNU_SOURCE    /* get_current_dir_name(3), */
#include <stdio.h>     /* printf(3), */
#include <string.h>    /* string(3), */
#include <stdlib.h>    /* exit(3), strtol(3), */
#include <stdbool.h>   /* bool, true, false, */
#include <assert.h>    /* assert(3), */
#include <unistd.h>    /* acess(2), pipe(2), dup2(2), */
#include <sys/wait.h>  /* wait(2), */
#include <sys/types.h> /* stat(2), */
#include <sys/stat.h>  /* stat(2), */
#include <unistd.h>    /* stat(2), */
#include <errno.h>     /* errno(3), */

#include "cli.h"
#include "config.h"
#include "path/binding.h"
#include "notice.h"
#include "path/path.h"
#include "tracee/event.h"
#include "tracee/tracee.h"
#include "execve/ldso.h"
#include "build.h"

static void handle_option_r(char *value)
{
	config.guest_rootfs = value;
}

struct binding {
	const char *host;
	const char *guest;
	bool must_exist;

	struct binding *next;
};

static struct binding *bindings = NULL;

static void new_binding2(const char *host, const char *guest, bool must_exist)
{
	struct binding *binding;

	binding = malloc(sizeof (struct binding));
	if (binding == NULL) {
		notice(WARNING, SYSTEM, "malloc()");
		return;
	}

	binding->host  = host;
	binding->guest = guest;
	binding->must_exist = must_exist;
	binding->next = bindings;
	bindings = binding;
}

static void new_binding(char *value, bool must_exist)
{
	char *ptr = strchr(value, ':');
	if (ptr != NULL) {
		*ptr = '\0';
		ptr++;
	}

	/* Expand environment variables like $HOME.  */
	if (value[0] == '$' && getenv(&value[1]))
		value = getenv(&value[1]);

	new_binding2(value, ptr, must_exist);
}

static void handle_option_b(char *value)
{
	new_binding(value, true);
}

/**
 * Return a calloc-ed buffer that contains the full path to @command.
 *
 * This function always returns something consistent or die trying.
 */
static char *which(char *const command)
{
	char *const argv[3] = { "which", command, NULL };
	char which_output[PATH_MAX];
	int pipe_fd[2];
	char *path;
	int status;
	pid_t pid;

	status = pipe(pipe_fd);
	if (status < 0)
		notice(ERROR, SYSTEM, "pipe()");

	pid = fork();
	switch (pid) {
	case -1:
		notice(ERROR, SYSTEM, "fork()");

	case 0: /* child */
		close(pipe_fd[0]); /* "read" end */

		/* Replace the standard output with the "write" end of
		 * the pipe.  */
		status = dup2(pipe_fd[1], STDOUT_FILENO);
		if (status < 0)
			notice(ERROR, SYSTEM, "dup2()");

		execvp(argv[0], argv);
		notice(ERROR, SYSTEM, "can't execute `%s %s`", argv[0], command);

	default: /* parent */
		close(pipe_fd[1]); /* "write" end */

		status = read(pipe_fd[0], which_output, PATH_MAX - 1);
		if (status < 0)
			notice(ERROR, SYSTEM, "read()");
		if (status == 0)
			notice(ERROR, USER, "%s: command not found", command);
		assert(status < PATH_MAX);
		which_output[status - 1] = '\0'; /* overwrite "\n" */

		close(pipe_fd[0]); /* "read" end */

		pid = wait(&status);
		if (pid < 0)
			notice(ERROR, SYSTEM, "wait()");

		if (status != 0)
			notice(ERROR, USER, "`%s %s` returned an error", argv[0], command);
		break;
	}

	path = realpath(which_output, NULL);
	if (!path)
		notice(ERROR, SYSTEM, "realpath(\"%s\")", which_output);

	return path;
}

static void handle_option_q(char *value)
{
	size_t nb_args;
	char *ptr;
	int i;

	nb_args = 0;
	ptr = value;
	while (1) {
		nb_args++;

		/* Keep consecutive non-space characters.  */
		while (*ptr != ' ' && *ptr != '\0')
			ptr++;

		/* End-of-string ?  */
		if (*ptr == '\0')
			break;

		/* Skip consecutive space separators.  */
		while (*ptr == ' ' && *ptr != '\0')
			ptr++;

		/* End-of-string ?  */
		if (*ptr == '\0')
			break;
	}

	config.qemu = calloc(nb_args + 1, sizeof(char *));
	if (!config.qemu)
		notice(ERROR, SYSTEM, "calloc()");

	i = 0;
	ptr = value;
	while (1) {
		config.qemu[i] = ptr;
		i++;

		/* Keep consecutive non-space characters.  */
		while (*ptr != ' ' && *ptr != '\0')
			ptr++;

		/* End-of-string ?  */
		if (*ptr == '\0')
			break;

		/* Remove consecutive space separators.  */
		while (*ptr == ' ' && *ptr != '\0')
			*ptr++ = '\0';

		/* End-of-string ?  */
		if (*ptr == '\0')
			break;
	}
	assert(i == nb_args);

	config.qemu[0] = which(config.qemu[0]);
	config.qemu[nb_args] = NULL;

	config.host_rootfs = "/host-rootfs";
	new_binding2("/", config.host_rootfs, true);
	new_binding2("/dev/null", "/etc/ld.so.preload", false);
}

static void handle_option_w(char *value)
{
	config.initial_cwd = value;
}

static void handle_option_u(char *value)
{
	config.allow_unknown_syscalls = true;
}

static void handle_option_k(char *value)
{
	config.kernel_release = value;
}

static void handle_option_0(char *value)
{
	config.fake_id0 = true;
}

static void handle_option_v(char *value)
{
	char *end_ptr = NULL;

	errno = 0;
	config.verbose_level = strtol(value, &end_ptr, 10);
	if (errno != 0 || end_ptr == value)
		notice(ERROR, USER, "option `-v` expects an integer value.");
}

static void handle_option_V(char *value)
{
	printf("PRoot %s: %s.\n", version, subtitle);
	printf("%s\n", colophon);
	exit(EXIT_SUCCESS);
}

static void print_usage(bool);
static void handle_option_h(char *value)
{
	print_usage(true);
	exit(EXIT_SUCCESS);
}

static void handle_option_B(char *value)
{
	int i;
	for (i = 0; recommended_bindings[i] != NULL; i++)
		new_binding(recommended_bindings[i], false);
}

static void handle_option_Q(char *value)
{
	handle_option_q(value);
	handle_option_B(NULL);
}

static void handle_option_W(char *value)
{
	handle_option_w(".");
	handle_option_b(".");
}

#define NB_OPTIONS (sizeof(options) / sizeof(struct option))

/**
 * Print a (@detailed) usage of PRoot.
 */
static void print_usage(bool detailed)
{
	const char *current_class = "none";
	int i, j;

#define DETAIL(a) if (detailed) a

	DETAIL(printf("PRoot %s: %s.\n\n", version, subtitle));
	printf("Usage:\n  %s\n", synopsis);
	DETAIL(printf("\n"));

	for (i = 0; i < NB_OPTIONS; i++) {
		for (j = 0; ; j++) {
			struct argument *argument = &(options[i].arguments[j]);

			if (!argument->name || (!detailed && j != 0)) {
				DETAIL(printf("\n"));
				printf("\t%s\n", options[i].description);
				if (detailed) {
					if (options[i].detail[0] != '\0')
						printf("\n%s\n\n", options[i].detail);
					else
						printf("\n");
				}
				break;
			}

			if (strcmp(options[i].class, current_class) != 0) {
				current_class = options[i].class;
				printf("%s:\n", current_class);
			}

			if (j == 0)
				printf("  %s", argument->name);
			else
				printf(", %s", argument->name);

			if (argument->separator != '\0')
				printf("%c*%s*", argument->separator, argument->value);
			else if (!detailed)
				printf("\t");
		}
	}

	if (detailed)
		printf("%s\n", colophon);
}

static char *default_command[] = { "/bin/sh", NULL };

static void print_execve_help(const char *argv0)
{
	notice(WARNING, SYSTEM, "execv(\"%s\")", argv0);
	notice(INFO, USER, "possible causes:\n"
"  * <program> is a script but its interpreter (eg. /bin/sh) was not found;\n"
"  * <program> is an ELF but its interpreter (eg. ld-linux.so) was not found;\n"
"  * <program> is a foreign binary but no <qemu> was specified;\n"
"  * <qemu> does not work correctly (if specified).");
}

static void error_separator(struct argument *argument)
{
	if (argument->separator == '\0')
		notice(ERROR, USER,
			"option '%s' expects no value.",
			argument->name);
	else
		notice(ERROR, USER,
			"option '%s' and its value must be separated by '%c'.",
			argument->name,
			argument->separator);
}

int main(int argc, char *argv[])
{
	option_handler_t handler = NULL;
	int i, j, k;
	int status;
	pid_t pid = 0;

	if (argc == 1) {
		print_usage(false);
		exit(EXIT_FAILURE);
	}

	for (i = 1; i < argc; i++) {
		char *arg = argv[i];

		/* The current argument is the value of a short option.  */
		if (handler != NULL) {
			handler(arg);
			handler = NULL;
			continue;
		}

		if (arg[0] != '-')
			break; /* End of PRoot options. */

		for (j = 0; j < NB_OPTIONS; j++) {
			struct option *option = &options[j];

			/* A given option has several aliases.  */
			for (k = 0; ; k++) {
				struct argument *argument;
				size_t length;

				argument = &option->arguments[k];

				/* End of aliases for this option.  */
				if (!argument->name)
					break;

				length = strlen(argument->name);
				if (strncmp(arg, argument->name, length) != 0)
					continue;

				/* Avoid ambiguities.  */
				if (strlen(arg) > length
				    && arg[length] != argument->separator)
					error_separator(argument);

				/* No option value.  */
				if (!argument->value) {
					option->handler(arg);
					goto known_option;
				}

				/* Value coalesced with to its option.  */
				if (argument->separator == arg[length]) {
					assert(strlen(arg) >= length);
					option->handler(&arg[length + 1]);
					goto known_option;
				}

				/* Avoid ambiguities.  */
				if (argument->separator != ' ')
					error_separator(argument);

				/* Short option with a separated value.  */
				handler = option->handler;
				goto known_option;
			}
		}

		notice(ERROR, USER, "unknown option '%s'.", arg);

	known_option:
		if (handler != NULL && i == argc - 1)
			notice(ERROR, USER,
				"missing value for option '%s'.", arg);
	}

	/* When no guest rootfs were specified: if the first bare
	 * option is a directory, then the old command-line interface
	 * (similar to the chroot one) is expected.  Otherwise this is
	 * the new command-line interface where the default guest
	 * rootfs is "/".
	 */
	if (config.guest_rootfs == NULL) {
		struct stat buf;

		status = stat(argv[i], &buf);
		if (status == 0 && S_ISDIR(buf.st_mode))
			config.guest_rootfs = argv[i++];
		else
			config.guest_rootfs = "/";
	}

	config.guest_rootfs = realpath(config.guest_rootfs, NULL);
	if (config.guest_rootfs == NULL)
		notice(ERROR, SYSTEM, "realpath(\"%s\")", config.guest_rootfs);

	/* Bindings can be registered once config.guest_rootfs is
	 * correctly initialized.  */
	while (bindings != NULL) {
		struct binding *next;

		bind_path(bindings->host, bindings->guest, bindings->must_exist);

		next = bindings->next;
		free(bindings);
		bindings = next;
	}

	if (i < argc)
		config.command = &argv[i];
	else
		config.command = default_command;

	if (config.verbose_level)
		print_config();

	status = pid
		? attach_process(pid)
		: launch_process();
	if (!status) {
		print_execve_help(config.command[0]);
		exit(EXIT_FAILURE);
	}

	exit(event_loop());
}
