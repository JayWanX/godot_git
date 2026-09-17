#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "git_callbacks.h"
#include "git_wrappers.h"

#include "core/os/thread.h"
#include "core/templates/hash_map.h"
#include "core/variant/dictionary.h"
#include "editor/version_control/editor_vcs_interface.h"
#include "git2.h"

struct Credentials {
	String username;
	String password;
	String ssh_public_key_path;
	String ssh_private_key_path;
	String ssh_passphrase;

	// 本次网络连接里 credentials 回调被调用的次数。每次 _run_network_job 开始时清零
	// （凭据以副本形式投递给后台线程，计数器不跨任务残留）。
	// 用途：libgit2 在认证被拒后会拿同一份凭据反复回问，直到服务器掐断连接
	// （ssh_libssh2.c 的 while (error == GIT_EAUTH) 循环）。本模块一次连接只有一套
	// 凭据，靠它判断"已经给过并被拒"，从而主动终止重试。详见 git_callbacks.cpp。
	int auth_attempt = 0;
};

class Git : public EditorVCSInterface {
	GDCLASS(Git, EditorVCSInterface);

protected:
	static void _bind_methods();

public:
	Credentials creds;
	// pull 在后台线程写、commit 在主线程读，必须原子（否则是数据竞争）。
	std::atomic<bool> has_merge{ false };
	git_repository_ptr repo;
	// pull（后台线程）写、commit（主线程）读，访问需持有 bg_mutex。
	git_oid pull_merge_oid = {};
	String repo_project_path;
	// 只在构造期写入，之后跨线程只读。
	std::unordered_map<git_status_t, ChangeType> map_changes;

	Git();
	~Git();

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

	// —— 口令持久化 ——
	//
	// 引擎的「本地设置」对话框只回填 username 与两个密钥路径，且只把这三项写进
	// EditorSettings（sources_stable/godot-4.7.2-stable/editor/version_control/
	// version_control_editor_plugin.cpp:186-188）。Password 与 SSH Passphrase 两栏
	// 既不保存也不回填，对话框每次重建都是空的；而插件在 NOTIFICATION_READY 里
	// 紧接着就用这套空值调一次 set_credentials（同文件 :78），于是重启编辑器后
	// 送到 libgit2 的口令就是空字符串——加密私钥在本地签名阶段解不开，libgit2 报
	// GIT_EAUTH(-16)。本模块自己把这两项存下来，补齐被引擎漏掉的那一半凭据。
	//
	// 存放位置见 _secrets_file_path()。明文落盘是已知取舍：ConfigFile 只提供基于
	// 口令的加密，而"保管这个口令的口令"本身无处可放，加密只是把问题挪一层。
	// 文件位于编辑器配置目录，与引擎已存的 username/密钥路径同级、同权限保护。
	//
	// 口令按私钥路径归档：口令是"这把私钥的"属性，换密钥不该串用。
	Dictionary stored_passphrases; // 私钥路径（已规范化）-> 口令
	String stored_password;        // HTTPS 密码 / PAT
	bool secrets_loaded = false;   // 惰性加载，每次实例化只读一次盘
	static String _secrets_file_path();
	void _load_persisted_secrets();
	void _persist_secrets();

	// —— 线程化改造：状态快照缓存 / diff 预计算 / 网络操作后台化 ——
	//
	// 模型：后台线程持独立的 git_repository 句柄（libgit2 要求句柄不跨线程
	// 共用），周期扫描 status 快照并预计算 diff；主线程的取数端点直接读缓存，
	// 零耗时。push/pull/fetch 等网络操作投递给后台线程执行，不再冻结编辑器。
	// 互斥约定：
	//   bg_mutex 保护 status_snapshot / diff_cache / pull_merge_oid /
	//   job_posted / job_active / posted_job；
	//   主线程的写操作（stage/commit/checkout 等）经 _wait_no_job 与后台
	//   网络任务互斥（网络任务会写索引与工作区）；
	//   后台线程的扫描/预计算只写本地数据，持锁仅发生在发布结果的一瞬。

	// 快照条目：纯数据，主线程取用时才转成 Dictionary。
	struct StatusEntry {
		String path;
		int change_type = 0;
		int tree_area = 0;
	};

	enum JobType {
		JOB_NONE,
		JOB_PUSH,
		JOB_PULL,
		JOB_FETCH,
	};

	// 后台网络任务。creds_copy 是投递时刻的凭据副本，供 libgit2 回调跨线程使用。
	struct BgJob {
		int type = JOB_NONE;
		String remote;
		bool force = false;
		Credentials creds_copy;
	};

	Thread *bg_thread = nullptr;        // 后台工作线程（须在成员析构前 join）
	std::atomic<bool> bg_stop{ false }; // 停止标志
	std::atomic<bool> scan_requested{ false }; // 主线程信号回调置位，后台线程消费后扫描
	std::mutex bg_mutex;                // 见上方互斥约定
	std::condition_variable bg_cv;      // 唤醒后台线程（投递任务 / 停止 / 请求扫描）
	bool job_active = false;            // 后台网络任务执行中
	bool job_posted = false;            // 有待执行的网络任务
	BgJob posted_job;
	std::vector<StatusEntry> status_snapshot; // 最近一次状态快照
	HashMap<String, TypedArray<Dictionary>> diff_cache; // diff 预计算缓存，键 "路径#区域"
	uint64_t write_generation = 0;      // 主线程写操作计数（bg_mutex 保护）：后台扫描期间
	                                    // 发生写操作则扫描结果作废，防止旧快照覆盖新状态
	std::vector<String> pending_commit_ids;     // 待预计算 diff 的提交 SHA 队列（bg_mutex 保护）
	bool commit_precompute_pending = false;     // 上列队列有未处理内容（bg_mutex 保护）
	bool fs_signal_connected = false;   // 仅主线程访问：是否已连接 EditorFileSystem 信号

	void _start_bg_thread();
	void _stop_bg_thread();
	static void _bg_trampoline(void *p_userdata); // 4.7 Thread 只收原始函数指针，经此跳转回成员函数
	void _bg_main();
	void _on_filesystem_changed(); // 主线程：编辑器文件系统变更信号 → 请求后台扫描
	void _scan_status_into(git_repository *p_repo, std::vector<StatusEntry> &r_out);
	static bool _status_equal(const std::vector<StatusEntry> &p_a, const std::vector<StatusEntry> &p_b);
	TypedArray<Dictionary> _status_to_array(const std::vector<StatusEntry> &p_entries);
	void _sync_refresh_status();
	void _precompute_diffs(git_repository *p_repo, const std::vector<StatusEntry> &p_entries, HashMap<String, TypedArray<Dictionary>> &r_cache);
	void _precompute_commit_diffs(git_repository *p_repo, const std::vector<String> &p_ids);
	TypedArray<Dictionary> _compute_diff_with(git_repository *p_repo, const String &identifier, int32_t area);
	String _current_branch_name_with(git_repository *p_repo);
	void _post_job(int p_type, const String &p_remote, bool p_force);
	void _wait_no_job();
	void _run_network_job(const BgJob &p_job, git_repository *p_repo);
	void _fetch_impl(git_repository *p_repo, Credentials &p_creds, const String &p_remote);
	void _pull_impl(git_repository *p_repo, Credentials &p_creds, const String &p_remote);
	void _push_impl(git_repository *p_repo, Credentials &p_creds, const String &p_remote, bool p_force);

	// 传输层代理选项的唯一来源：三个 _*_impl 的 connect 与 fetch/push options
	// 都必须取自它，详见 git.cpp 中的实现注释。
	static git_proxy_options _proxy_options();

	// 连接失败时的归类提示：按 GIT_EUSER（回调主动终止重试，见 git_callbacks.cpp 的
	// credentials_cb）、GIT_EAUTH、以及 git_error_last()->klass 分派，避免把网络层故障
	// 一律说成「凭据错误」。详见 git.cpp 中的实现注释。
	static String _connect_failure_hint(int p_error);
};
