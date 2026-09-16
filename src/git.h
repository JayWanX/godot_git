#pragma once

#include <unordered_map>

#include "git_callbacks.h"
#include "git_wrappers.h"

#include "editor/version_control/editor_vcs_interface.h"
#include "git2.h"

#include <vector>

struct Credentials {
	String username;
	String password;
	String ssh_public_key_path;
	String ssh_private_key_path;
	String ssh_passphrase;
};

class Git : public EditorVCSInterface {
	GDCLASS(Git, EditorVCSInterface);

protected:
	static void _bind_methods();

public:
	Credentials creds;
	bool has_merge = false;
	git_repository_ptr repo;
	git_oid pull_merge_oid = {};
	String repo_project_path;
	std::unordered_map<git_status_t, ChangeType> map_changes;

	Git();

	// Endpoints（由桥接 GDScript 转发，见 git.cpp 的 GIT_BRIDGE_SCRIPT）
	bool _initialize(const String &project_path);
	void _set_credentials(const String &username, const String &password, const String &ssh_public_key_path, const String &ssh_private_key_path, const String &ssh_passphrase);
	TypedArray<Dictionary> _get_modified_files_data();
	void _stage_file(const String &file_path);
	void _unstage_file(const String &file_path);
	void _discard_file(const String &file_path);
	void _commit(const String &msg, bool amend);
	bool _allow_amends();
	TypedArray<Dictionary> _get_diff(const String &identifier, int32_t area);
	bool _shut_down();
	String _get_vcs_name();
	TypedArray<Dictionary> _get_previous_commits(int32_t max_commits);
	TypedArray<String> _get_branch_list();
	TypedArray<String> _get_remotes();
	void _create_branch(const String &branch_name);
	void _remove_branch(const String &branch_name);
	void _create_remote(const String &remote_name, const String &remote_url);
	void _remove_remote(const String &remote_name);
	String _get_current_branch_name();
	bool _checkout_branch(const String &branch_name);
	void _pull(const String &remote);
	void _push(const String &remote, bool force);
	void _fetch(const String &remote);
	TypedArray<Dictionary> _get_line_diff(const String &file_path, const String &text);

	// Helpers
	TypedArray<Dictionary> _parse_diff(git_diff *p_diff);
	bool check_errors(int error, String function, String file, int line, String message, const std::vector<git_error_code> &ignores = {});
	void create_gitignore_and_gitattributes();
	bool create_initial_commit();

private:
	void _attach_bridge_script();
};
