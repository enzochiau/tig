/* Copyright (c) 2006-2015 Jonas Fonseca <jonas.fonseca@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include "tig/argv.h"
#include "tig/refdb.h"
#include "tig/repo.h"
#include "tig/options.h"
#include "tig/display.h"
#include "tig/parse.h"
#include "tig/pager.h"
#include "tig/diff.h"
#include "tig/draw.h"

static bool
diff_open(struct view *view, enum open_flags flags)
{
	const char *diff_argv[] = {
		"git", "show", encoding_arg, "--pretty=fuller", "--root",
			"--patch-with-stat", use_mailmap_arg(),
			show_notes_arg(), diff_context_arg(), ignore_space_arg(),
			"%(diffargs)", "%(cmdlineargs)", "--no-color", "%(commit)",
			"--", "%(fileargs)", NULL
	};

	diff_save_line(view, view->private, flags);

	return begin_update(view, NULL, diff_argv, flags);
}

struct line *
diff_common_add_diff_stat(struct view *view, const char *text, size_t offset)
{
	const char *start = text + offset;
	const char *data = start + strspn(start, " ");
	size_t len = strlen(data);
	char *pipe = strchr(data, '|');

	/* Ensure that '|' is present and the file name part contains
	 * non-space characters. */
	if (!pipe || pipe == data || strcspn(data, " ") == 0)
		return NULL;

	/* Detect remaining part of a diff stat line:
	 *
	 *	added                    |   40 +++++++++++
	 *	remove                   |  124 --------------------------
	 *	updated                  |   14 +----
	 *	rename.from => rename.to |    0
	 *	.../truncated file name  |   11 ++---
	 *	binary add               |  Bin 0 -> 1234 bytes
	 *	binary update            |  Bin 1234 -> 2345 bytes
	 *	unmerged                 | Unmerged
	 */
	if ((data[len - 1] == '-' || data[len - 1] == '+') ||
	    strstr(pipe, " 0") ||
	    (strstr(pipe, "Bin") && strstr(pipe, "->")) ||
	    strstr(pipe, "Unmerged") ||
	    (data[len - 1] == '0' && (strstr(data, "=>") || !prefixcmp(data, "..."))))
		return add_line_text(view, text, LINE_DIFF_STAT);
	return NULL;
}

bool
diff_common_read(struct view *view, const char *data, struct diff_state *state)
{
	enum line_type type = get_line_type(data);

	if (!view->lines && type != LINE_COMMIT)
		state->reading_diff_stat = true;

	if (state->combined_diff && !state->after_diff && data[0] == ' ' && data[1] != ' ')
		state->reading_diff_stat = true;

	if (state->reading_diff_stat) {
		if (diff_common_add_diff_stat(view, data, 0))
			return true;
		state->reading_diff_stat = false;

	} else if (!strcmp(data, "---")) {
		state->reading_diff_stat = true;
	}

	if (!state->after_commit_title && !prefixcmp(data, "    ")) {
		struct line *line = add_line_text(view, data, LINE_DEFAULT);

		if (line)
			line->commit_title = 1;
		state->after_commit_title = true;
		return line != NULL;
	}

	if (type == LINE_DIFF_HEADER) {
		const int len = STRING_SIZE("diff --");

		state->after_diff = true;
		if (!strncmp(data + len, "combined ", strlen("combined ")) ||
		    !strncmp(data + len, "cc ", strlen("cc ")))
			state->combined_diff = true;

	} else if (type == LINE_PP_MERGE) {
		state->combined_diff = true;
	}

	/* ADD2 and DEL2 are only valid in combined diff hunks */
	if (!state->combined_diff && (type == LINE_DIFF_ADD2 || type == LINE_DIFF_DEL2))
		type = LINE_DEFAULT;

	return pager_common_read(view, data, type, NULL);
}

static bool
diff_find_stat_entry(struct view *view, struct line *line, enum line_type type)
{
	struct line *marker = find_next_line_by_type(view, line, type);

	return marker &&
		line == find_prev_line_by_type(view, marker, LINE_DIFF_HEADER);
}

static struct line *
diff_find_header_from_stat(struct view *view, struct line *line)
{
	if (line->type == LINE_DIFF_STAT) {
		int file_number = 0;

		while (view_has_line(view, line) && line->type == LINE_DIFF_STAT) {
			file_number++;
			line--;
		}

		for (line = view->line; view_has_line(view, line); line++) {
			line = find_next_line_by_type(view, line, LINE_DIFF_HEADER);
			if (!line)
				break;

			if (diff_find_stat_entry(view, line, LINE_DIFF_INDEX)
			    || diff_find_stat_entry(view, line, LINE_DIFF_SIMILARITY)) {
				if (file_number == 1) {
					break;
				}
				file_number--;
			}
		}

		return line;
	}

	return NULL;
}

enum request
diff_common_enter(struct view *view, enum request request, struct line *line)
{
	if (line->type == LINE_DIFF_STAT) {
		line = diff_find_header_from_stat(view, line);
		if (!line) {
			report("Failed to find file diff");
			return REQ_NONE;
		}

		select_view_line(view, line - view->line);
		report_clear();
		return REQ_NONE;

	} else {
		return pager_request(view, request, line);
	}
}

void
diff_save_line(struct view *view, struct diff_state *state, enum open_flags flags)
{
	if (flags & OPEN_RELOAD) {
		struct line *line = &view->line[view->pos.lineno];
		const char *file = view_has_line(view, line) ? diff_get_pathname(view, line) : NULL;

		if (file) {
			state->file = get_path(file);
			state->lineno = diff_get_lineno(view, line);
			state->pos = view->pos;
		}
	}
}

void
diff_restore_line(struct view *view, struct diff_state *state)
{
	struct line *line = &view->line[view->lines - 1];

	if (!state->file)
		return;

	while ((line = find_prev_line_by_type(view, line, LINE_DIFF_HEADER))) {
		const char *file = diff_get_pathname(view, line);

		if (file && !strcmp(file, state->file))
			break;
		line--;
	}

	state->file = NULL;

	if (!line)
		return;

	while ((line = find_next_line_by_type(view, line, LINE_DIFF_CHUNK))) {
		unsigned int lineno = diff_get_lineno(view, line);

		for (line++; view_has_line(view, line) && line->type != LINE_DIFF_CHUNK; line++) {
			if (lineno == state->lineno) {
				unsigned long lineno = line - view->line;
				unsigned long offset = lineno - (state->pos.lineno - state->pos.offset);

				goto_view_line(view, offset, lineno);
				redraw_view(view);
				return;
			}
			if (line->type != LINE_DIFF_DEL &&
			    line->type != LINE_DIFF_DEL2)
				lineno++;
		}
	}
}

static bool
diff_read_describe(struct view *view, struct buffer *buffer, struct diff_state *state)
{
	struct line *line = find_next_line_by_type(view, view->line, LINE_PP_REFS);

	if (line && buffer) {
		const char *ref = chomp_string(buffer->data);
		const char *sep = !strcmp("Refs: ", line->data) ? "" : ", ";

		if (*ref && !append_line_format(view, line, "%s%s", sep, ref))
			return false;
	}

	return true;
}

static bool
diff_read(struct view *view, struct buffer *buf)
{
	struct diff_state *state = view->private;

	if (state->adding_describe_ref)
		return diff_read_describe(view, buf, state);

	if (!buf) {
		/* Fall back to retry if no diff will be shown. */
		if (view->lines == 0 && opt_file_args) {
			int pos = argv_size(view->argv)
				- argv_size(opt_file_args) - 1;

			if (pos > 0 && !strcmp(view->argv[pos], "--")) {
				for (; view->argv[pos]; pos++) {
					free((void *) view->argv[pos]);
					view->argv[pos] = NULL;
				}

				if (view->pipe)
					io_done(view->pipe);
				if (view_exec(view, 0))
					return false;
			}
		}

		diff_restore_line(view, state);

		if (!state->adding_describe_ref && !ref_list_contains_tag(view->vid)) {
			const char *describe_argv[] = { "git", "describe", view->vid, NULL };

			if (!begin_update(view, NULL, describe_argv, OPEN_EXTRA)) {
				report("Failed to load describe data");
				return true;
			}

			state->adding_describe_ref = true;
			return false;
		}

		return true;
	}

	return diff_common_read(view, buf->data, state);
}

static bool
diff_blame_line(const char *ref, const char *file, unsigned long lineno,
		struct blame_header *header, struct blame_commit *commit)
{
	char author[SIZEOF_STR] = "";
	char line_arg[SIZEOF_STR];
	const char *blame_argv[] = {
		"git", "blame", encoding_arg, "-p", line_arg, ref, "--", file, NULL
	};
	struct io io;
	bool ok = false;
	struct buffer buf;

	if (!string_format(line_arg, "-L%ld,+1", lineno))
		return false;

	if (!io_run(&io, IO_RD, repo.cdup, NULL, blame_argv))
		return false;

	while (io_get(&io, &buf, '\n', true)) {
		if (header) {
			if (!parse_blame_header(header, buf.data, 9999999))
				break;
			header = NULL;

		} else if (parse_blame_info(commit, author, buf.data)) {
			ok = commit->filename != NULL;
			break;
		}
	}

	if (io_error(&io))
		ok = false;

	io_done(&io);
	return ok;
}

unsigned int
diff_get_lineno(struct view *view, struct line *line)
{
	const struct line *header, *chunk;
	unsigned int lineno;
	struct chunk_header chunk_header;

	/* Verify that we are after a diff header and one of its chunks */
	header = find_prev_line_by_type(view, line, LINE_DIFF_HEADER);
	chunk = find_prev_line_by_type(view, line, LINE_DIFF_CHUNK);
	if (!header || !chunk || chunk < header)
		return 0;

	/*
	 * In a chunk header, the number after the '+' sign is the number of its
	 * following line, in the new version of the file. We increment this
	 * number for each non-deletion line, until the given line position.
	 */
	if (!parse_chunk_header(&chunk_header, chunk->data))
		return 0;

	lineno = chunk_header.new.position;

	for (chunk++; chunk < line; chunk++)
		if (chunk->type != LINE_DIFF_DEL &&
		    chunk->type != LINE_DIFF_DEL2)
			lineno++;

	return lineno;
}

static enum request
diff_trace_origin(struct view *view, struct line *line)
{
	struct line *diff = find_prev_line_by_type(view, line, LINE_DIFF_HEADER);
	struct line *chunk = find_prev_line_by_type(view, line, LINE_DIFF_CHUNK);
	const char *chunk_data;
	int chunk_marker = line->type == LINE_DIFF_DEL ? '-' : '+';
	unsigned long lineno = 0;
	const char *file = NULL;
	char ref[SIZEOF_REF];
	struct blame_header header;
	struct blame_commit commit;

	if (!diff || !chunk || chunk == line) {
		report("The line to trace must be inside a diff chunk");
		return REQ_NONE;
	}

	for (; diff < line && !file; diff++) {
		const char *data = diff->data;

		if (!prefixcmp(data, "--- a/")) {
			file = data + STRING_SIZE("--- a/");
			break;
		}
	}

	if (diff == line || !file) {
		report("Failed to read the file name");
		return REQ_NONE;
	}

	chunk_data = chunk->data;

	if (!parse_chunk_lineno(&lineno, chunk_data, chunk_marker)) {
		report("Failed to read the line number");
		return REQ_NONE;
	}

	if (lineno == 0) {
		report("This is the origin of the line");
		return REQ_NONE;
	}

	for (chunk += 1; chunk < line; chunk++) {
		if (chunk->type == LINE_DIFF_ADD) {
			lineno += chunk_marker == '+';
		} else if (chunk->type == LINE_DIFF_DEL) {
			lineno += chunk_marker == '-';
		} else {
			lineno++;
		}
	}

	if (chunk_marker == '+')
		string_copy(ref, view->vid);
	else
		string_format(ref, "%s^", view->vid);

	if (string_rev_is_null(ref)) {
		string_ncopy(view->env->file, file, strlen(file));
		string_copy(view->env->ref, "");
		view->env->goto_lineno = lineno - 1;

	} else {
		if (!diff_blame_line(ref, file, lineno, &header, &commit)) {
			report("Failed to read blame data");
			return REQ_NONE;
		}

		string_ncopy(view->env->file, commit.filename, strlen(commit.filename));
		string_copy(view->env->ref, header.id);
		view->env->goto_lineno = header.orig_lineno - 1;
	}

	return REQ_VIEW_BLAME;
}

const char *
diff_get_pathname(struct view *view, struct line *line)
{
	const struct line *header;
	const char *dst = NULL;
	const char *prefixes[] = { " b/", "cc ", "combined " };
	int i;

	header = find_prev_line_by_type(view, line, LINE_DIFF_HEADER);
	if (!header)
		return NULL;

	for (i = 0; i < ARRAY_SIZE(prefixes) && !dst; i++)
		dst = strstr(header->data, prefixes[i]);

	return dst ? dst + strlen(prefixes[--i]) : NULL;
}

enum request
diff_common_edit(struct view *view, enum request request, struct line *line)
{
	const char *file;
	char path[SIZEOF_STR];
	unsigned int lineno;

	if (line->type == LINE_DIFF_STAT) {
		file = view->env->file;
		lineno = view->env->lineno;
	} else {
		file = diff_get_pathname(view, line);
		lineno = diff_get_lineno(view, line);
	}

	if (file && string_format(path, "%s%s", repo.cdup, file) && access(path, R_OK)) {
		report("Failed to open file: %s", file);
		return REQ_NONE;
	}

	open_editor(file, lineno);
	return REQ_NONE;
}

static enum request
diff_request(struct view *view, enum request request, struct line *line)
{
	switch (request) {
	case REQ_VIEW_BLAME:
		return diff_trace_origin(view, line);

	case REQ_EDIT:
		return diff_common_edit(view, request, line);

	case REQ_ENTER:
		return diff_common_enter(view, request, line);

	case REQ_REFRESH:
		if (string_rev_is_null(view->vid))
			refresh_view(view);
		else
			reload_view(view);
		return REQ_NONE;

	default:
		return pager_request(view, request, line);
	}
}

void
diff_common_select(struct view *view, struct line *line, const char *changes_msg)
{
	if (line->type == LINE_DIFF_STAT) {
		struct line *header = diff_find_header_from_stat(view, line);
		if (header) {
			const char *file = diff_get_pathname(view, header);

			if (file) {
				string_format(view->env->file, "%s", file);
				view->env->lineno = view->env->goto_lineno = 0;
				view->env->blob[0] = 0;
			}
		}

		string_format(view->ref, "Press '%s' to jump to file diff",
			      get_view_key(view, REQ_ENTER));
	} else {
		const char *file = diff_get_pathname(view, line);

		if (file) {
			if (changes_msg)
				string_format(view->ref, "%s to '%s'", changes_msg, file);
			string_format(view->env->file, "%s", file);
			view->env->lineno = view->env->goto_lineno = diff_get_lineno(view, line);
			view->env->blob[0] = 0;
		} else {
			string_ncopy(view->ref, view->ops->id, strlen(view->ops->id));
			pager_select(view, line);
		}
	}
}

static void
diff_select(struct view *view, struct line *line)
{
	diff_common_select(view, line, "Changes");
}

static struct view_ops diff_ops = {
	"line",
	argv_env.commit,
	VIEW_DIFF_LIKE | VIEW_ADD_DESCRIBE_REF | VIEW_ADD_PAGER_REFS | VIEW_FILE_FILTER | VIEW_REFRESH | VIEW_FLEX_WIDTH,
	sizeof(struct diff_state),
	diff_open,
	diff_read,
	view_column_draw,
	diff_request,
	view_column_grep,
	diff_select,
	NULL,
	view_column_bit(LINE_NUMBER) | view_column_bit(TEXT),
	pager_get_column_data,
};

DEFINE_VIEW(diff);

/* vim: set ts=8 sw=8 noexpandtab: */
