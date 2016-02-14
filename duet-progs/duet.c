/*
 * Copyright (C) 2014 George Amvrosiadis.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#define _GNU_SOURCE
#include "commands.h"
#include "version.h"

static const char * const duet_cmd_group_usage[] = {
	"duet [--help] [--version] <group> [<group>...] <command> [<args>]",
	NULL
};

static const char duet_cmd_group_info[] =
	"Use --help as an argument for information on a specific group or command.";

static char argv0_buf[ARGV0_BUF_SIZE] = "duet";

static inline const char *skip_prefix(const char *str, const char *prefix)
{
	size_t len = strlen(prefix);
	return strncmp(str, prefix, len) ? NULL : str + len;
}

int prefixcmp(const char *str, const char *prefix)
{
	for (; ; str++, prefix++)
		if (!*prefix)
			return 0;
		else if (*str != *prefix)
			return (unsigned char)*prefix - (unsigned char)*str;
}

static int parse_one_token(const char *arg, const struct cmd_group *grp,
			   const struct cmd_struct **cmd_ret)
{
	const struct cmd_struct *cmd = grp->commands;
	const struct cmd_struct *abbrev_cmd = NULL, *ambiguous_cmd = NULL;

	for (; cmd->token; cmd++) {
		const char *rest;

		rest = skip_prefix(arg, cmd->token);
		if (!rest) {
			if (!prefixcmp(cmd->token, arg)) {
				if (abbrev_cmd) {
					/*
					 * If this is abbreviated, it is
					 * ambiguous. So when there is no
					 * exact match later, we need to
					 * error out.
					 */
					ambiguous_cmd = abbrev_cmd;
				}
				abbrev_cmd = cmd;
			}
			continue;
		}
		if (*rest)
			continue;

		*cmd_ret = cmd;
		return 0;
	}

	if (ambiguous_cmd)
		return -2;

	if (abbrev_cmd) {
		*cmd_ret = abbrev_cmd;
		return 0;
	}

	return -1;
}

static const struct cmd_struct *
parse_command_token(const char *arg, const struct cmd_group *grp)
{
	const struct cmd_struct *cmd = NULL;

	switch(parse_one_token(arg, grp, &cmd)) {
	case -1:
		help_unknown_token(arg, grp);
	case -2:
		help_ambiguous_token(arg, grp);
	}

	return cmd;
}

static void handle_help_options_next_level(const struct cmd_struct *cmd,
		int fd, int argc, char **argv)
{
	if (argc < 2)
		return;

	if (!strcmp(argv[1], "--help")) {
		if (cmd->next) {
			argc--;
			argv++;
			help_command_group(cmd->next, fd, argc, argv);
		} else {
			usage_command(cmd, 1, 0);
		}

		exit(0);
	}
}

static void fixup_argv0(char **argv, const char *token)
{
	int len = strlen(argv0_buf);

	snprintf(argv0_buf + len, sizeof(argv0_buf) - len, " %s", token);
	argv[0] = argv0_buf;
}

int handle_command_group(const struct cmd_group *grp, int fd, int argc,
			 char **argv)

{
	const struct cmd_struct *cmd;

	argc--;
	argv++;
	if (argc < 1) {
		usage_command_group(grp, 0, 0);
		exit(1);
	}

	cmd = parse_command_token(argv[0], grp);

	handle_help_options_next_level(cmd, fd, argc, argv);

	fixup_argv0(argv, cmd->token);
	return cmd->fn(fd, argc, argv);
}

int check_argc_exact(int nargs, int expected)
{
	if (nargs < expected)
		fprintf(stderr, "%s: too few arguments\n", argv0_buf);
	if (nargs > expected)
		fprintf(stderr, "%s: too many arguments\n", argv0_buf);

	return nargs != expected;
}

static const struct cmd_group duet_cmd_group;

static const char * const cmd_help_usage[] = {
	"duet help [--full]",
	"Display help information",
	"",
	"--full     display detailed help on every command",
	NULL
};

static int cmd_help(int fd, int argc, char **argv)
{
	help_command_group(&duet_cmd_group, fd, argc, argv);
	return 0;
}

static const char * const cmd_version_usage[] = {
	"duet version",
	"Display duet-progs version",
	NULL
};

static int cmd_version(int fd, int argc, char **argv)
{
	printf("%s\n", DUET_BUILD_VERSION);
	return 0;
}

static int handle_options(int *argc, char ***argv)
{
	char **orig_argv = *argv;

	while (*argc > 0) {
		const char *arg = (*argv)[0];
		if (arg[0] != '-')
			break;

		if (!strcmp(arg, "--help")) {
			break;
		} else if (!strcmp(arg, "--version")) {
			break;
		} else {
			fprintf(stderr, "Unknown option: %s\n", arg);
			fprintf(stderr, "usage: %s\n",
				duet_cmd_group.usagestr[0]);
			exit(129);
		}

		(*argv)++;
		(*argc)--;
	}

	return (*argv) - orig_argv;
}

static const struct cmd_group duet_cmd_group = {
	duet_cmd_group_usage, duet_cmd_group_info, {
		{ "status", cmd_status, NULL, &status_cmd_group, 0 },
		{ "task", cmd_task, NULL, &task_cmd_group, 0 },
		{ "debug", cmd_debug, NULL, &debug_cmd_group, 0 },
		{ "help", cmd_help, cmd_help_usage, NULL, 0 },
		{ "version", cmd_version, cmd_version_usage, NULL, 0 },
		NULL_CMD_STRUCT
	},
};

int main(int argc, char **argv)
{
	int ret, fd;
	const struct cmd_struct *cmd;

	/* Open the duet device */
	fd = open_duet_dev();
	if (fd == -1) {
		fprintf(stderr, "Error: failed to open duet device\n");
		return -1;
	}

	/* Skip the duet name */
	argc--;
	argv++;

	handle_options(&argc, &argv);
	if (argc > 0) {
		if (!prefixcmp(argv[0], "--"))
			argv[0] += 2;
	} else {
		usage_command_group(&duet_cmd_group, 0, 0);
		exit(1);
	}

	cmd = parse_command_token(argv[0], &duet_cmd_group);
	handle_help_options_next_level(cmd, fd, argc, argv);

	fixup_argv0(argv, cmd->token);
	ret = cmd->fn(fd, argc, argv);

	close_duet_dev(fd);
	exit(ret);
}
