#include "git.h"

#include <chrono>
#include <cstring>

#include <git2/tree.h>
#include "core/object/class_db.h"
#include "core/object/callable_mp.h"
#include "core/io/file_access.h"
#include "core/io/dir_access.h"
#include "core/os/os.h"
#include "editor/file_system/editor_file_system.h"
#include "core/config/project_settings.h"
#include "core/string/print_string.h"
#include "core/string/ustring.h"
#include "core/variant/variant.h"

// MODULE_GDSCRIPT_ENABLED 不在任何全局头文件链里，必须显式 include 这个
// 构建期生成的头（与引擎 editor_node.cpp 等文件的做法一致），
// 下面的 #ifdef 才能正确看到 gdscript 模块的启用状态。
#include "modules/modules_enabled.gen.h"

#ifdef MODULE_GDSCRIPT_ENABLED
#include "modules/gdscript/gdscript.h"
#endif

#define GIT2_CALL(error, msg)                                         \
	if (check_errors(error, __FUNCTION__, __FILE__, __LINE__, msg)) { \
		return;                                                       \
	}

#define GIT2_CALL_R(error, msg, return_value)                         \
	if (check_errors(error, __FUNCTION__, __FILE__, __LINE__, msg)) { \
		return return_value;                                          \
	}

#define GIT2_CALL_IGNORE(error, msg, ignores)                                  \
	if (check_errors(error, __FUNCTION__, __FILE__, __LINE__, msg, ignores)) { \
		return;                                                                \
	}

#define GIT2_CALL_R_IGNORE(error, msg, return_value, ignores)                  \
	if (check_errors(error, __FUNCTION__, __FILE__, __LINE__, msg, ignores)) { \
		return return_value;                                                   \
	}

#define COMMA ,

// ------------------------------------------------------------------
// 桥接脚本：EditorVCSInterface 的 25 个回调是 GDVIRTUAL（只认脚本与
// GDExtension 覆写），引擎内部 C++ 无法 override。这里在对象构造时挂一个
// 内存中的 GDScript，把每个回调转发回本类的 C++ 实现，从而在完全不修改
// 引擎源码的前提下以原生模块提供 VCS 功能。
//
// 设计约束（两条都是实测踩过的坑）：
// 1. 必须 extends EditorVCSInterface：GDScript 解析原生类只认
//    GDScriptLanguage::init() 建立的 ClassDB 快照表（gdscript.cpp:2159，
//    gdscript_compiler.cpp:122），模块在编辑器级注册的类不在可靠解析
//    路径上，而基类 EditorVCSInterface 一定在表里。
// 2. 转发一律用动态 call("git_xxx", ...)：静态写 git_xxx(...) 会在编译期
//    被解析到 extends 声明的基类上（报 "Function ... not found in base
//    self"）；动态 call 运行时走 Object::callp——脚本实例查不到该方法时
//    回落到 ClassDB::get_method（object.cpp:820），正好命中 Git 上
//    bind_method 绑定的 C++ 实现。
// 3. 必须 @tool：编辑器启动时会把 ScriptServer::set_scripting_enabled(false)
//    （editor_node.cpp:8388），非 tool 脚本的 can_instantiate() 随之为 false，
//    set_script 会静默创建占位实例——has_method 照样报 true 但调用永远失败，
//    GDVIRTUAL 派发即报 "must be overridden"。@tool 让脚本在编辑器内可实例化。
static const char *GIT_BRIDGE_SCRIPT = R"BRIDGE(
@tool
extends EditorVCSInterface

func _initialize(project_path: String) -> bool:
	return call("git_initialize", project_path)

func _set_credentials(username: String, password: String, ssh_public_key_path: String, ssh_private_key_path: String, ssh_passphrase: String) -> void:
	call("git_set_credentials", username, password, ssh_public_key_path, ssh_private_key_path, ssh_passphrase)

func _get_modified_files_data() -> Array:
	return call("git_get_modified_files_data")

func _stage_file(file_path: String) -> void:
	call("git_stage_file", file_path)

func _unstage_file(file_path: String) -> void:
	call("git_unstage_file", file_path)

func _discard_file(file_path: String) -> void:
	call("git_discard_file", file_path)

func _commit(msg: String, amend: bool) -> void:
	call("git_commit", msg, amend)

func _allow_amends() -> bool:
	return call("git_allow_amends")

func _get_diff(identifier: String, area: int) -> Array:
	return call("git_get_diff", identifier, area)

func _shut_down() -> bool:
	return call("git_shut_down")

func _get_vcs_name() -> String:
	return call("git_get_vcs_name")

func _get_previous_commits(max_commits: int) -> Array:
	return call("git_get_previous_commits", max_commits)

func _get_branch_list() -> Array:
	return call("git_get_branch_list")

func _get_remotes() -> Array:
	return call("git_get_remotes")

func _create_branch(branch_name: String) -> void:
	call("git_create_branch", branch_name)

func _remove_branch(branch_name: String) -> void:
	call("git_remove_branch", branch_name)

func _create_remote(remote_name: String, remote_url: String) -> void:
	call("git_create_remote", remote_name, remote_url)

func _remove_remote(remote_name: String) -> void:
	call("git_remove_remote", remote_name)

func _get_current_branch_name() -> String:
	return call("git_get_current_branch_name")

func _checkout_branch(branch_name: String) -> bool:
	return call("git_checkout_branch", branch_name)

func _pull(remote: String) -> void:
	call("git_pull", remote)

func _push(remote: String, force: bool) -> void:
	call("git_push", remote, force)

func _fetch(remote: String) -> void:
	call("git_fetch", remote)

func _get_line_diff(file_path: String, text: String) -> Array:
	return call("git_get_line_diff", file_path, text)
)BRIDGE";

void Git::_attach_bridge_script() {
#ifdef MODULE_GDSCRIPT_ENABLED
	// 每个实例持有自己的桥脚本（不用 static：引擎退出时静态 Resource 的析构
	// 会晚于 GDScriptLanguage::finish，存在崩溃风险；随对象释放则生命周期干净）。
	Ref<GDScript> bridge;
	bridge.instantiate();
	bridge->set_source_code(String::utf8(GIT_BRIDGE_SCRIPT));
	Error err = bridge->reload();
	if (err != OK) {
		ERR_PRINT(vformat("Git: bridge script failed to compile (error %d), VCS integration will not work.", (int)err));
	}
	set_script(bridge);
#else
	ERR_PRINT_ONCE("Git: GDScript module is required for the VCS bridge.");
#endif
}

void Git::_bind_methods() {
	// 暴露给桥接 GDScript 的 C++ 实现（见 GIT_BRIDGE_SCRIPT）
	ClassDB::bind_method(D_METHOD("git_initialize", "project_path"), &Git::_initialize);
	ClassDB::bind_method(D_METHOD("git_set_credentials", "username", "password", "ssh_public_key_path", "ssh_private_key_path", "ssh_passphrase"), &Git::_set_credentials);
	ClassDB::bind_method(D_METHOD("git_get_modified_files_data"), &Git::_get_modified_files_data);
	ClassDB::bind_method(D_METHOD("git_stage_file", "file_path"), &Git::_stage_file);
	ClassDB::bind_method(D_METHOD("git_unstage_file", "file_path"), &Git::_unstage_file);
	ClassDB::bind_method(D_METHOD("git_discard_file", "file_path"), &Git::_discard_file);
	ClassDB::bind_method(D_METHOD("git_commit", "msg", "amend"), &Git::_commit);
	ClassDB::bind_method(D_METHOD("git_allow_amends"), &Git::_allow_amends);
	ClassDB::bind_method(D_METHOD("git_get_diff", "identifier", "area"), &Git::_get_diff);
	ClassDB::bind_method(D_METHOD("git_shut_down"), &Git::_shut_down);
	ClassDB::bind_method(D_METHOD("git_get_vcs_name"), &Git::_get_vcs_name);
	ClassDB::bind_method(D_METHOD("git_get_previous_commits", "max_commits"), &Git::_get_previous_commits);
	ClassDB::bind_method(D_METHOD("git_get_branch_list"), &Git::_get_branch_list);
	ClassDB::bind_method(D_METHOD("git_get_remotes"), &Git::_get_remotes);
	ClassDB::bind_method(D_METHOD("git_create_branch", "branch_name"), &Git::_create_branch);
	ClassDB::bind_method(D_METHOD("git_remove_branch", "branch_name"), &Git::_remove_branch);
	ClassDB::bind_method(D_METHOD("git_create_remote", "remote_name", "remote_url"), &Git::_create_remote);
	ClassDB::bind_method(D_METHOD("git_remove_remote", "remote_name"), &Git::_remove_remote);
	ClassDB::bind_method(D_METHOD("git_get_current_branch_name"), &Git::_get_current_branch_name);
	ClassDB::bind_method(D_METHOD("git_checkout_branch", "branch_name"), &Git::_checkout_branch);
	ClassDB::bind_method(D_METHOD("git_pull", "remote"), &Git::_pull);
	ClassDB::bind_method(D_METHOD("git_push", "remote", "force"), &Git::_push);
	ClassDB::bind_method(D_METHOD("git_fetch", "remote"), &Git::_fetch);
	ClassDB::bind_method(D_METHOD("git_get_line_diff", "file_path", "text"), &Git::_get_line_diff);
	// Doesn't seem to require binding functions for now
}

Git::Git() {
	map_changes[GIT_STATUS_WT_NEW] = CHANGE_TYPE_NEW;
	map_changes[GIT_STATUS_INDEX_NEW] = CHANGE_TYPE_NEW;
	map_changes[GIT_STATUS_WT_MODIFIED] = CHANGE_TYPE_MODIFIED;
	map_changes[GIT_STATUS_INDEX_MODIFIED] = CHANGE_TYPE_MODIFIED;
	map_changes[GIT_STATUS_WT_RENAMED] = CHANGE_TYPE_RENAMED;
	map_changes[GIT_STATUS_INDEX_RENAMED] = CHANGE_TYPE_RENAMED;
	map_changes[GIT_STATUS_WT_DELETED] = CHANGE_TYPE_DELETED;
	map_changes[GIT_STATUS_INDEX_DELETED] = CHANGE_TYPE_DELETED;
	map_changes[GIT_STATUS_WT_TYPECHANGE] = CHANGE_TYPE_TYPECHANGE;
	map_changes[GIT_STATUS_INDEX_TYPECHANGE] = CHANGE_TYPE_TYPECHANGE;
	map_changes[GIT_STATUS_CONFLICTED] = CHANGE_TYPE_UNMERGED;

	_attach_bridge_script();
}

bool Git::check_errors(int error, String function, String file, int line, String message, const std::vector<git_error_code> &ignores) {
	const git_error *lg2err;

	if (error == 0) {
		return false;
	}

	for (auto &ig : ignores) {
		if (error == ig) {
			return false;
		}
	}

	message = message + ".";
	if ((lg2err = git_error_last()) != nullptr && lg2err->message != nullptr) {
		message = message + " Error " + String::num_int64(error) + ": ";
		message = message + String::utf8(lg2err->message);
	}

	ERR_PRINT(vformat("Git: %s in %s:%s#L%d", message, file, function, line));
	return true;
}

void Git::_set_credentials(const String &username, const String &password, const String &ssh_public_key_path, const String &ssh_private_key_path, const String &ssh_passphrase) {
	creds.username = username;
	creds.password = password;
	creds.ssh_public_key_path = ssh_public_key_path;
	creds.ssh_private_key_path = ssh_private_key_path;
	creds.ssh_passphrase = ssh_passphrase;
}

void Git::_discard_file(const String &file_path) {
	_wait_no_job(); // 网络任务会写索引/工作区，与写操作互斥
	git_checkout_options opts = GIT_CHECKOUT_OPTIONS_INIT;
	CString c_path(file_path);
	char *paths[] = { c_path.data };
	opts.paths = { paths, 1 };
	opts.checkout_strategy = GIT_CHECKOUT_FORCE;

	GIT2_CALL(git_checkout_index(repo.get(), nullptr, &opts), "Could not checkout index");
	_sync_refresh_status(); // 变更落盘后立即刷新快照，面板下一次取数即为新状态
}

void Git::_commit(const String &msg, bool amend) {
	_wait_no_job(); // 网络任务会写索引/工作区，与写操作互斥

	// pull 已移到后台线程，合并状态跨线程传递：持锁取本地副本。
	bool local_has_merge = false;
	git_oid local_merge_oid = {};
	{
		std::unique_lock<std::mutex> lock(bg_mutex);
		local_has_merge = has_merge.load();
		local_merge_oid = pull_merge_oid;
	}

	git_index_ptr repo_index;
	GIT2_CALL(git_repository_index(Capture(repo_index), repo.get()), "Could not get repository index");

	git_oid tree_id;
	GIT2_CALL(git_index_write_tree(&tree_id, repo_index.get()), "Could not write index to tree");
	GIT2_CALL(git_index_write(repo_index.get()), "Could not write index to disk");

	git_tree_ptr tree;
	GIT2_CALL(git_tree_lookup(Capture(tree), repo.get(), &tree_id), "Could not lookup tree from ID");

	git_signature_ptr default_sign;
	GIT2_CALL(git_signature_default(Capture(default_sign), repo.get()), "Could not get default signature");

	git_oid parent_commit_id = {};
	GIT2_CALL_IGNORE(git_reference_name_to_id(&parent_commit_id, repo.get(), "HEAD"), "Could not get repository HEAD", { GIT_ENOTFOUND });

	git_commit_ptr parent_commit;
	if (!git_oid_is_zero(&parent_commit_id)) {
		GIT2_CALL(git_commit_lookup(Capture(parent_commit), repo.get(), &parent_commit_id), "Could not lookup parent commit data");
	}

	git_oid new_commit_id;
	if (!local_has_merge) {
		if (amend && parent_commit) {
			// Godot 4.7 起 _commit 带 amend 参数，修正最近一次提交。
			GIT2_CALL(
					git_commit_amend(
							&new_commit_id,
							parent_commit.get(),
							"HEAD",
							default_sign.get(),
							default_sign.get(),
							"UTF-8",
							CString(msg).data,
							tree.get()),
					"Could not amend commit");
		} else {
			GIT2_CALL(
					git_commit_create_v(
							&new_commit_id,
							repo.get(),
							"HEAD",
							default_sign.get(),
							default_sign.get(),
							"UTF-8",
							CString(msg).data,
							tree.get(),
							parent_commit.get() ? 1 : 0,
							parent_commit.get()),
					"Could not create commit");
		}
	} else {
		git_commit_ptr fetchhead_commit;
		GIT2_CALL(git_commit_lookup(Capture(fetchhead_commit), repo.get(), &local_merge_oid), "Could not lookup commit pointed to by HEAD");

		GIT2_CALL(
				git_commit_create_v(
						&new_commit_id,
						repo.get(),
						"HEAD",
						default_sign.get(),
						default_sign.get(),
						"UTF-8",
						CString(msg).data,
						tree.get(),
						2,
						parent_commit.get(),
						fetchhead_commit.get()),
				"Could not create merge commit");
		has_merge.store(false);
		GIT2_CALL(git_repository_state_cleanup(repo.get()), "Could not clean repository state");
	}

	_sync_refresh_status(); // 变更落盘后立即刷新快照，面板下一次取数即为新状态
}

bool Git::_allow_amends() {
	return true;
}

void Git::_stage_file(const String &file_path) {
	_wait_no_job(); // 网络任务会写索引/工作区，与写操作互斥
	CString c_path(file_path);
	char *paths[] = { c_path.data };
	git_strarray array = { paths, 1 };

	git_index_ptr index;
	GIT2_CALL(git_repository_index(Capture(index), repo.get()), "Could not get repository index");
	GIT2_CALL(git_index_add_all(index.get(), &array, GIT_INDEX_ADD_DEFAULT | GIT_INDEX_ADD_DISABLE_PATHSPEC_MATCH, nullptr, nullptr), "Could not add " + file_path + " to index");
	GIT2_CALL(git_index_write(index.get()), "Could not write changes to disk");
	_sync_refresh_status(); // 变更落盘后立即刷新快照，面板下一次取数即为新状态
}

void Git::_unstage_file(const String &file_path) {
	_wait_no_job(); // 网络任务会写索引/工作区，与写操作互斥
	CString c_path(file_path);
	char *paths[] = { c_path.data };
	git_strarray array = { paths, 1 };

	git_reference_ptr head;
	GIT2_CALL_IGNORE(git_repository_head(Capture(head), repo.get()), "Could not find repository HEAD", { GIT_ENOTFOUND COMMA GIT_EUNBORNBRANCH });

	if (head) {
		git_object_ptr head_commit;
		GIT2_CALL(git_reference_peel(Capture(head_commit), head.get(), GIT_OBJ_COMMIT), "Could not peel HEAD reference");
		GIT2_CALL(git_reset_default(repo.get(), head_commit.get(), &array), "Could not reset " + file_path + " to state at HEAD");
	} else {
		// If there is no HEAD commit, we should just remove the file from the index.

		CString c_path(file_path);

		git_index_ptr index;
		GIT2_CALL(git_repository_index(Capture(index), repo.get()), "Could not get repository index");
		GIT2_CALL(git_index_remove_bypath(index.get(), c_path.data), "Could not add " + file_path + " to index");
		GIT2_CALL(git_index_write(index.get()), "Could not write changes to disk");
	}

	_sync_refresh_status(); // 变更落盘后立即刷新快照，面板下一次取数即为新状态
}

void Git::create_gitignore_and_gitattributes() {
	if (!FileAccess::exists(repo_project_path + "/.gitignore")) {
		Ref<FileAccess> file = FileAccess::open(repo_project_path + "/.gitignore", FileAccess::ModeFlags::WRITE);
		ERR_FAIL_COND(file.is_null());
		file->store_string(
				"# Godot 4+ specific ignores\n"
				".godot/\n");
	}

	if (!FileAccess::exists(repo_project_path + "/.gitattributes")) {
		Ref<FileAccess> file = FileAccess::open(repo_project_path + "/.gitattributes", FileAccess::ModeFlags::WRITE);
		ERR_FAIL_COND(file.is_null());
		file->store_string(
				"# Set the default behavior, in case people don't have core.autocrlf set.\n"
				"* text=auto\n\n"

				"# Explicitly declare text files you want to always be normalized and converted\n"
				"# to native line endings on checkout.\n"
				"*.cpp text\n"
				"*.c text\n"
				"*.h text\n"
				"*.gd text\n"
				"*.cs text\n\n"

				"# Declare files that will always have CRLF line endings on checkout.\n"
				"*.sln text eol=crlf\n\n"

				"# Denote all files that are truly binary and should not be modified.\n"
				"*.png binary\n"
				"*.jpg binary\n");
	}
}

TypedArray<Dictionary> Git::_get_modified_files_data() {
	// 直接返回后台线程维护的快照，主线程零耗时（status 全量扫描是原本最频繁
	// 的阻塞源）。快照的新鲜度由三处保证：_initialize 时同步首扫、后台线程
	// 周期刷新（外部变化，如命令行里的 git 操作）、写操作后 _sync_refresh_status。
	std::vector<StatusEntry> local;
	{
		std::unique_lock<std::mutex> lock(bg_mutex);
		local = status_snapshot;
	}
	// 持锁窗口内不做任何打印/重活（print_line 会拿引擎全局锁，可能与后台
	// 线程形成锁序交叉），拷贝完立即解锁，转换在锁外进行。
	TypedArray<Dictionary> result = _status_to_array(local);
	return result;
}

void Git::_scan_status_into(git_repository *p_repo, std::vector<StatusEntry> &r_out) {
	// 全量扫描工作区 + 索引状态，输出纯数据条目（不构造 Dictionary，
	// 因此可在后台线程安全调用）。失败时输出空列表并打印错误。
	r_out.clear();

	git_status_options opts = GIT_STATUS_OPTIONS_INIT;
	opts.show = GIT_STATUS_SHOW_INDEX_AND_WORKDIR;
	opts.flags = GIT_STATUS_OPT_EXCLUDE_SUBMODULES;
	opts.flags |= GIT_STATUS_OPT_INCLUDE_UNTRACKED | GIT_STATUS_OPT_RENAMES_HEAD_TO_INDEX | GIT_STATUS_OPT_SORT_CASE_SENSITIVELY | GIT_STATUS_OPT_RECURSE_UNTRACKED_DIRS;

	git_status_list_ptr statuses;
	int err = git_status_list_new(Capture(statuses), p_repo, &opts);
	if (err != 0) {
		check_errors(err, __FUNCTION__, __FILE__, __LINE__, "Could not get status information from repository");
		return;
	}

	size_t count = git_status_list_entrycount(statuses.get());
	for (size_t i = 0; i < count; ++i) {
		const git_status_entry *entry = git_status_byindex(statuses.get(), i);
		String path;
		if (entry->index_to_workdir) {
			path = String::utf8(entry->index_to_workdir->new_file.path);
		} else {
			path = String::utf8(entry->head_to_index->new_file.path);
		}

		// 跳过引擎 Windows 原子保存的瞬态临时文件（file_access_windows.cpp:204，
		// 命名规则「原文件名 + 数字 + .tmp」）。这类文件只存在几毫秒且被编辑器
		// 独占打开——读它做 diff 会报共享冲突，列进面板也只是闪现的伪变更。
		{
			String base = path.get_basename();
			if (path.get_extension() == "tmp" && !base.is_empty() && base[base.length() - 1] >= '0' && base[base.length() - 1] <= '9') {
				continue;
			}
		}

		const static int git_status_wt = GIT_STATUS_WT_NEW | GIT_STATUS_WT_MODIFIED | GIT_STATUS_WT_DELETED | GIT_STATUS_WT_TYPECHANGE | GIT_STATUS_WT_RENAMED | GIT_STATUS_CONFLICTED;
		const static int git_status_index = GIT_STATUS_INDEX_NEW | GIT_STATUS_INDEX_MODIFIED | GIT_STATUS_INDEX_DELETED | GIT_STATUS_INDEX_RENAMED | GIT_STATUS_INDEX_TYPECHANGE;

		if (entry->status & git_status_wt) {
			StatusEntry e;
			e.path = path;
			e.change_type = map_changes[git_status_t(entry->status & git_status_wt)];
			e.tree_area = TREE_AREA_UNSTAGED;
			r_out.push_back(e);
		}

		if (entry->status & git_status_index) {
			if (entry->status & GIT_STATUS_INDEX_RENAMED) {
				String old_path = String::utf8(entry->head_to_index->old_file.path);
				StatusEntry deleted;
				deleted.path = old_path;
				deleted.change_type = map_changes.at(GIT_STATUS_INDEX_DELETED);
				deleted.tree_area = TREE_AREA_STAGED;
				r_out.push_back(deleted);

				StatusEntry added;
				added.path = path;
				added.change_type = map_changes.at(GIT_STATUS_INDEX_NEW);
				added.tree_area = TREE_AREA_STAGED;
				r_out.push_back(added);
			} else {
				StatusEntry e;
				e.path = path;
				e.change_type = map_changes.at(git_status_t(entry->status & git_status_index));
				e.tree_area = TREE_AREA_STAGED;
				r_out.push_back(e);
			}
		}
	}
}

TypedArray<Dictionary> Git::_status_to_array(const std::vector<StatusEntry> &p_entries) {
	TypedArray<Dictionary> stats_files;
	for (const StatusEntry &e : p_entries) {
		stats_files.push_back(create_status_file(e.path, (ChangeType)e.change_type, (TreeArea)e.tree_area));
	}
	return stats_files;
}

void Git::_sync_refresh_status() {
	// 主线程写操作（stage/commit/checkout 等）落盘后调用：立即重扫一次快照，
	// 保证面板下一次取数即为新状态。工作区 diff 缓存一并失效，由后台线程重建；
	// 提交 diff（SHA 键，内容对当前分支无依赖）保留，避免整批重算。
	if (!repo) {
		return;
	}
	std::vector<StatusEntry> fresh;
	_scan_status_into(repo.get(), fresh);
	std::unique_lock<std::mutex> lock(bg_mutex);
	status_snapshot = std::move(fresh);
	Vector<String> to_erase;
	for (const KeyValue<String, TypedArray<Dictionary>> &kv : diff_cache) {
		if (kv.key.rfind("#") != 40) { // 非 "<40位SHA>#0" 形态即工作区条目
			to_erase.push_back(kv.key);
		}
	}
	for (const String &k : to_erase) {
		diff_cache.erase(k);
	}
	write_generation++; // 作废扫描中的后台结果，防止旧快照覆盖新状态
}

TypedArray<String> Git::_get_branch_list() {
	git_branch_iterator_ptr it;
	GIT2_CALL_R(git_branch_iterator_new(Capture(it), repo.get(), GIT_BRANCH_LOCAL), "Could not create branch iterator", TypedArray<Dictionary>());

	TypedArray<String> branch_names;

	git_reference_ptr ref;
	git_branch_t type;
	while (git_branch_next(Capture(ref), &type, it.get()) != GIT_ITEROVER) {
		const char *name = nullptr;

		GIT2_CALL_R(git_branch_name(&name, ref.get()), "Could not get branch name", TypedArray<String>());

		if (git_branch_is_head(ref.get())) {
			// Always send the current branch as the first branch in list
			branch_names.push_front(String::utf8(name));
		} else {
			branch_names.push_back(String::utf8(name));
		}
	}

	return branch_names;
}

void Git::_create_branch(const String &branch_name) {
	_wait_no_job();
	git_oid head_commit_id;
	GIT2_CALL(git_reference_name_to_id(&head_commit_id, repo.get(), "HEAD"), "Could not get HEAD commit ID");

	git_commit_ptr head_commit;
	GIT2_CALL(git_commit_lookup(Capture(head_commit), repo.get(), &head_commit_id), "Could not lookup HEAD commit");

	git_reference_ptr branch_ref;
	GIT2_CALL(git_branch_create(Capture(branch_ref), repo.get(), CString(branch_name).data, head_commit.get(), 0), "Could not create branch from HEAD");
}

void Git::_create_remote(const String &remote_name, const String &remote_url) {
	git_remote_ptr remote;
	GIT2_CALL(git_remote_create(Capture(remote), repo.get(), CString(remote_name).data, CString(remote_url).data), "Could not create remote");
}

void Git::_remove_branch(const String &branch_name) {
	_wait_no_job();
	git_reference_ptr branch;
	GIT2_CALL(git_branch_lookup(Capture(branch), repo.get(), CString(branch_name).data, GIT_BRANCH_LOCAL), "Could not find branch " + branch_name);
	GIT2_CALL(git_branch_delete(branch.get()), "Could not delete branch reference of " + branch_name);
}

void Git::_remove_remote(const String &remote_name) {
	_wait_no_job();
	GIT2_CALL(git_remote_delete(repo.get(), CString(remote_name).data), "Could not delete remote " + remote_name);
}

TypedArray<Dictionary> Git::_get_line_diff(const String &file_path, const String &text) {
	git_diff_options opts = GIT_DIFF_OPTIONS_INIT;

	opts.context_lines = 0;
	opts.flags = GIT_DIFF_DISABLE_PATHSPEC_MATCH | GIT_DIFF_INCLUDE_UNTRACKED;

	git_index_ptr index;
	GIT2_CALL_R(git_repository_index(Capture(index), repo.get()), "Failed to get repository index", TypedArray<Dictionary>());
	GIT2_CALL_R(git_index_read(index.get(), 0), "Failed to read index", TypedArray<Dictionary>());

	const git_index_entry *entry = git_index_get_bypath(index.get(), CString(file_path).data, GIT_INDEX_STAGE_NORMAL);
	if (!entry) {
		return TypedArray<Dictionary>();
	}

	git_reference_ptr head;
	GIT2_CALL_R(git_repository_head(Capture(head), repo.get()), "Failed to load repository head", TypedArray<Dictionary>());

	git_blob_ptr blob;
	GIT2_CALL_R(git_blob_lookup(Capture(blob), repo.get(), &entry->id), "Failed to load head blob", TypedArray<Dictionary>());

	TypedArray<Dictionary> diff_contents;

	DiffHelper diff_helper = { &diff_contents, this };
	GIT2_CALL_R(git_diff_blob_to_buffer(blob.get(), nullptr, CString(text).data, text.length(), nullptr, &opts, nullptr, nullptr, diff_hunk_cb, nullptr, &diff_helper), "Failed to make diff", TypedArray<Dictionary>());

	return diff_contents;
}

String Git::_get_current_branch_name() {
	return _current_branch_name_with(repo.get());
}

String Git::_current_branch_name_with(git_repository *p_repo) {
	git_reference_ptr head;
	GIT2_CALL_R_IGNORE(git_repository_head(Capture(head), p_repo), "Could not find repository HEAD", "", { GIT_ENOTFOUND COMMA GIT_EUNBORNBRANCH });

	if (!head) {
		// We are likely at a state where the initial commit hasn't been made yet.
		return "";
	}

	git_reference_ptr branch;
	GIT2_CALL_R(git_reference_resolve(Capture(branch), head.get()), "Could not resolve HEAD reference", "");

	const char *name = "";
	GIT2_CALL_R(git_branch_name(&name, branch.get()), "Could not get branch name from current branch reference", "");

	return String::utf8(name);
}

TypedArray<String> Git::_get_remotes() {
	git_strarray remote_array;
	GIT2_CALL_R(git_remote_list(&remote_array, repo.get()), "Could not get list of remotes", TypedArray<Dictionary>());

	TypedArray<String> remotes;
	for (int i = 0; i < remote_array.count; i++) {
		remotes.push_back(String::utf8(remote_array.strings[i]));
	}

	git_strarray_dispose(&remote_array); // git_remote_list 内部分配，须释放
	return remotes;
}

TypedArray<Dictionary> Git::_get_previous_commits(int32_t max_commits) {
	git_revwalk_ptr walker;
	GIT2_CALL_R(git_revwalk_new(Capture(walker), repo.get()), "Could not create new revwalk", TypedArray<Dictionary>());
	GIT2_CALL_R(git_revwalk_sorting(walker.get(), GIT_SORT_TIME), "Could not sort revwalk by time", TypedArray<Dictionary>());

	GIT2_CALL_R_IGNORE(git_revwalk_push_head(walker.get()), "Could not push HEAD to revwalk", TypedArray<Dictionary>(), { GIT_ENOTFOUND COMMA GIT_ERROR });

	git_oid oid;
	TypedArray<Dictionary> commits;
	std::vector<String> commit_ids; // 交给后台线程预计算 diff 的批次
	char commit_id[GIT_OID_HEXSZ + 1];
	for (int i = 0; !git_revwalk_next(&oid, walker.get()) && i <= max_commits; i++) {
		git_commit_ptr commit;
		GIT2_CALL_R(git_commit_lookup(Capture(commit), repo.get(), &oid), "Failed to lookup the commit", commits);

		git_oid_tostr(commit_id, GIT_OID_HEXSZ + 1, git_commit_id(commit.get()));
		String msg = String::utf8(git_commit_message(commit.get()));

		const git_signature *sig = git_commit_author(commit.get());
		String author = String::utf8(sig->name) + " <" + String::utf8(sig->email) + ">";

		commits.push_back(create_commit(msg, author, commit_id, sig->when.time, sig->when.offset));
		commit_ids.push_back(String(commit_id));
	}

	// 把这批提交转交后台线程预计算 diff：提交列表点击时 _get_diff 直接命中缓存。
	// 历史提交 diff 此前是主线程同步算（大提交动辄上千 delta，能把编辑器卡数秒）。
	{
		std::unique_lock<std::mutex> lock(bg_mutex);
		pending_commit_ids = std::move(commit_ids);
		commit_precompute_pending = true;
	}
	bg_cv.notify_all();

	return commits;
}

void Git::_fetch(const String &remote) {
	// 网络操作全部转交后台线程执行，主线程立即返回，不再冻结编辑器。
	// 结果随下一次状态快照刷新体现在面板上。
	_post_job(JOB_FETCH, remote, false);
}

void Git::_pull(const String &remote) {
	_post_job(JOB_PULL, remote, false);
}

void Git::_push(const String &remote, bool force) {
	_post_job(JOB_PUSH, remote, force);
}

void Git::_post_job(int p_type, const String &p_remote, bool p_force) {
	bool busy = false;
	{
		std::unique_lock<std::mutex> lock(bg_mutex);
		if (job_active || job_posted) {
			busy = true; // ERR_PRINT 也属 IO，挪到锁外（避免持锁打印的死锁风险）
		} else {
			posted_job.type = p_type;
			posted_job.remote = p_remote;
			posted_job.force = p_force;
			posted_job.creds_copy = creds; // 投递时刻的凭据快照，供回调在后台线程使用
			job_posted = true;
		}
	}
	if (busy) {
		ERR_PRINT("Git: 上一个网络任务尚未结束，已忽略本次请求（" + p_remote + "）。");
		return;
	}
	bg_cv.notify_all();
}

void Git::_wait_no_job() {
	// 主线程写操作与后台网络任务互斥：无任务时立即通过，有任务时等待其结束
	// （网络任务期间用户点 stage/commit 等，宁可稍等也不允许并发写索引/工作区）。
	std::unique_lock<std::mutex> lock(bg_mutex);
	bg_cv.wait(lock, [this]() { return !job_active; });
}

void Git::_run_network_job(const BgJob &p_job, git_repository *p_repo) {
	// libgit2 的 credentials 回调 payload 需要非 const 指针，这里取一份可变副本，
	// 生命周期覆盖整个网络操作期间。改名 job_creds 以免遮蔽类成员 creds。
	Credentials job_creds = p_job.creds_copy;
	switch (p_job.type) {
		case JOB_PUSH: {
			_push_impl(p_repo, job_creds, p_job.remote, p_job.force);
		} break;
		case JOB_PULL: {
			_pull_impl(p_repo, job_creds, p_job.remote);
		} break;
		case JOB_FETCH: {
			_fetch_impl(p_repo, job_creds, p_job.remote);
		} break;
		default:
			break;
	}
}

void Git::_fetch_impl(git_repository *p_repo, Credentials &p_creds, const String &p_remote) {
	print_line("Git: Performing fetch from ", p_remote);

	git_remote_ptr remote_object;
	GIT2_CALL(git_remote_lookup(Capture(remote_object), p_repo, CString(p_remote).data), "Could not lookup remote \"" + p_remote + "\"");

	git_remote_callbacks remote_cbs = GIT_REMOTE_CALLBACKS_INIT;
	remote_cbs.credentials = &credentials_cb;
	remote_cbs.update_tips = &update_cb;
	remote_cbs.sideband_progress = &progress_cb;
	remote_cbs.transfer_progress = &transfer_progress_cb;
	remote_cbs.payload = &p_creds;
	remote_cbs.push_transfer_progress = &push_transfer_progress_cb;
	remote_cbs.push_update_reference = &push_update_reference_cb;

	GIT2_CALL(git_remote_connect(remote_object.get(), GIT_DIRECTION_FETCH, &remote_cbs, nullptr, nullptr), "Could not connect to remote \"" + p_remote + "\". Are your credentials correct? Try using a PAT token (in case you are using Github) as your password");

	git_fetch_options opts = GIT_FETCH_OPTIONS_INIT;
	opts.callbacks = remote_cbs;
	GIT2_CALL(git_remote_fetch(remote_object.get(), nullptr, &opts, "fetch"), "Could not fetch data from remote");

	print_line("Git: Fetch ended");
}

void Git::_pull_impl(git_repository *p_repo, Credentials &p_creds, const String &p_remote) {
	print_line("Git: Performing pull from ", p_remote);

	git_remote_ptr remote_object;
	GIT2_CALL(git_remote_lookup(Capture(remote_object), p_repo, CString(p_remote).data), "Could not lookup remote \"" + p_remote + "\"");

	git_remote_callbacks remote_cbs = GIT_REMOTE_CALLBACKS_INIT;
	remote_cbs.credentials = &credentials_cb;
	remote_cbs.update_tips = &update_cb;
	remote_cbs.sideband_progress = &progress_cb;
	remote_cbs.transfer_progress = &transfer_progress_cb;
	remote_cbs.payload = &p_creds;
	remote_cbs.push_transfer_progress = &push_transfer_progress_cb;
	remote_cbs.push_update_reference = &push_update_reference_cb;

	GIT2_CALL(git_remote_connect(remote_object.get(), GIT_DIRECTION_FETCH, &remote_cbs, nullptr, nullptr), "Could not connect to remote \"" + p_remote + "\". Are your credentials correct? Try using a PAT token (in case you are using Github) as your password");

	git_fetch_options fetch_opts = GIT_FETCH_OPTIONS_INIT;
	fetch_opts.callbacks = remote_cbs;

	String branch_name = _current_branch_name_with(p_repo);

	CString ref_spec_str("refs/heads/" + branch_name);

	char *ref[] = { ref_spec_str.data };
	git_strarray refspec = { ref, 1 };

	GIT2_CALL(git_remote_fetch(remote_object.get(), &refspec, &fetch_opts, "pull"), "Could not fetch data from remote");

	// 合并状态要跨线程交给主线程的 _commit 使用，写回成员时持锁。
	git_oid local_merge_oid = {};
	GIT2_CALL(git_repository_fetchhead_foreach(p_repo, fetchhead_foreach_cb, &local_merge_oid), "Could not read \"FETCH_HEAD\" file");

	{
		std::unique_lock<std::mutex> lock(bg_mutex);
		pull_merge_oid = local_merge_oid;
	}

	if (git_oid_is_zero(&local_merge_oid)) {
		ERR_PRINT(vformat("Git: Could not find remote branch HEAD for %s. Try pushing the branch first.", branch_name));
		return;
	}

	git_annotated_commit_ptr fetchhead_annotated_commit;
	GIT2_CALL(git_annotated_commit_lookup(Capture(fetchhead_annotated_commit), p_repo, &local_merge_oid), "Could not get merge commit");

	const git_annotated_commit *merge_heads[] = { fetchhead_annotated_commit.get() };

	git_merge_analysis_t merge_analysis;
	git_merge_preference_t preference = GIT_MERGE_PREFERENCE_NONE;
	GIT2_CALL(git_merge_analysis(&merge_analysis, &preference, p_repo, merge_heads, 1), "Merge analysis failed");

	if (merge_analysis & GIT_MERGE_ANALYSIS_FASTFORWARD) {
		git_checkout_options ff_checkout_options = GIT_CHECKOUT_OPTIONS_INIT;

		git_reference_ptr target_ref;
		GIT2_CALL(git_repository_head(Capture(target_ref), p_repo), "Failed to get HEAD reference");

		git_object_ptr target;
		GIT2_CALL(git_object_lookup(Capture(target), p_repo, &local_merge_oid, GIT_OBJECT_COMMIT), "Failed to lookup OID " + String(git_oid_tostr_s(&local_merge_oid)));

		ff_checkout_options.checkout_strategy = GIT_CHECKOUT_SAFE;
		GIT2_CALL(git_checkout_tree(p_repo, target.get(), &ff_checkout_options), "Failed to checkout HEAD reference");

		git_reference_ptr new_target_ref;
		GIT2_CALL(git_reference_set_target(Capture(new_target_ref), target_ref.get(), &local_merge_oid, nullptr), "Failed to move HEAD reference");

		print_line("Git: Fast Forwarded");
		GIT2_CALL(git_repository_state_cleanup(p_repo), "Could not clean repository state");

	} else if (merge_analysis & GIT_MERGE_ANALYSIS_NORMAL) {
		git_merge_options merge_opts = GIT_MERGE_OPTIONS_INIT;
		git_checkout_options checkout_opts = GIT_CHECKOUT_OPTIONS_INIT;

		merge_opts.file_favor = GIT_MERGE_FILE_FAVOR_NORMAL;
		merge_opts.file_flags = (GIT_MERGE_FILE_STYLE_DIFF3 | GIT_MERGE_FILE_DIFF_MINIMAL);
		checkout_opts.checkout_strategy = (GIT_CHECKOUT_SAFE | GIT_CHECKOUT_ALLOW_CONFLICTS | GIT_CHECKOUT_CONFLICT_STYLE_MERGE);
		GIT2_CALL(git_merge(p_repo, merge_heads, 1, &merge_opts, &checkout_opts), "Merge Failed");

		git_index_ptr index;
		GIT2_CALL(git_repository_index(Capture(index), p_repo), "Could not get repository index");

		if (git_index_has_conflicts(index.get())) {
			ERR_PRINT("Git: Index has conflicts. Solve conflicts and make a merge commit.");
		} else {
			ERR_PRINT("Git: Changes are staged. Make a merge commit.");
		}

		has_merge.store(true);

	} else if (merge_analysis & GIT_MERGE_ANALYSIS_UP_TO_DATE) {
		print_line("Git: Already up to date");

		GIT2_CALL(git_repository_state_cleanup(p_repo), "Could not clean repository state");

	} else {
		ERR_PRINT("Git: Can not merge");
	}

	print_line("Git: Pull ended");
}

void Git::_push_impl(git_repository *p_repo, Credentials &p_creds, const String &p_remote, bool p_force) {
	print_line("Git: Performing push to ", p_remote);

	git_remote_ptr remote_object;
	GIT2_CALL(git_remote_lookup(Capture(remote_object), p_repo, CString(p_remote).data), "Could not lookup remote \"" + p_remote + "\"");

	git_remote_callbacks remote_cbs = GIT_REMOTE_CALLBACKS_INIT;
	remote_cbs.credentials = &credentials_cb;
	remote_cbs.update_tips = &update_cb;
	remote_cbs.sideband_progress = &progress_cb;
	remote_cbs.transfer_progress = &transfer_progress_cb;
	remote_cbs.payload = &p_creds;
	remote_cbs.push_transfer_progress = &push_transfer_progress_cb;
	remote_cbs.push_update_reference = &push_update_reference_cb;

	String msg = "Could not connect to remote \"" + p_remote + "\". Are your credentials correct? Try using a PAT token (in case you are using Github) as your password";
	GIT2_CALL(git_remote_connect(remote_object.get(), GIT_DIRECTION_PUSH, &remote_cbs, nullptr, nullptr), msg);

	String branch_name = _current_branch_name_with(p_repo);

	CString pushspec(String() + (p_force ? "+" : "") + "refs/heads/" + branch_name);
	const git_strarray refspec = { &pushspec.data, 1 };

	git_push_options push_options = GIT_PUSH_OPTIONS_INIT;
	push_options.callbacks = remote_cbs;

	GIT2_CALL(git_remote_push(remote_object.get(), &refspec, &push_options), "Failed to push");

	print_line("Git: Push ended");
}

bool Git::_checkout_branch(const String &branch_name) {
	_wait_no_job(); // 网络任务会写索引/工作区，与写操作互斥
	git_reference_ptr branch;
	GIT2_CALL_R(git_branch_lookup(Capture(branch), repo.get(), CString(branch_name).data, GIT_BRANCH_LOCAL), "Could not find branch", false);
	const char *branch_ref_name = git_reference_name(branch.get());

	git_object_ptr treeish;
	GIT2_CALL_R(git_revparse_single(Capture(treeish), repo.get(), CString(branch_name).data), "Could not find branch head", false);

	git_checkout_options opts = GIT_CHECKOUT_OPTIONS_INIT;
	opts.checkout_strategy = GIT_CHECKOUT_SAFE;
	GIT2_CALL_R(git_checkout_tree(repo.get(), treeish.get(), &opts), "Could not checkout branch tree", false);
	GIT2_CALL_R(git_repository_set_head(repo.get(), branch_ref_name), "Could not set head", false);

	_sync_refresh_status(); // 切换分支后工作区大变，立即刷新快照与失效 diff 缓存
	return true;
}

TypedArray<Dictionary> Git::_get_diff(const String &identifier, const int32_t area) {
	// 后台线程已为当前变更列表里的每个文件预计算 diff（见 _precompute_diffs），
	// 命中缓存即零耗时返回；未命中（刚改动后台尚未重算、或历史提交查看）回退
	// 为主线程同步计算，保证点击文件总能看到 diff。
	String key = identifier + "#" + String::num_int64(area);
	bool cache_hit = false;
	TypedArray<Dictionary> cached;
	{
		std::unique_lock<std::mutex> lock(bg_mutex);
		HashMap<String, TypedArray<Dictionary>>::Iterator it = diff_cache.find(key);
		if (it) {
			cache_hit = true;
			cached = it->value; // 锁内只做查找与拷贝
		}
	}
	if (cache_hit) {
		return cached;
	}

	if (!repo) {
		return TypedArray<Dictionary>();
	}
	return _compute_diff_with(repo.get(), identifier, area);
}

TypedArray<Dictionary> Git::_compute_diff_with(git_repository *p_repo, const String &identifier, const int32_t area) {
	git_diff_options opts = GIT_DIFF_OPTIONS_INIT;
	TypedArray<Dictionary> diff_contents;

	opts.context_lines = 2;
	opts.interhunk_lines = 0;
	opts.flags = GIT_DIFF_RECURSE_UNTRACKED_DIRS | GIT_DIFF_DISABLE_PATHSPEC_MATCH | GIT_DIFF_INCLUDE_UNTRACKED | GIT_DIFF_SHOW_UNTRACKED_CONTENT | GIT_DIFF_INCLUDE_TYPECHANGE;

	CString pathspec(identifier);
	opts.pathspec.strings = &pathspec.data;
	opts.pathspec.count = 1;

	git_diff_ptr diff;
	switch ((TreeArea)area) {
		case TREE_AREA_UNSTAGED: {
			GIT2_CALL_R(git_diff_index_to_workdir(Capture(diff), p_repo, nullptr, &opts), "Could not create diff for index from working directory", diff_contents);
		} break;
		case TREE_AREA_STAGED: {
			git_object_ptr obj;

			// Ignore the case when HEAD is not found. We need to compare with a null tree in the case where the HEAD reference object is empty.
			GIT2_CALL_R_IGNORE(git_revparse_single(Capture(obj), p_repo, "HEAD^{tree}"), "Could not get HEAD^{tree} object", diff_contents, { GIT_ENOTFOUND });

			git_tree_ptr tree;
			if (obj) {
				GIT2_CALL_R_IGNORE(git_tree_lookup(Capture(tree), p_repo, git_object_id(obj.get())), "Could not lookup HEAD^{tree}", diff_contents, { GIT_ENOTFOUND });
			}

			GIT2_CALL_R(git_diff_tree_to_index(Capture(diff), p_repo, tree.get(), nullptr, &opts), "Could not create diff for tree from index directory", diff_contents);
		} break;
		case TREE_AREA_COMMIT: {
			opts.pathspec = {};

			git_object_ptr obj;
			GIT2_CALL_R(git_revparse_single(Capture(obj), p_repo, pathspec.data), "Could not get object at " + identifier, diff_contents);

			git_commit_ptr commit;
			GIT2_CALL_R(git_commit_lookup(Capture(commit), p_repo, git_object_id(obj.get())), "Could not get commit " + identifier, diff_contents);

			git_commit_ptr parent;

			// We ignore the case when the parent is not found to handle the case when this commit is the root commit. We only need to diff against a null tree in that case.
			GIT2_CALL_R_IGNORE(git_commit_parent(Capture(parent), commit.get(), 0), "Could not get parent commit of " + identifier, diff_contents, { GIT_ENOTFOUND });

			git_tree_ptr commit_tree;
			GIT2_CALL_R(git_commit_tree(Capture(commit_tree), commit.get()), "Could not get commit tree of " + identifier, diff_contents);

			git_tree_ptr parent_tree;
			if (parent) {
				GIT2_CALL_R(git_commit_tree(Capture(parent_tree), parent.get()), "Could not get commit tree of " + identifier, diff_contents);
			}

			GIT2_CALL_R(git_diff_tree_to_tree(Capture(diff), p_repo, parent_tree.get(), commit_tree.get(), &opts), "Could not generate diff for commit " + identifier, diff_contents);
		} break;
	}

	diff_contents = _parse_diff(diff.get());

	return diff_contents;
}

void Git::_precompute_diffs(git_repository *p_repo, const std::vector<StatusEntry> &p_entries, HashMap<String, TypedArray<Dictionary>> &r_cache) {
	// 为当前变更列表里的每个文件预计算 diff：用户点击文件时 _get_diff 命中缓存，
	// 无需阻塞。变更条目过多时跳过（病态大仓库不在后台做海量计算），此时点击
	// 文件走 _get_diff 的主线程同步兜底。
	const size_t PRECOMPUTE_LIMIT = 400;
	if (p_entries.size() > PRECOMPUTE_LIMIT) {
		return;
	}
	for (const StatusEntry &e : p_entries) {
		String key = e.path + "#" + String::num_int64(e.tree_area);
		if (r_cache.has(key)) {
			continue;
		}
		r_cache.insert(key, _compute_diff_with(p_repo, e.path, e.tree_area));
	}
}

TypedArray<Dictionary> Git::_parse_diff(git_diff *diff) {
	TypedArray<Dictionary> diff_contents;
	for (int i = 0; i < git_diff_num_deltas(diff); i++) {
		const git_diff_delta *delta = git_diff_get_delta(diff, i);

		git_patch_ptr patch;
		GIT2_CALL_R(git_patch_from_diff(Capture(patch), diff, i), "Could not create patch from diff", TypedArray<Dictionary>());

		Dictionary diff_file = create_diff_file(String::utf8(delta->new_file.path), String::utf8(delta->old_file.path));

		TypedArray<Dictionary> diff_hunks;
		for (int j = 0; j < git_patch_num_hunks(patch.get()); j++) {
			const git_diff_hunk *git_hunk;
			size_t line_count;
			GIT2_CALL_R(git_patch_get_hunk(&git_hunk, &line_count, patch.get(), j), "Could not get hunk from patch", TypedArray<Dictionary>());

			Dictionary diff_hunk = create_diff_hunk(git_hunk->old_start, git_hunk->new_start, git_hunk->old_lines, git_hunk->new_lines);

			TypedArray<Dictionary> diff_lines;
			for (int k = 0; k < line_count; k++) {
				const git_diff_line *git_diff_line;
				GIT2_CALL_R(git_patch_get_line_in_hunk(&git_diff_line, patch.get(), j, k), "Could not get line from hunk in patch", TypedArray<Dictionary>());

				char *content = new char[git_diff_line->content_len + 1];
				std::memcpy(content, git_diff_line->content, git_diff_line->content_len);
				content[git_diff_line->content_len] = '\0';

				String status = " "; // We reserve 1 null terminated space to fill the + or the - character at git_diff_line->origin
				status[0] = git_diff_line->origin;
				diff_lines.push_back(create_diff_line(git_diff_line->new_lineno, git_diff_line->old_lineno, String::utf8(content), status));

				delete[] content;
			}

			diff_hunk = add_line_diffs_into_diff_hunk(diff_hunk, diff_lines);
			diff_hunks.push_back(diff_hunk);
		}
		diff_file = add_diff_hunks_into_diff_file(diff_file, diff_hunks);
		diff_contents.push_back(diff_file);
	}
	return diff_contents;
}

String Git::_get_vcs_name() {
	return "Git";
}

bool Git::_initialize(const String &project_path) {
	ERR_FAIL_COND_V(project_path == "", false);

	// 重连 VCS 时会重复调用 _initialize，先停掉旧的后台线程再重建。
	_stop_bg_thread();

	int init = git_libgit2_init();
	if (init > 1) {
		WARN_PRINT("Multiple libgit2 instances are running");
	}

	git_buf discovered_repo_path = {};
	if (git_repository_discover(&discovered_repo_path, CString(project_path).data, 1, nullptr) == 0) {
		repo_project_path = String::utf8(discovered_repo_path.ptr);

		print_line("Found a repository at " + repo_project_path + ".");
		git_buf_dispose(&discovered_repo_path);
	} else {
		repo_project_path = project_path;

		WARN_PRINT("Could not find any higher level repositories.");
	}

	print_line("Selected repository path: " + repo_project_path + ".");
	GIT2_CALL_R(git_repository_init(Capture(repo), CString(repo_project_path).data, 0), "Could not initialize repository", false);

	git_reference_ptr head;
	GIT2_CALL_R_IGNORE(git_repository_head(Capture(head), repo.get()), "Could not get repository HEAD", false, { GIT_EUNBORNBRANCH COMMA GIT_ENOTFOUND });

	if (!head) {
		create_gitignore_and_gitattributes();
	}

	uint8_t _entropy[8] = {};
	OS::get_singleton()->get_entropy(_entropy, sizeof(_entropy));
	// We need to create a temporary file to load the CA from (libgit2 does not support loading certificates from string or raw pem).
	const String cafile = ProjectSettings::get_singleton()->globalize_path("res://.godot/git-cas" + String::hex_encode_buffer(_entropy, sizeof(_entropy)) + ".crt");
	Ref<FileAccess> file = FileAccess::open(cafile, FileAccess::WRITE_READ);
	if (file.is_null()) {
		return false;
	}
	file->store_buffer(OS::get_singleton()->get_system_ca_certificates().to_utf8_buffer());
	file->close();
	int error = git_libgit2_opts(GIT_OPT_SET_SSL_CERT_LOCATIONS, cafile.utf8().get_data(), NULL);
	DirAccess::remove_absolute(cafile); // Always remove the file
	if (unlikely(error)) {
		ERR_PRINT("Git: Failed to load CA bundle: " + cafile + ", error: " + itos(error));
	} else {
		print_line("Git: Loaded system CA certificates");
	}

	// 初始化完成后：主线程同步首扫一次填快照（面板打开即有数据），随后交给
	// 后台线程维护快照与 diff 预计算。
	_sync_refresh_status();
	_start_bg_thread();

	// 事件驱动：编辑器文件系统一有变更（保存、导入、外部改动后的重扫描）
	// 就请求后台线程扫一次快照，平时不再靠密集轮询。与版本控制插件自身的
	// 接法一致（version_control_editor_plugin.cpp:200）。
	if (EditorFileSystem::get_singleton() != nullptr && !fs_signal_connected) {
		EditorFileSystem::get_singleton()->connect(SNAME("filesystem_changed"), callable_mp(this, &Git::_on_filesystem_changed));
		fs_signal_connected = true;
	}

	return true;
}

bool Git::_shut_down() {
	if (fs_signal_connected && EditorFileSystem::get_singleton() != nullptr) {
		EditorFileSystem::get_singleton()->disconnect(SNAME("filesystem_changed"), callable_mp(this, &Git::_on_filesystem_changed));
		fs_signal_connected = false;
	}
	_stop_bg_thread(); // 先停后台线程，确保不再有任何 libgit2 调用，再释放资源
	repo.reset(); // Destroy repo object before libgit2 shuts down
	GIT2_CALL_R(git_libgit2_shutdown(), "Could not shutdown Git Plugin", false);
	return true;
}

void Git::_on_filesystem_changed() {
	// 主线程信号回调：只置标志 + 唤醒，绝不在回调里做扫描（扫描可能耗时）。
	// 空临界区拿一下锁：保证「置位 → 通知」与后台线程「谓词检查 → 入睡」
	// 互相串行，通知不会落在检查与入睡之间被丢失（否则最坏多睡一个兜底周期）。
	scan_requested = true;
	{
		std::lock_guard<std::mutex> l(bg_mutex);
	}
	bg_cv.notify_all();
}

Git::~Git() {
	// 必须先停后台线程：线程体还会访问本对象的成员（互斥量/快照缓存），
	// 成员析构后线程再碰它们就是未定义行为。
	_stop_bg_thread();
}

void Git::_start_bg_thread() {
	if (bg_thread != nullptr) {
		return;
	}
	bg_stop = false;
	// 4.7 的 Thread 已无 Thread::create：memnew 后 start(原始函数指针, userdata)。
	bg_thread = memnew(Thread);
	bg_thread->start(&Git::_bg_trampoline, this);
}

void Git::_stop_bg_thread() {
	if (bg_thread == nullptr) {
		return;
	}
	bg_stop = true;
	bg_cv.notify_all(); // 后台线程可能正睡在 wait_for 上，先叫醒
	bg_thread->wait_to_finish();
	memdelete(bg_thread);
	bg_thread = nullptr;
}

void Git::_bg_trampoline(void *p_userdata) {
	// 新线程内命名（4.7 的 Thread::set_name 是静态方法，须由目标线程调用）。
	Thread::set_name("GitVCSWorker");
	static_cast<Git *>(p_userdata)->_bg_main();
}

void Git::_bg_main() {
	// 后台线程主体：独立 repo 句柄（libgit2 要求句柄不跨线程共用），
	// 周期扫描 status 快照 + 预计算 diff；同时执行投递过来的网络任务。
	git_repository_ptr thread_repo;
	int err = git_repository_open(Capture(thread_repo), CString(repo_project_path).data);
	if (err != 0) {
		check_errors(err, __FUNCTION__, __FILE__, __LINE__, "Could not open repository on background thread: " + repo_project_path);
		return; // 句柄打开失败：快照不再自动刷新，主线程写操作后的同步刷新仍可用
	}

	std::unique_lock<std::mutex> lock(bg_mutex);
	while (!bg_stop) {
		// 事件驱动为主：编辑器 filesystem_changed 信号或投递的任务会立即唤醒；
		// 5 秒兜底轮询只覆盖编辑器感知不到的场景（如终端里的 git 操作恰好
		// 在编辑器聚焦期间发生）。wait_for 调用时必须已持有锁。
		bg_cv.wait_for(lock, std::chrono::milliseconds(5000), [this]() {
			return bg_stop.load() || job_posted || scan_requested.load() || commit_precompute_pending;
		});
		if (bg_stop) {
			break;
		}
		scan_requested = false; // 消费扫描请求（无论由信号还是超时兜底触发都要扫）
		// 记下本轮扫描起点的写版本号：扫描期间若主线程发生写操作（版本号增长），
		// 写路径已自行刷新过快照，本轮结果就是过期的，发布时必须丢弃。
		const uint64_t gen_at_start = write_generation;
		// 锁内只做状态摘取，随后立刻解锁——下面的打印与 libgit2 调用绝不能
		// 发生在持锁窗口内：print_line/ERR_PRINT 内部要拿引擎全局锁、控制台、
		// 编辑器日志处理器等资源，与主线程交叉即成 ABBA 死锁（实测卡死形态）。
		std::vector<StatusEntry> prev = status_snapshot; // 供扫描后比对（浅拷贝条目，代价可忽略）
		BgJob job;
		bool has_job = false;
		if (job_posted) {
			job = posted_job;
			posted_job = BgJob();
			job_posted = false;
			job_active = true;
			has_job = true;
		}
		lock.unlock();

		if (has_job) {
			_run_network_job(job, thread_repo.get()); // 网络操作，可能耗时数分钟
			{
				std::lock_guard<std::mutex> l(bg_mutex);
				job_active = false;
			}
			bg_cv.notify_all(); // 唤醒可能正阻塞在 _wait_no_job 的主线程写操作
		}

		std::vector<StatusEntry> fresh;
		_scan_status_into(thread_repo.get(), fresh);

		// 与上次快照完全一致时跳过 diff 预计算并复用现有缓存：编辑器每次
		// 保存/导入都发 filesystem_changed，但绝大多数并不改变 git 状态，
		// 重算一遍 patch 是纯浪费（预计算才是扫描流程里最贵的一段）。
		const bool unchanged = _status_equal(fresh, prev);
		HashMap<String, TypedArray<Dictionary>> new_cache;
		if (!unchanged) {
			_precompute_diffs(thread_repo.get(), fresh, new_cache);
		}

		// 重新上锁发布快照（锁内零 IO，仅两次 move）。发布后【保持持锁】回到
		// 循环顶的 wait_for——wait_for 要求调用方已持有锁，未持锁调用是 UB，
		// MSVC 下实测直接挂死（后台线程消失 + 主线程永远抢不到 bg_mutex）。
		lock.lock();
		const bool stale_generation = (write_generation != gen_at_start);
		if (!stale_generation) {
			status_snapshot = std::move(fresh);
			if (!unchanged) {
				diff_cache = std::move(new_cache);
			}
		} // 写路径已刷新过时丢弃本轮结果（stale_generation 为真）

		// 提交 diff 预计算队列（仅锁内搬移，计算在解锁后进行）。
		std::vector<String> commit_todo;
		if (commit_precompute_pending) {
			commit_todo = std::move(pending_commit_ids);
			pending_commit_ids.clear();
			commit_precompute_pending = false;
		}
		lock.unlock();

		if (!commit_todo.empty()) {
			_precompute_commit_diffs(thread_repo.get(), commit_todo);
		}
		lock.lock(); // 恢复循环不变量：回到 wait_for 时必须持锁（无条件，含无队列路径）
	}
	thread_repo.reset();
}

void Git::_precompute_commit_diffs(git_repository *p_repo, const std::vector<String> &p_ids) {
	// 为提交列表当前展示的这批提交预计算 diff（后台线程调用，计算全程不持锁）。
	// 缓存键与 _get_diff 一致："<40位SHA>#<TREE_AREA_COMMIT>"。旧的提交条目
	// （不在本批次内）在合并时驱逐，内存上限即为一次提交列表的规模。
	const String commit_key_suffix = "#" + String::num_int64(TREE_AREA_COMMIT);
	HashMap<String, TypedArray<Dictionary>> fresh_map;
	for (const String &id : p_ids) {
		String key = id + commit_key_suffix;
		bool already_cached = false;
		{
			std::unique_lock<std::mutex> lock(bg_mutex);
			already_cached = diff_cache.has(key); // 面板重复刷新时整批命中，零重算
		}
		if (!already_cached) {
			fresh_map.insert(key, _compute_diff_with(p_repo, id, TREE_AREA_COMMIT));
		}
	}

	// 合并 + 驱逐不在本批次的旧提交条目（工作区键的 "#1"/"#2" 不受影响；
	// 提交键的特征是分隔符恰好位于第 40 位——SHA 固定 40 位十六进制）。
	{
		std::unique_lock<std::mutex> lock(bg_mutex);
		Vector<String> to_erase;
		for (const KeyValue<String, TypedArray<Dictionary>> &kv : diff_cache) {
			if (kv.key.rfind("#") == 40 && !fresh_map.has(kv.key)) {
				to_erase.push_back(kv.key);
			}
		}
		for (const String &k : to_erase) {
			diff_cache.erase(k);
		}
		for (const KeyValue<String, TypedArray<Dictionary>> &kv : fresh_map) {
			diff_cache.insert(kv.key, kv.value);
		}
	}
}

bool Git::_status_equal(const std::vector<StatusEntry> &p_a, const std::vector<StatusEntry> &p_b) {
	if (p_a.size() != p_b.size()) {
		return false;
	}
	for (size_t i = 0; i < p_a.size(); ++i) {
		if (p_a[i].path != p_b[i].path || p_a[i].change_type != p_b[i].change_type || p_a[i].tree_area != p_b[i].tree_area) {
			return false;
		}
	}
	return true;
}
