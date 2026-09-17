#include "git.h"

#include <chrono>
#include <cstdio>
#include <cstring>

#include <git2/tree.h>
#include "core/object/class_db.h"
#include "core/object/callable_mp.h"
#include "core/io/file_access.h"
#include "core/io/dir_access.h"
#include "core/io/config_file.h"
#include "core/os/os.h"
#include "editor/file_system/editor_file_system.h"
#include "editor/file_system/editor_paths.h"
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

// ---------------------------------------------------------------------------
// 模块日志的统一出口（声明与设计说明见 git.h）
//
// 统一格式：与 git CLI 的对齐感来自"分区 + 内容"两段式，而不是零散前缀。
//   普通进度   Git: [push] 开始推送到 "origin"
//   成功结果   Git: [push] 完成
//   警告       Git: [warn] ...
//   错误       Git: [error] ...
// fflush 只对真实 stdout 有效（编辑器日志面板走引擎 IO 管线，不经过 stdout），
// 它保的是"重定向到文件/管道时不被进程退出吃掉"这一场景。
void git_log(const String &p_message) {
	print_line("Git: ", p_message);
	fflush(stdout);
}

void git_log_error(const String &p_message) {
	ERR_PRINT("Git: [error] " + p_message);
	fflush(stdout);
}

void git_log_warn(const String &p_message) {
	WARN_PRINT("Git: [warn] " + p_message);
	fflush(stdout);
}

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
		git_log_error(vformat("初始化：桥接脚本编译失败（错误码 %d），VCS 功能本次不可用。", (int)err));
	}
	set_script(bridge);
#else
	ERR_PRINT_ONCE("Git: [error] 初始化：需要 GDScript 模块才能建立 VCS 桥接。");
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

	ERR_PRINT(vformat("Git: [error] %s（%s:%s 第 %d 行）", message, file, function, line));
	return true;
}

// 凭据文件的路径。放在用户的编辑器配置目录下，由 EditorPaths 决定具体位置：
//   - 普通安装：%APPDATA%/Godot/godot_git/credentials.cfg
//     （editor_paths.cpp:178 = OS::get_config_path() + godot 目录名）
//   - 自包含构建（可执行文件旁有 ._sc_ 或 _sc_，本机就是这种）：
//     <exe 目录>/editor_data/godot_git/credentials.cfg
//     （editor_paths.cpp:163-170 = data_dir，与 editor_settings-4.7.tres 同处）
// 选这里而不是项目目录的理由：
//   1) 凭据属于"这台机器上的这个用户"，不属于仓库 —— 放进项目目录会被提交，
//      更糟的是会随仓库一起分发出去；
//   2) 与引擎自己存 username/密钥路径的地方一致，备份与迁移的粒度相同。
// 用 EditorPaths 而不是自己拼路径，就是为了自动跟上 --self-contained 等情形。
String Git::_secrets_file_path() {
	EditorPaths *paths = EditorPaths::get_singleton();
	if (paths == nullptr) {
		return String();
	}
	const String config_dir = paths->get_config_dir();
	if (config_dir.is_empty()) {
		return String();
	}
	return config_dir.path_join("godot_git").path_join("credentials.cfg");
}

void Git::_load_persisted_secrets() {
	if (secrets_loaded) {
		return;
	}
	secrets_loaded = true;

	const String path = _secrets_file_path();
	if (path.is_empty()) {
		// 取不到配置目录：本次会话不持久化。功能不受影响，只是重启后要重填。
		return;
	}

	ConfigFile cfg;
	if (cfg.load(path) != OK) {
		return; // 首次运行尚无该文件，属正常情况。
	}

	stored_password = cfg.get_value("credentials", "password", String());

	const Variant passphrases = cfg.get_value("credentials", "passphrases", Dictionary());
	if (passphrases.get_type() == Variant::DICTIONARY) {
		stored_passphrases = passphrases;
	}
}

void Git::_persist_secrets() {
	const String path = _secrets_file_path();
	if (path.is_empty()) {
		return;
	}

	const String dir = path.get_base_dir();
	if (!DirAccess::dir_exists_absolute(dir)) {
		const Error err = DirAccess::make_dir_recursive_absolute(dir);
		if (err != OK) {
			git_log_error(vformat("凭据：无法创建目录 \"%s\"（错误码 %d），SSH 口令与 HTTPS 密码本次会话内仍可用，但不会被记住。", dir, (int)err));
			return;
		}
	}

	ConfigFile cfg;
	cfg.load(path); // 先读后写：保留文件里可能存在的其它键，便于将来扩展
	cfg.set_value("credentials", "password", stored_password);
	cfg.set_value("credentials", "passphrases", stored_passphrases);

	const Error err = cfg.save(path);
	if (err != OK) {
		git_log_error(vformat("凭据：无法写入文件 \"%s\"（错误码 %d），SSH 口令与 HTTPS 密码本次会话内仍可用，但不会被记住。", path, (int)err));
		// 不改变内存中的值：本次会话仍然可用，只是下次启动要重填。
		// 此处不引入新的失败语义——持久化只是便利，不该让网络操作失败。
	}
}

void Git::_set_credentials(const String &username, const String &password, const String &ssh_public_key_path, const String &ssh_private_key_path, const String &ssh_passphrase) {
	_load_persisted_secrets();

	creds.username = username;
	creds.password = password;
	creds.ssh_public_key_path = ssh_public_key_path;
	creds.ssh_private_key_path = ssh_private_key_path;
	creds.ssh_passphrase = ssh_passphrase;

	// —— 口径：空 = 沿用上次填过的值，非空 = 采用并记住 ——
	//
	// 必须这么定，而不是把空值当成"用户要清空"。因为引擎在编辑器启动时就会用
	// 一整套空值调一次本函数（version_control_editor_plugin.cpp:78 的
	// NOTIFICATION_READY 分支：_load_plugin() 成功后紧接着 _set_credentials()），
	// 而对话框本身不保存口令与 HTTPS 密码两栏——若把空值当"清空"，存下来的口令
	// 会在每次启动时被自己抹掉，回到空口令连服务器、报 -16 的老样子。
	//
	// 注意规范化后的私钥路径才是归档键，且必须与 credentials_cb 用的是同一个
	// 结果（normalize_credential_path）：否则同一条路径因粘法不同而分成两个键，
	// 保存过的口令将永远取不回来。
	const String private_key = normalize_credential_path(ssh_private_key_path);
	bool dirty = false;
	bool used_stored_passphrase = false;
	bool used_stored_password = false;

	if (password.is_empty()) {
		if (!stored_password.is_empty()) {
			creds.password = stored_password;
			used_stored_password = true;
		}
	} else if (password != stored_password) {
		stored_password = password;
		dirty = true;
	}

	if (ssh_passphrase.is_empty()) {
		if (!private_key.is_empty() && stored_passphrases.has(private_key)) {
			creds.ssh_passphrase = stored_passphrases[private_key];
			used_stored_passphrase = true;
		}
	} else if (!private_key.is_empty()) {
		if (!stored_passphrases.has(private_key) || String(stored_passphrases[private_key]) != ssh_passphrase) {
			stored_passphrases[private_key] = ssh_passphrase;
			dirty = true;
		}
	}
	// private_key 为空时口令无处归档，只在本次会话内有效（不写盘）。

	if (dirty) {
		_persist_secrets();
	}

	// 兜底必须留痕：否则"对话框密码栏是空的、却认证成功了"这件事在日志里毫无
	// 痕迹，下次出问题又要从"为什么这次能成"重新猜起。
	if (used_stored_passphrase) {
		git_log(vformat("凭据：口令栏为空，沿用本模块为私钥 \"%s\" 保存的口令。"
				"要更换请在对话框里填入新口令并点应用；要清除请删除 \"%s\"。",
				private_key, _secrets_file_path()));
	}
	if (used_stored_password) {
		git_log("凭据：密码栏为空，沿用本模块保存的密码/令牌。要更换请在对话框里填入新值并点应用。");
	}
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

// 引擎在 Windows 上保存文件时会短暂生成两类临时文件，都要从变更列表里剔掉：
//   <名><数字>.tmp        —— 备份保存路径，drivers/windows/file_access_windows.cpp:200-215
//   <名>~RF<十六进制>.TMP —— 同文件 :254 的 ReplaceFileW 做原子替换时由 Windows 内部
//                            创建的中间名（引擎源码里搜不到这个命名，不是它拼出来的）
// 两者都只存在几毫秒、且可能被独占打开：列进面板不过是闪现的伪变更，而等后台线程去
// 算 diff 时文件往往已经消失，libgit2 报 ENOTFOUND(-3)，会把整个文件的 diff 一起毁掉。
//
// 判据必须对扩展名做大小写归一：String::get_extension() 只按最后一个 '.' 截取、**不转
// 小写**（core/string/ustring.cpp:5034-5041），而这两类的扩展名一个是 .tmp、一个是
// .TMP —— 只比字面量 "tmp" 就会漏掉后者（实机踩到的正是这一条）。
bool Git::_is_transient_engine_file(const String &p_path) {
	if (p_path.get_extension().to_lower() != "tmp") {
		return false;
	}

	const String base = p_path.get_basename(); // 去掉扩展名，如 "icons_data.tres~RF56c1f6"
	if (base.is_empty()) {
		return false;
	}

	if (base[base.length() - 1] >= '0' && base[base.length() - 1] <= '9') { // <名><数字>.tmp
		return true;
	}

	const int marker = base.rfind("~RF"); // <名>~RF<十六进制>.TMP
	if (marker < 0 || marker + 3 >= base.length()) {
		return false;
	}
	for (int i = marker + 3; i < base.length(); i++) {
		const char32_t c = base[i];
		const bool is_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
		if (!is_hex) {
			return false;
		}
	}
	return true;
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

		// 跳过引擎/Windows 原子保存产生的瞬态临时文件，命名规则见
		// _is_transient_engine_file()。它们在被扫到的下一刻就可能消失，
		// 留在列表里只会让面板闪现伪变更、并让后台 diff 报错。
		//
		// 额外要求"未被跟踪"：命名判据本身是启发式的（末位数字那条尤其宽），
		// 而引擎的中间产物一定是刚建立、尚未纳入版本控制的新文件。加这一道
		// 闸门后，仓库里即使真有一个叫 data1.TMP 的被跟踪文件，它的改动也
		// 不会被悄悄藏掉 —— 过滤只可能作用于未跟踪的新增项。
		const bool untracked = (entry->status & GIT_STATUS_WT_NEW) != 0;
		if (untracked && _is_transient_engine_file(path)) {
			continue;
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
		git_log_error("网络：上一个任务尚未结束，已忽略本次 " + p_remote + " 的请求。");
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
	// 每次任务重置认证计数：一次连接只允许回调给出一套凭据，见 credentials_cb。
	job_creds.auth_attempt = 0;
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

// 传输层代理选项的唯一来源。三个 _*_impl 的 connect 与 fetch/push options 都必须取自它：
// 少一处，代理就会在那一处被丢弃。libgit2 的两段机制决定了这一点：
//   1) git_remote_fetch / git_remote_push 内部都走 connect_or_reset_options()，传输层已连接时会
//      用「由 fetch/push options 派生」的参数覆盖已存的 connect_opts
//      （remote.c: connect_or_reset_options → smart.c: git_smart__set_connect_opts
//       → git_remote_connect_options_normalize）；
//   2) http 传输是每个请求都重新判决代理（http.c: lookup_proxy 读 transport->owner->connect_opts）。
//
// 取 GIT_PROXY_AUTO 而非 NONE / SPECIFIED：AUTO 让 libgit2 按 git CLI 的同一套规则解析
// remote.<name>.proxy → http.<url>.proxy → http.proxy → HTTPS_PROXY / no_proxy
// （remote.c: git_remote__http_proxy），用户换代理不必改本模块。https 目标会走 CONNECT 隧道
// （httpclient.c: use_connect_proxy 要求 scheme 为 https 且配有代理主机），故 HTTPS 远端同样可达。
// ssh 传输不读取 proxy_opts（ssh*.c 内无 proxy 引用），填了也不会影响 SSH 远端。
git_proxy_options Git::_proxy_options() {
	git_proxy_options opts = GIT_PROXY_OPTIONS_INIT;
	opts.type = GIT_PROXY_AUTO;
	return opts;
}

// 连接失败时的归类提示。
//
// 这里原本是一句无条件的 "Are your credentials correct? Try using a PAT token
// (in case you are using Github) as your password"。当真实原因是网络不可达时，
// 这句话是纯粹的误导：本项目实际踩过——本机直连 github.com:443 超时，提示却把
// 排查方向引向凭据/token，绕了一大圈。libgit2 的真实错误始终由紧随其后的
// check_errors() 原样打印，本函数只负责归类并给出相应的排查方向。
//
// GIT_EAUTH(-16) 是 libgit2 专为「远端拒绝认证」定义的返回码（errors.h:52），
// 与网络/传输失败严格区分，故优先按它判断；其余情况按 git_error_last()->klass
// 分派（错误类别由 libgit2 在出错现场设置，比包装文案可靠）。
//
// GIT_EAUTH 的成因已用 tools/ssh2-trace-probe 实测定案（同一把密钥、同一台
// ssh.github.com:443，每次只改一个变量）：
//
//   私钥口令         libssh2 返回               libgit2 返回     模块最终报出
//   ------------------------------------------------------------------------------
//   正确             0                          成功             —
//   错 / 空          -19 PUBLICKEY_UNVERIFIED   -16 GIT_EAUTH    authentication failed: ...
//   密钥文件不存在   -16 LIBSSH2_ERROR_FILE     -1               failed to authenticate SSH session
//
// 即：**加密私钥的口令为空或填错，就是 -16**。链路是：OpenSSH 格式私钥的公钥部分
// 是明文，libssh2 不需要口令就能读出来，于是请求照样发到服务器并拿到 PK_OK；等到
// 真正要用私钥签名时（userauth.c:1748 的 sign_callback）才解不开，报
// PUBLICKEY_UNVERIFIED——这条路径的文案是 "Callback returned error"（1.11.1 里依旧
// 如此），听着像调用方写错了代码（已在本模块捆绑的 libssh2 里改写为与口令相关的说明）。
// 而 ssh_libssh2.c:374 把 -19 与「服务器拒绝公钥」一起映射成 GIT_EAUTH，两者在那一层
// 不可区分。注意别把这里的 -16 与 LIBSSH2_ERROR_FILE 的 -16 混为一谈，那是两个库
// 各自的编号。
//
// 曾经把这归因于「GitHub 拒绝 SHA-1(ssh-rsa) 签名」或「RSA 需要 rsa-sha2-* 协商」，
// 已实测推翻，不要重蹈：让 OpenSSH 强制以 SHA-1 签名（ssh -o PubkeyAcceptedAlgorithms=
// ssh-rsa -T git@github.com）认证照样成功；探针用同一把 id_rsa 直接调
// libssh2_userauth_publickey_fromfile 也是 rc=0。升级 libssh2 到 1.11.1 与这个
// 问题无关：1.11.1 在 userauth.c 上只动了三处（分配失败分支的判断修正、
// privkey_file/privkey_mem 命名纠正、新增 rsa-sha2-*_cert 证书算法支持），
// 非证书路径的签名算法选择逻辑没有变。
//
// 文案层面也曾更糟：libgit2 认证被拒后会拿同一份凭据反复重试（ssh_libssh2.c 的
// while (error == GIT_EAUTH) 循环），直到服务器掐断连接，最后停在 list_auth_methods()
// 上，于是表面文案变成 "remote rejected authentication: Failed getting response"，
// 看着像连不上。现已在**模块侧**堵住：credentials_cb 第二次被问到时返回 GIT_EUSER
// 终止重试（git_callbacks.cpp），并在 libgit2 没留下失败原因时自己补一条 ——
// 于是错误码从 GIT_EAUTH(-16) 变成 GIT_EUSER(-7)，文案由本函数给。
//
// 子模块那几处属**可选增强**，不改也完整成立（错误码与可读文案都不缺，见上）：
//   libgit2/…/ssh_libssh2.c  返回 GIT_EAUTH 前用 libssh2 原话记一笔；刷新认证方法
//                            失败时不覆盖该原因。不改则由上面那条模块侧兜底代劳。
//   libssh2/src/userauth.c   "Callback returned error" 改为说明口令/密钥的文案。
//                            不改则那条 libssh2 原话仍是旧措辞。
//   libssh2/src/session.c    按 LIBSSH2_TRACE 环境变量打开 trace（纯诊断）。
// 之所以可省：引擎链接的是 bin/thirdparty 下的预编译 .lib，子模块源码根本不参与
// 构建（见 SCsub），改了不重编 .lib 就不生效；而不重编 .lib 也照样能编引擎。
String Git::_connect_failure_hint(int p_error) {
	// GIT_EUSER(-7)：credentials_cb 主动终止重试后 libgit2 原样传出的码
	// （callback 返回负值 -> ssh_libssh2.c 的 while (error == GIT_EAUTH) 循环不成立
	// -> goto done）。凡是走到这里，都意味着「同一份凭据被给了第二次」，也就是
	// 首次凭据确已被拒——只是 libgit2 把本地签名失败（口令错/空）与服务器拒绝
	// 公钥归成了同一个 GIT_EAUTH，光看码分不出来，所以文案要同时覆盖两者。
	if (p_error == GIT_EUSER) {
		return "The credentials were rejected, so the retry was stopped early. For an SSH key only the private key path is required - the public key path may be left empty, in which case the public key is derived from the private key file. With an encrypted private key an empty or wrong passphrase is the most common cause. Also check that the public key is registered on the remote and that the paths carry no surrounding quotes or spaces.";
	}

	if (p_error == GIT_EAUTH) {
		// 只有私钥路径是必需的：公钥路径留空时 libssh2 会从私钥现算公钥
		// （userauth.c:1992-2006 的 if(publickey) 分支）。所以提示先讲这一条，
		// 免得用户以为公钥没填/填错才失败，而去折腾密钥本身。
		return "Authentication was rejected by the remote. For an SSH key only the private key path is required - the public key path may be left empty, in which case the public key is derived from the private key file. With an encrypted private key, an empty or wrong passphrase is refused locally by libssh2 and reported here as a rejected authentication. Also check that the public key is registered on the remote and that the paths carry no surrounding quotes or spaces. Over HTTPS, GitHub requires a personal access token (PAT) rather than the account password.";
	}

	const git_error *lg2err = git_error_last();
	switch (lg2err != nullptr ? lg2err->klass : (int)GIT_ERROR_NONE) {
		case GIT_ERROR_NET:
			return "The remote could not be reached over the network. Check the connection, and if a proxy is required configure it for git (remote.<name>.proxy / http.<url>.proxy / http.proxy / HTTPS_PROXY).";
		case GIT_ERROR_SSL:
			return "TLS negotiation with the remote failed. Check the proxy settings and any TLS-inspecting software on the network path.";
		case GIT_ERROR_HTTP:
			return "The remote rejected the HTTPS request. Over HTTPS, GitHub requires a personal access token (PAT) rather than the account password.";
		case GIT_ERROR_SSH:
			return "The SSH transport failed. Only the private key path is required - the public key path may be left empty, in which case the public key is derived from the private key file. Check the key path and its passphrase in the local settings dialog (an encrypted private key is rejected when its passphrase is empty or wrong - the key never gets as far as the remote), that the host key is present in your known_hosts file, and that the public key is registered on the remote.";
		case GIT_ERROR_CALLBACK:
			return "A callback refused to continue. Check the credentials entered in the local settings dialog.";
		default:
			return "See the libgit2 error printed with this message for the underlying cause.";
	}
}

void Git::_fetch_impl(git_repository *p_repo, Credentials &p_creds, const String &p_remote) {
	git_log(vformat("[fetch] 开始，远端 \"%s\"", p_remote));

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

	const git_proxy_options proxy_opts = _proxy_options();

	// 拆成两步而不是写成 GIT2_CALL 的单行：C++ 未规定函数实参的求值顺序，
	// 若把 git_remote_connect 直接写在第一个参数位置，MSVC 会先算 message，
	// 于是 _connect_failure_hint 读到的是上一次的错误而非本次的。
	const int connect_error = git_remote_connect(remote_object.get(), GIT_DIRECTION_FETCH, &remote_cbs, &proxy_opts, nullptr);
	GIT2_CALL(connect_error, "Could not connect to remote \"" + p_remote + "\". " + _connect_failure_hint(connect_error));

	git_fetch_options opts = GIT_FETCH_OPTIONS_INIT;
	opts.callbacks = remote_cbs;
	opts.proxy_opts = proxy_opts;
	GIT2_CALL(git_remote_fetch(remote_object.get(), nullptr, &opts, "fetch"), "Could not fetch data from remote");

	git_log("[fetch] 完成");
}

void Git::_pull_impl(git_repository *p_repo, Credentials &p_creds, const String &p_remote) {
	git_log(vformat("[pull] 开始，远端 \"%s\"", p_remote));

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

	const git_proxy_options proxy_opts = _proxy_options();

	// 拆成两步，原因同 _fetch_impl 中的注释（实参求值顺序）。
	const int connect_error = git_remote_connect(remote_object.get(), GIT_DIRECTION_FETCH, &remote_cbs, &proxy_opts, nullptr);
	GIT2_CALL(connect_error, "Could not connect to remote \"" + p_remote + "\". " + _connect_failure_hint(connect_error));

	git_fetch_options fetch_opts = GIT_FETCH_OPTIONS_INIT;
	fetch_opts.callbacks = remote_cbs;
	fetch_opts.proxy_opts = proxy_opts;

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
		git_log_error(vformat("远程：找不到 %s 在远端的分支 HEAD，请先把该分支推上去。", branch_name));
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

		git_log("[pull] 快进合并完成");
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
			git_log_error("[pull] 索引存在冲突，请解决冲突后再提交合并结果。");
		} else {
			git_log_error("[pull] 变更已暂存，请提交合并结果以完成合并。");
		}

		has_merge.store(true);

	} else if (merge_analysis & GIT_MERGE_ANALYSIS_UP_TO_DATE) {
		git_log("[pull] 已是最新，无需合并");

		GIT2_CALL(git_repository_state_cleanup(p_repo), "Could not clean repository state");

	} else {
		git_log_error("[pull] 无法合并（未识别的合并分析结果）");
	}

	git_log("[pull] 完成");
}

void Git::_push_impl(git_repository *p_repo, Credentials &p_creds, const String &p_remote, bool p_force) {
	git_log(vformat("[push] 开始，远端 \"%s\"%s", p_remote, p_force ? "，强制" : ""));

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

	const git_proxy_options proxy_opts = _proxy_options();

	// 拆成两步，原因同 _fetch_impl 中的注释（实参求值顺序）。
	const int connect_error = git_remote_connect(remote_object.get(), GIT_DIRECTION_PUSH, &remote_cbs, &proxy_opts, nullptr);
	GIT2_CALL(connect_error, "Could not connect to remote \"" + p_remote + "\". " + _connect_failure_hint(connect_error));

	String branch_name = _current_branch_name_with(p_repo);

	CString pushspec(String() + (p_force ? "+" : "") + "refs/heads/" + branch_name);
	const git_strarray refspec = { &pushspec.data, 1 };

	git_push_options push_options = GIT_PUSH_OPTIONS_INIT;
	push_options.callbacks = remote_cbs;
	push_options.proxy_opts = proxy_opts;

	GIT2_CALL(git_remote_push(remote_object.get(), &refspec, &push_options), "Failed to push");

	git_log("[push] 完成");
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
		const int patch_error = git_patch_from_diff(Capture(patch), diff, i);
		if (patch_error != 0) {
			// 建不出单个 delta 的 patch 不该毁掉整份 diff。最常见的原因是文件在 status
			// 扫描之后、这里之前消失了（引擎原子保存的瞬态临时文件、或用户刚删掉的文件），
			// libgit2 给的是 ENOTFOUND(-3)。跳过这一条，其余 delta 照常返回。
			if (patch_error == GIT_ENOTFOUND) {
				continue;
			}
			GIT2_CALL_R(patch_error, "Could not create patch from diff", TypedArray<Dictionary>());
		}

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
		git_log_warn(vformat("初始化：进程内有 %d 个 libgit2 实例，请确认没有第二个插件同时在使用它。", init));
	}

	git_buf discovered_repo_path = {};
	if (git_repository_discover(&discovered_repo_path, CString(project_path).data, 1, nullptr) == 0) {
		repo_project_path = String::utf8(discovered_repo_path.ptr);

		git_log(vformat("初始化：在 %s 找到仓库。", repo_project_path));
		git_buf_dispose(&discovered_repo_path);
	} else {
		repo_project_path = project_path;

		git_log_warn("初始化：向上未找到已有仓库，将就地新建。");
	}

	git_log(vformat("初始化：使用的仓库路径为 %s。", repo_project_path));
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
		git_log_error(vformat("TLS：加载 CA 证书包失败（%s，错误码 %d），HTTPS 连接可能无法验证服务器。", cafile, error));
	} else {
		git_log("TLS：已加载系统 CA 证书。");
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
