#pragma once

#include "core/variant/array.h"
#include "git2.h"

class Git;

struct DiffHelper {
	Array *diff_hunks;
	Git *git;
};

extern "C" int progress_cb(const char *str, int len, void *data);
extern "C" int update_cb(const char *refname, const git_oid *a, const git_oid *b, void *data);
extern "C" int transfer_progress_cb(const git_indexer_progress *stats, void *payload);
extern "C" int fetchhead_foreach_cb(const char *ref_name, const char *remote_url, const git_oid *oid, unsigned int is_merge, void *payload);
extern "C" int credentials_cb(git_cred **out, const char *url, const char *username_from_url, unsigned int allowed_types, void *payload);
extern "C" int push_transfer_progress_cb(unsigned int current, unsigned int total, size_t bytes, void *payload);
extern "C" int push_update_reference_cb(const char *refname, const char *status, void *data);
extern "C" int diff_hunk_cb(const git_diff_delta *delta, const git_diff_hunk *range, void *payload);

// 规范化用户粘进「本地设置」对话框的路径：去掉首尾空白，以及成对的英文双引号。
// 引擎对话框直接取 LineEdit::get_text()，不做任何清理
// （sources_stable/godot-4.7.2-stable/editor/version_control/version_control_editor_plugin.cpp:170-172），
// 而从资源管理器「复制为路径」会带上两侧的引号，手工粘贴也常带入首尾空白。
//
// 凭据的读写两侧（git_callbacks.cpp 的 credentials_cb 与 git.cpp 的 _set_credentials）
// 必须用同一个规范化结果：模块把 SSH 口令按**私钥路径**归档，同一条路径若因粘法不同
// 被规范化成两个不同的键，保存过的口令就再也取不回来。
String normalize_credential_path(const String &p_path);
