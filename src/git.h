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
#include "editor/version_control/editor_vcs_interface.h"
#include "git2.h"

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
	git_repository_ptr bg_repo;         // 仅后台线程访问的独立句柄
	std::vector<StatusEntry> status_snapshot; // 最近一次状态快照
	HashMap<String, TypedArray<Dictionary>> diff_cache; // diff 预计算缓存，键 "路径#区域"
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
	TypedArray<Dictionary> _compute_diff_with(git_repository *p_repo, const String &identifier, int32_t area);
	String _current_branch_name_with(git_repository *p_repo);
	void _post_job(int p_type, const String &p_remote, bool p_force);
	void _wait_no_job();
	void _run_network_job(const BgJob &p_job, git_repository *p_repo);
	void _fetch_impl(git_repository *p_repo, Credentials &p_creds, const String &p_remote);
	void _pull_impl(git_repository *p_repo, Credentials &p_creds, const String &p_remote);
	void _push_impl(git_repository *p_repo, Credentials &p_creds, const String &p_remote, bool p_force);
};
