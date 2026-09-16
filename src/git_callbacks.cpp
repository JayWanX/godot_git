#include <iostream>
#include <cstring>

#include "git_callbacks.h"
#include "git.h"

#include "core/variant/variant_utility.h"

extern "C" int progress_cb(const char *str, int len, void *data) {
	(void)data;

	print_line("remote: ", String::utf8(str, len).strip_edges());

	return 0;
}

extern "C" int update_cb(const char *refname, const git_oid *a, const git_oid *b, void *data) {
	constexpr int short_commit_length = 8;
	char a_str[short_commit_length + 1];
	char b_str[short_commit_length + 1];
	(void)data;

	git_oid_tostr(b_str, short_commit_length, b);
	if (git_oid_is_zero(a)) {
		print_line("* [new] ", String::utf8(b_str), " ", String::utf8(refname));
	} else {
		git_oid_tostr(a_str, short_commit_length, a);
		print_line("[updated] ", String::utf8(a_str), "...", String::utf8(b_str), " ", String::utf8(refname));
	}

	return 0;
}

extern "C" int transfer_progress_cb(const git_indexer_progress *stats, void *payload) {
	(void)payload;

	if (stats->received_objects == stats->total_objects) {
		print_line("Resolving deltas ", uint32_t(stats->indexed_deltas), "/", uint32_t(stats->total_deltas));
	} else if (stats->total_objects > 0) {
		print_line(
				"Received ", uint32_t(stats->received_objects), "/", uint32_t(stats->total_objects),
				" objects (", uint32_t(stats->indexed_objects), ") in ", uint32_t(stats->received_bytes), " bytes");
	}
	return 0;
}

extern "C" int fetchhead_foreach_cb(const char *ref_name, const char *remote_url, const git_oid *oid, unsigned int is_merge, void *payload) {
	if (is_merge) {
		git_oid_cpy((git_oid *)payload, oid);
	}
	return 0;
}

extern "C" int push_transfer_progress_cb(unsigned int current, unsigned int total, size_t bytes, void *payload) {
	int64_t progress = 100;

	if (total != 0) {
		progress = (current * 100) / total;
	}

	print_line("Writing Objects: ", uint32_t(progress), "% (", uint32_t(current), "/", uint32_t(total), ", ", uint32_t(bytes), " bytes done.)");
	return 0;
}

extern "C" int push_update_reference_cb(const char *refname, const char *status, void *data) {
	if (status != NULL) {
		String status_str = String::utf8(status);
		print_line("[rejected] ", String::utf8(refname), " ", status_str);
	} else {
		print_line("[updated] ", String::utf8(refname));
	}
	return 0;
}

extern "C" int credentials_cb(git_cred **out, const char *url, const char *username_from_url, unsigned int allowed_types, void *payload) {
	Credentials *creds = (Credentials *)payload;

	String proper_username = username_from_url ? username_from_url : creds->username;

	if (!creds->ssh_public_key_path.is_empty()) {
		if (allowed_types & GIT_CREDENTIAL_SSH_KEY) {
			return git_credential_ssh_key_new(out,
					CString(proper_username).data,
					CString(creds->ssh_public_key_path).data,
					CString(creds->ssh_private_key_path).data,
					CString(creds->ssh_passphrase).data);
		}
	}

	if (allowed_types & GIT_CREDENTIAL_USERPASS_PLAINTEXT) {
		return git_cred_userpass_plaintext_new(out, CString(proper_username).data, CString(creds->password).data);
	}

	if (allowed_types & GIT_CREDENTIAL_USERNAME) {
		return git_credential_username_new(out, CString(proper_username).data);
	}

	return GIT_EUSER;
}

extern "C" int diff_hunk_cb(const git_diff_delta *delta, const git_diff_hunk *range, void *payload) {
	DiffHelper *diff_helper = (DiffHelper *)payload;

	Dictionary hunk = diff_helper->git->create_diff_hunk(range->old_start, range->new_start, range->old_lines, range->new_lines);
	diff_helper->diff_hunks->push_back(hunk);

	return 1;
}
