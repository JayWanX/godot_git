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

String normalize_credential_path(const String &p_path) {
	String s = p_path.strip_edges();
	if (s.length() >= 2 && s[0] == '"' && s[s.length() - 1] == '"') {
		s = s.substr(1, s.length() - 2).strip_edges();
	}
	return s;
}

extern "C" int credentials_cb(git_cred **out, const char *url, const char *username_from_url, unsigned int allowed_types, void *payload) {
	Credentials *creds = (Credentials *)payload;
	const bool first_call = (creds->auth_attempt++ == 0);

	// 第二次被问到，说明上一次给出的凭据没能通过认证。注意原因未必在服务器一侧：
	// 口令错/空的加密私钥是在**本地签名阶段**失败的（libssh2 报
	// PUBLICKEY_UNVERIFIED），libgit2 却把它和「服务器拒绝公钥」一起归成 GIT_EAUTH。
	//
	// 这里必须收手，否则真正的失败原因会被 libgit2 自己覆盖掉：认证被拒后
	// libgit2 会回到 ssh_libssh2.c 的 `while (error == GIT_EAUTH)` 循环，把我们
	// 上一轮给的同一份凭据原样再给一次（本模块每次连接只有一套凭据，重给必然再失败），
	// 如此空转到服务器掐断连接。实测（tools/ssh2-trace-probe）单次被拒并不会断开
	// ——重试后 userauth_list 仍返回 publickey——断开是这一轮轮重试累积出来的；
	// 一旦断开，libgit2 最后就停在 list_auth_methods() 上，报出
	// "remote rejected authentication: Failed getting response"，看上去像连不上。
	//
	// 返回负值而不是 GIT_EAUTH：负值让 ssh_libssh2.c 的循环立刻 goto done 退出。
	//
	// 错误信息分两种情况处理，两条路都能用：
	//   已有一条  -> 不覆盖。libgit2 上游在认证失败这条分支上**不设** error message，
	//               所以走到这里通常是没有的；只有给 ssh_libssh2.c 打过那处可选增强
	//               （返回 GIT_EAUTH 前用 libssh2 原话记一笔）才会有。
	//   没有一条  -> 由下面这段自己补，文案覆盖「口令错/空」与「公钥未登记」两种成因。
	// 也就是说这条兜底不依赖任何子模块改动；子模块改了只是让文案换成 libssh2 的原话。
	if (!first_call) {
		const git_error *lg2err = git_error_last();
		if (lg2err == nullptr || lg2err->message == nullptr) {
			git_error_set_str(GIT_ERROR_SSH,
					"Authentication was rejected by the remote and no other credential is available. "
					"Check the SSH public/private key paths and the key passphrase in the local settings dialog "
					"(an encrypted private key is rejected when its passphrase is empty or wrong).");
		}
		return GIT_EUSER;
	}

	String proper_username = username_from_url ? username_from_url : creds->username;

	// 公钥路径与口令都可省略，**只有私钥路径是必需的**，所以判据必须用私钥：
	//   libgit2  git_credential_ssh_key_type_new()：GIT_ASSERT_ARG(privatekey)，
	//            publickey/passphrase 均判空后才 strdup
	//            （thirdparty/git2/libgit2/src/libgit2/transports/credential.c:239-259）
	//   libssh2  userauth_publickey_fromfile()：if(publickey) 读公钥文件，
	//            else 从私钥现算（thirdparty/ssh2/libssh2/src/userauth.c:1992-2006）
	// 这条链路必须与 OpenSSH 对齐：`ssh -i id_rsa` 也只认私钥，.pub 文件缺失照样能认证。
	//
	// 此处曾以「公钥路径非空」为判据，后果是只填私钥（完全合法的填法）时整个跳过
	// SSH 密钥认证，退化成用户名+密码去连 GitHub 的 SSH 端口，必然被拒并报
	// GIT_EAUTH(-16)，看上去像是密钥或算法不对。
	//
	// 空公钥必须传 nullptr 而非 ""：libssh2 靠 if(publickey) 分派，空串会走
	// file_read_publickey("") -> fopen("") 失败。注意 CharString 是临时对象，
	// 其 .data 只在完整表达式内有效，故不能先取出指针再跨语句使用。
	//
	// 规范化函数与 _set_credentials 共用一份实现（git_callbacks.h 的
	// normalize_credential_path）：口令按私钥路径归档，两边必须得到同一个键。
	const String ssh_private_key = normalize_credential_path(creds->ssh_private_key_path);
	const String ssh_public_key = normalize_credential_path(creds->ssh_public_key_path);

	if (!ssh_private_key.is_empty()) {
		if (allowed_types & GIT_CREDENTIAL_SSH_KEY) {
			// 配置排错用：把实际交给 libgit2 的参数记进日志。路径填错、口令漏填
			// 原本在日志里没有任何痕迹，只能逐个字段去猜。口令只报有无，不打印内容。
			print_line("Git: using SSH key credentials (user=\"", proper_username,
					"\", publickey=\"", ssh_public_key.is_empty() ? "(none, derived from private key)" : ssh_public_key,
					"\", privatekey=\"", ssh_private_key,
					"\", passphrase=", creds->ssh_passphrase.is_empty() ? "(empty)" : "(provided)", ")");
			return git_credential_ssh_key_new(out,
					CString(proper_username).data,
					ssh_public_key.is_empty() ? nullptr : CString(ssh_public_key).data,
					CString(ssh_private_key).data,
					CString(creds->ssh_passphrase).data);
		}
	}

	if (allowed_types & GIT_CREDENTIAL_USERPASS_PLAINTEXT) {
		print_line("Git: using username/password credentials (user=\"", proper_username,
				"\", password=", creds->password.is_empty() ? "(empty)" : "(provided)", ")");
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
