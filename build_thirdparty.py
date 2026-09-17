#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
build_thirdparty.py —— 构建 godot_git 模块所链接的三个第三方静态库。

为什么要有这个脚本
    SCons 从不进入 thirdparty/：SCsub 只做两件事，编本模块的源码、链接
    bin/thirdparty 下预先编好的静态库。而 .gitignore 把 bin/ 整个排除了，
    于是从远程 clone 下来的干净模块缺少这些库，编引擎会在链接阶段失败。
    本脚本就是把一份干净 clone 补齐到"可编译"状态的那一步。

可移植性
    本脚本不含任何一个写死的绝对路径：
      - 模块根目录 = 本文件所在目录，其余位置全部由它派生；
      - 目标平台与架构由本机探测得到（--platform / --arch 可覆盖）；
      - 工具链先看环境变量，再走 PATH，找不到就报错并说明怎么装，
        绝不假定某个人的安装目录。

    控制台输出一律 ASCII 英文：Windows 控制台默认代码页是 936，输出中文字节
    会变成乱码，不便粘贴日志。中文只出现在注释与 --help 里。

用法
    python build_thirdparty.py [all|openssl|libssh2|libgit2] [选项]
    python build_thirdparty.py --help

    顺序是有依赖的：OpenSSL -> libssh2 -> libgit2。只编单个目标时，
    它前面的产物必须已经存在。

    两个逃生口：--clean 删掉构建目录从头来（只删本次 target 的），--reconfigure
    强制重跑 OpenSSL 的配置（正常情况下配置没变就不会重跑，见"增量"一节）。

前置条件
    - 三个子模块的源码可用。哪个缺了就自动补哪个：
      git submodule update --init <缺失的路径>（--no-init 可禁用）。
      于是「干净 clone + 一份工具链 + 本脚本」就是全部输入。
    - CMake 与 Perl 可调用（Perl 是 OpenSSL 的 Configure 需要的，绕不开）
    - Windows 下需要一个 MSVC 工具集（Visual Studio 或 Build Tools 的 C++ 工作负载）。
      include/lib 环境由脚本自己调 vcvarsall.bat 建立，不必先手工开
      "x64 Native Tools Command Prompt"：机器级的 INCLUDE/LIB 环境变量常常是
      手工拼的残缺版本（本机就只有 include/ucrt/um 三段、缺 shared，于是
      windows.h 里的 winapifamily.h 找不到，nmake 报 C1083），所以一律以
      vcvarsall 给出的完整集合为准（VCVARSALL 环境变量可指定用它哪一份）
    - 不需要 NASM：OpenSSL 统一以 no-asm 配置（与仓库里已验证的那份产出一致）

重复运行是安全的，而且实测是增量的：
    - OpenSSL 只在"配置参数或参与配置的源文件变了"时才重跑 Configure，
      否则直接交给 nmake 增量编译（不这么做的话每次都会全量重编 1094 个
      obj，实测 981 秒，见"增量"一节的因果链）；
    - libssh2 / libgit2 复用 cmake cache，只有真正的源码改动才会重编。
"""

import argparse
import locale
import os
import platform as host_platform
import re
import shutil
import subprocess
import sys
from pathlib import Path

# ==============================================================================
# 位置：全部相对本文件推算，没有任何绝对路径
# ==============================================================================

MODULE_ROOT = Path(__file__).resolve().parent

TP_DIR = MODULE_ROOT / "thirdparty"
OPENSSL_SRC = TP_DIR / "openssl"
SSH2_SRC = TP_DIR / "ssh2" / "libssh2"
GIT2_SRC = TP_DIR / "git2" / "libgit2"

BIN_DIR = MODULE_ROOT / "bin" / "thirdparty"

# 每个目标的源码是否就位的判据：子模块自己必定带的那个文件。
SOURCE_MARKERS = {
    "openssl": OPENSSL_SRC / "Configure",
    "libssh2": SSH2_SRC / "CMakeLists.txt",
    "libgit2": GIT2_SRC / "CMakeLists.txt",
}

# 目标名 -> 子模块源码目录。传给 git 时再转成相对模块根的路径，
# 不另存一份字符串常量，免得和上面几行走散。
SUBMODULE_DIRS = {
    "openssl": OPENSSL_SRC,
    "libssh2": SSH2_SRC,
    "libgit2": GIT2_SRC,
}

# ==============================================================================
# 平台与架构命名
#
# 目录名刻意与 Godot 引擎自己的 platform/arch 命名保持一致
# （windows / linuxbsd / macos / freebsd，x86_64 / x86_32 / arm64 / arm32 …），
# 这样 SCsub 用 env["platform"] + "/" + env["arch"] 就能拼出同一个位置。
# ==============================================================================

PLATFORM_NAMES = {
    "win32": "windows",
    "cygwin": "windows",
    "msys": "windows",
    "linux": "linuxbsd",
    "linux2": "linuxbsd",
    "darwin": "macos",
}

ARCH_NAMES = {
    "amd64": "x86_64",
    "x86_64": "x86_64",
    "x64": "x86_64",
    "em64t": "x86_64",
    "i386": "x86_32",
    "i486": "x86_32",
    "i586": "x86_32",
    "i686": "x86_32",
    "x86": "x86_32",
    "ia32": "x86_32",
    "arm64": "arm64",
    "aarch64": "arm64",
    "armv8l": "arm64",
    "armv7l": "arm32",
    "armv6l": "arm32",
    "armv7": "arm32",
    "arm": "arm32",
    "riscv64": "rv64",
}

# (platform, arch) -> OpenSSL Configure 目标名。
# 这些名字取自子模块自己的 Configurations/*.conf；不在表里的组合请用
# --openssl-target 显式指定。
OPENSSL_TARGETS = {
    ("windows", "x86_64"): "VC-WIN64A",
    ("windows", "x86_32"): "VC-WIN32",
    ("windows", "arm64"): "VC-WIN64-ARM",
    ("linuxbsd", "x86_64"): "linux-x86_64",
    ("linuxbsd", "x86_32"): "linux-x86",
    ("linuxbsd", "arm64"): "linux-aarch64",
    ("linuxbsd", "arm32"): "linux-armv4",
    ("linuxbsd", "rv64"): "linux64-riscv64",
    ("macos", "x86_64"): "darwin64-x86_64-cc",
    ("macos", "arm64"): "darwin64-arm64-cc",
    ("freebsd", "x86_64"): "BSD-x86_64",
    ("freebsd", "arm64"): "BSD-aarch64",
}

# 传给所有 cmake 项目的通用参数。
#
# CMake 4.0 起移除了对 cmake_minimum_required(<3.5) 的兼容，碰到就中止并提示
# "Compatibility with CMake < 3.5 has been removed"。libssh2 1.11.0 声明的是
# 3.1（CMakeLists.txt:48），在 cmake 4.x 上必然配置失败；libgit2 声明 3.5.1、
# 它内嵌的 chromium-zlib 声明 3.11，本来就能过。
# 统一把策略版本下限抬到 3.5：对声明了更高版本的项目是无操作；cmake < 4 时该
# 变量未被使用，只会多一条 "Manually-specified variables were not used" 提示，
# 不构成错误。
CMAKE_COMMON_ARGS = ["-DCMAKE_POLICY_VERSION_MINIMUM=3.5"]

# OpenSSL 的配置选项。与仓库里那份已验证可用的产出一致：
# 静态库、无汇编（因此不需要 NASM）、不要测试套件与命令行工具、
# 去掉弱算法与 legacy provider。enable-capieng 只在 Windows 上有效。
OPENSSL_OPTIONS = [
    "no-shared",
    "no-tests",
    "no-apps",
    "no-asm",
    "no-ssl2",
    "no-ssl3",
    "no-weak-ssl-ciphers",
    "no-legacy",
]

TARGETS = ("all", "openssl", "libssh2", "libgit2")


# ==============================================================================
# 运行期上下文（由 main() 填好）
# ==============================================================================


class Context:
    """一次运行的全部路径与工具，均为运行期算出。"""

    def __init__(self, args):
        self.args = args
        self.platform = args.platform or detect_platform()
        self.arch = args.arch or detect_arch()

        # 三个库的构建目录：bin/thirdparty/<lib>/<platform>/<arch>/
        self.openssl_bld = BIN_DIR / "openssl" / self.platform / self.arch
        self.ssh2_bld = BIN_DIR / "ssh2" / self.platform / self.arch
        self.git2_bld = BIN_DIR / "git2" / self.platform / self.arch
        # OpenSSL 的安装前缀。libssh2 / libgit2 的头文件搜索路径指向这里。
        self.openssl_dest = self.openssl_bld / "dest"

        # 产物文件名：Windows 上是 .lib，类 Unix 上是 .a。
        # libssh2 与 libgit2 都在 CMakeLists 里设了 OUTPUT_NAME，
        # libgit2 在类 Unix 上产出的是 libgit2.a（CMake 自动加 lib 前缀）。
        is_win = self.platform == "windows"
        self.lib_ext = ".lib" if is_win else ".a"
        self.openssl_ssl = self.openssl_bld / ("libssl" + self.lib_ext)
        self.openssl_crypto = self.openssl_bld / ("libcrypto" + self.lib_ext)
        self.ssh2_lib = self.ssh2_bld / "src" / ("libssh2" + self.lib_ext)
        self.git2_lib = self.git2_bld / (("git2" if is_win else "libgit2") + self.lib_ext)

        self.generator = ""
        self.cmake = None
        self.perl = None
        self.make = None
        self.jobs = args.jobs or (os.cpu_count() or 1)

    @property
    def is_windows(self):
        return self.platform == "windows"


# ==============================================================================
# 探测与工具定位
# ==============================================================================


def detect_platform():
    name = sys.platform
    if name in PLATFORM_NAMES:
        return PLATFORM_NAMES[name]
    if name.startswith("freebsd"):
        return "freebsd"
    die(
        'unknown platform "%s". Pass --platform with one of: windows, linuxbsd, '
        "macos, freebsd." % name
    )


def detect_arch():
    machine = host_platform.machine().lower()
    if machine in ARCH_NAMES:
        return ARCH_NAMES[machine]
    die(
        'unknown architecture "%s". Pass --arch with one of: %s.'
        % (machine, ", ".join(sorted(set(ARCH_NAMES.values()))))
    )


def which(name):
    """在 PATH 上找一个可执行文件。

    shutil.which 在 Windows 上要靠 PATHEXT 才知道该补哪些扩展名。Python 3.12
    起它自带一份默认列表，更早的版本在 PATHEXT 没有导出时会只去找无扩展名的
    文件，于是 cmake.exe 明明在 PATH 上却找不到。这里为旧版本补上这一手。
    """
    found = shutil.which(name)
    if found or os.name != "nt":
        return found
    extensions = [e for e in os.environ.get("PATHEXT", "").split(os.pathsep) if e]
    for extension in extensions or [".COM", ".EXE", ".BAT", ".CMD"]:
        found = shutil.which(name + extension)
        if found:
            return found
    return None


def resolve_tool(env_var, candidates, probe_args, hint):
    """按「环境变量 -> PATH」的顺序定位一个可执行文件，并真正调一次确认可用。

    只做 shutil.which 是不够的：PATH 上可能存在一个跑不起来的同名文件。
    用 probe_args 实际执行一次，同时覆盖「没装」和「装了但坏了」两种情况。
    """
    override = os.environ.get(env_var, "").strip()
    tried = []
    if override:
        tried.append((env_var, override))
    for name in candidates:
        found = which(name)
        if found:
            tried.append(("PATH", found))
            break

    if not tried:
        die(
            '%s is not on PATH (looked for: %s).\n        %s\n'
            "        Set %s to its full path to override." % (candidates[0], ", ".join(candidates), hint, env_var)
        )

    for source, path in tried:
        try:
            proc = subprocess.run(
                [path] + probe_args,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
        except OSError:
            continue
        if proc.returncode == 0:
            return path, source
    die(
        '%s was found (%s) but failed to run "%s".\n        %s\n'
        "        Set %s to a working executable to override."
        % (candidates[0], tried[0][1], " ".join(probe_args), hint, env_var)
    )


# vcvarsall.bat 用的是 VS 自己的架构命名（主机_目标），与本脚本的 arch 名不同。
VCVARSALL_ARCH = {
    "x86_64": "x64",
    "x86_32": "x86",
    "arm64": "arm64",
    "arm32": "x86_arm",
}

# 这几个变量交给 vcvarsall 全权重建。调用前必须从子进程环境里摘掉：vcvarsall
# 是"把新值插到已有值前面"（prepend），不清空就会让旧的残缺列表留在尾部——
# 本机的机器级 INCLUDE 只有 include/ucrt/um 三段，正是那份残缺列表把构建卡死的，
# 留着它只会在同名头文件上引入不确定的搜索顺序。
MSVC_ENV_KEYS = ("INCLUDE", "LIB", "LIBPATH")


def find_vcvarsall():
    """定位 vcvarsall.bat；找不到返回 None。

    顺序是 VCVARSALL 环境变量 -> vswhere。用 vswhere 而不是猜安装路径：它由
    VS 安装器放在固定位置，且把 Build Tools 与完整 IDE 一视同仁地列出来，
    不依赖当前 PATH 里有什么。
    """
    override = os.environ.get("VCVARSALL", "").strip()
    if override:
        return override if Path(override).is_file() else None

    vswhere = which("vswhere")
    if not vswhere:
        # vswhere 的位置与 VS 版本、安装盘符无关，是安装器的固定组件。
        # 用环境变量拼而不是写死 Program Files (x86) 的盘符路径：系统装在别的盘上
        # 那个字面量就是错的，而且它会让"没有硬编码路径"这条断言失效。
        for var in ("ProgramFiles(x86)", "ProgramFiles"):
            root = os.environ.get(var, "").strip()
            if not root:
                continue
            probe = Path(root) / "Microsoft Visual Studio" / "Installer" / "vswhere.exe"
            if probe.is_file():
                vswhere = str(probe)
                break
    if not vswhere:
        return None

    try:
        proc = subprocess.run(
            [vswhere, "-latest", "-products", "*", "-property", "installationPath"],
            stdout=subprocess.PIPE,
            stderr=subprocess.DEVNULL,
            text=True,
            errors="replace",
        )
    except OSError:
        return None
    if proc.returncode != 0:
        return None

    for root in (proc.stdout or "").splitlines():
        root = root.strip()
        if not root:
            continue
        candidate = Path(root) / "VC" / "Auxiliary" / "Build" / "vcvarsall.bat"
        if candidate.is_file():
            return str(candidate)
    return None


def load_msvc_environment(ctx):
    """在 Windows 上用 vcvarsall.bat 建立完整的 MSVC 环境，成功返回 True。

    为什么不直接沿用进程里的 INCLUDE/LIB：这两个变量在 Windows 上是全局持久
    的，常被手工设成残缺版本——本机的机器级变量就只有 include/ucrt/um 三段，
    少了 shared，于是 windows.h 里的 winapifamily.h 找不到，OpenSSL 的 nmake
    报 C1083 直接停工，而报错信息只会说"无法打开包括文件"，指不到环境变量上。
    vcvarsall 给出的是该工具集权威且完整的集合，所以以它为准：这是一次
    "让脏环境失效"的动作，而不是在一份可疑的列表上打补丁。

    vcvarsall.bat 是批处理，必须经 cmd 调用；用 `call ... && set` 把改完的
    环境整份取回来注入本进程，之后所有子进程（cmake / nmake / cl）自动继承。
    """
    vcvarsall = find_vcvarsall()
    if not vcvarsall:
        return False

    # VCVARSALL_ARCH 可覆盖映射：--arch 用的是 Godot 的架构名，而测试用的哨兵架构
    # （或将来某个自定义架构名）不在表里，可能仍需要一个能交给 vcvarsall 的名字。
    arch = os.environ.get("VCVARSALL_ARCH", "").strip() or VCVARSALL_ARCH.get(ctx.arch)
    if not arch:
        die(
            'no vcvarsall architecture for arch "%s". Pass --arch with one of: %s,\n'
            "        or set VCVARSALL_ARCH to the name vcvarsall.bat should receive."
            % (ctx.arch, ", ".join(sorted(VCVARSALL_ARCH)))
        )

    # 这里特意传字符串而不是列表：cmd 的引号规则是"若 /c 之后的命令行以引号
    # 开头、首尾是一对引号，就剥掉这一对"，内层的路径引号因此原样交给 call。
    # 交给 subprocess 去拼反而会按 MSVC 规则加 \" 转义，而 cmd 不认那个。
    cmdline = 'cmd.exe /c "call "%s" %s >nul && set"' % (vcvarsall, arch)
    # 摘掉旧的 INCLUDE/LIB/LIBPATH 再调（理由见 MSVC_ENV_KEYS 处的注释）。
    # PATH 必须留着：cmd 要靠它找到 vcvarsall 与被它调用的工具。
    clean_env = {
        key: value
        for key, value in os.environ.items()
        if key.upper() not in MSVC_ENV_KEYS
    }
    proc = subprocess.run(
        cmdline, env=clean_env, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL
    )
    if proc.returncode != 0:
        return False

    # `set` 的输出跟随控制台代码页（中文 Windows 上是 936），按本地区编码解。
    text = (proc.stdout or b"").decode(locale.getpreferredencoding(False), errors="replace")
    applied = 0
    for line in text.splitlines():
        key, sep, value = line.partition("=")
        # 跳过"驱动器当前目录"伪变量：形如 =C:=... 的键以 = 开头
        if not sep or not key or key.startswith("="):
            continue
        os.environ[key.upper()] = value
        applied += 1

    if applied:
        log("msvc env   : %s (%s)" % (vcvarsall, arch))
    return applied > 0


def default_generator(ctx):
    """按平台挑一个合理的 CMake 生成器。

    Windows 默认用 NMake Makefiles：与仓库里已验证的那份产出一致（单配置，
    库直接落在构建目录根部，正是 SCsub 链接的位置）。
    类 Unix 上 Ninja 更快，没有就退回 Unix Makefiles。
    """
    if ctx.is_windows:
        return "NMake Makefiles"
    if which("ninja"):
        return "Ninja"
    return "Unix Makefiles"


def resolve_tools(ctx):
    env_generator = os.environ.get("CMAKE_GENERATOR", "").strip()
    ctx.generator = ctx.args.generator or env_generator or default_generator(ctx)

    uses_nmake = "NMake" in ctx.generator
    if ctx.is_windows:
        # 先把 MSVC 环境摆好，再去找 cl / nmake：机器级的 INCLUDE/LIB 可能是
        # 残缺的，那样即使工具都在 PATH 上，编译也会死在找不到 SDK 头上。
        if not load_msvc_environment(ctx):
            log("")
            log("[WARN] vcvarsall.bat was not found; using the environment as-is.")
            log("       If the build stops on a missing SDK header, run this script")
            log('       from an "x64 Native Tools Command Prompt for VS", or point')
            log("       VCVARSALL at vcvarsall.bat explicitly.")

    if uses_nmake:
        if not which("cl"):
            die(
                "cl.exe is not on PATH, so the NMake generator cannot work.\n"
                "        Install the C++ workload of Visual Studio (or Build Tools),\n"
                "        or pass --generator to use a compiler that is on PATH."
            )
        if not which("nmake"):
            die(
                "nmake.exe is not on PATH, so the NMake generator cannot work.\n"
                "        It ships with the C++ workload of Visual Studio / Build Tools."
            )

    ctx.cmake, cmake_src = resolve_tool(
        "CMAKE",
        ["cmake"],
        ["--version"],
        "Install CMake and put it on PATH (https://cmake.org/download/).",
    )
    # perl 只有 OpenSSL 的 Configure 需要（libssh2 / libgit2 走 cmake，不碰它），
    # 所以单独编后两个时不该因为机器上没有 perl 就退出。
    if "openssl" in selected_targets(ctx.args.target):
        ctx.perl, perl_src = resolve_tool(
            "PERL",
            ["perl"],
            ["-v"],
            "OpenSSL's Configure is a Perl script and cannot run without it.\n        "
            "Install Perl and put it on PATH (Windows: Strawberry Perl or the\n        "
            "portable build from the same project).",
        )
    else:
        perl_src = "not needed (OpenSSL is not in this build)"

    # OpenSSL 不走 CMake，需要直接调 make/nmake。这里一律解析成完整路径：
    # 裸名字交给 CreateProcess/execvp 去找时，前者不会按 PATHEXT 补扩展名，
    # 只认 .exe，于是放在 PATH 上的包装脚本反而轮不到。
    make_override = os.environ.get("MAKE", "").strip()
    if make_override:
        ctx.make = make_override
    elif ctx.is_windows:
        ctx.make = which("nmake") or "nmake"
    else:
        ctx.make = which("make") or which("gmake")
        if not ctx.make:
            die(
                "make is not on PATH; OpenSSL is built with make rather than cmake.\n"
                "        Set MAKE to its full path to override."
            )

    log("cmake      : %s (%s)" % (ctx.cmake, cmake_src))
    log("perl       : %s" % (("%s (%s)" % (ctx.perl, perl_src)) if ctx.perl else perl_src))
    log("make       : %s" % ctx.make)
    log("generator  : %s" % ctx.generator)
    log("platform   : %s / %s" % (ctx.platform, ctx.arch))
    log("jobs       : %d" % ctx.jobs)


def selected_targets(target):
    """把 "all" 展开成构建顺序上的一串目标名。（ORDER 定义在本文件后半段。）"""
    return ORDER if target == "all" else (target,)


def source_present(name):
    return SOURCE_MARKERS[name].is_file()


def submodule_args(names):
    """目标名 -> git 认识的子模块路径（相对模块根、正斜杠）。"""
    return [SUBMODULE_DIRS[n].relative_to(MODULE_ROOT).as_posix() for n in names]


def init_submodules(ctx, names):
    """为缺失的源码初始化子模块，成功返回 True。

    git 不在 PATH 上时返回 False，交给调用方给出可执行的提示——本脚本的
    其余部分不需要 git，不该因为补源码这件事把它变成硬依赖。
    """
    git = which("git")
    if not git:
        return False

    cmd = [git, "submodule", "update", "--init"]
    if ctx.args.depth > 0:
        cmd += ["--depth", str(ctx.args.depth)]
    cmd += submodule_args(names)

    log("")
    log("third-party sources are missing: %s" % ", ".join(names))
    log("fetching them now (network access; a first clone can take a while):")
    rc, _ = run(ctx, cmd, cwd=MODULE_ROOT)
    return rc == 0


def check_sources(ctx, targets):
    """确保目标所需的源码就位；缺了就自动补，补不上才退出。

    只检查本次要构建的那几个目标：编 libgit2 需要的是 OpenSSL 的 .lib，
    不是它的源码，所以 Openssl 源码不在不该妨碍这一步。
    """
    missing = [t for t in targets if not source_present(t)]
    if missing and not ctx.args.no_init:
        init_submodules(ctx, missing)
        missing = [t for t in missing if not source_present(t)]

    if not missing:
        return

    die(
        "third-party sources are missing:\n        %s\n"
        "        Fetch them with:\n"
        "        git submodule update --init --recursive\n"
        "        If that fails it is a network or proxy problem, not something\n"
        "        this script can work around."
        % "\n        ".join(submodule_args(missing))
    )


# ==============================================================================
# 命令行执行
# ==============================================================================


def log(message=""):
    print(message, flush=True)


def die(message):
    log("")
    log("[ERROR] %s" % message)
    sys.exit(1)


def quote(arg):
    arg = str(arg)
    if not arg or " " in arg or "\t" in arg:
        return '"%s"' % arg
    return arg


def run(ctx, cmd, cwd, capture=False, quiet=False):
    """执行一条命令。capture=True 时收集输出（同时回显），返回 (rc, text)。"""
    if not quiet:
        log("  $ %s" % " ".join(quote(c) for c in cmd))
    if capture:
        proc = subprocess.run(
            [str(c) for c in cmd],
            cwd=str(cwd),
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            errors="replace",
        )
        text = proc.stdout or ""
        if text.strip():
            for line in text.rstrip().splitlines():
                log("    | %s" % line)
        return proc.returncode, text
    proc = subprocess.run([str(c) for c in cmd], cwd=str(cwd))
    return proc.returncode, ""


def cmake_build(ctx, build_dir):
    """用 cmake --build 驱动生成器，避免自己拼 nmake/make/ninja 的参数。

    NMake 生成器不支持并行（cmake 会忽略 --parallel），所以只在别的生成器上
    才传它。
    """
    cmd = [ctx.cmake, "--build", str(build_dir)]
    if "NMake" not in ctx.generator:
        cmd += ["--parallel", str(ctx.jobs)]
    return run(ctx, cmd, cwd=build_dir)


def require_file(ctx, path, what):
    if not Path(path).is_file():
        die(
            "%s was not produced:\n        %s\n"
            "        The build reported success but the expected artifact is\n"
            "        missing. Check the build output above." % (what, path)
        )


# ==============================================================================
# libgit2 专用：确认它到底用了哪一份 libssh2
# ==============================================================================

# libgit2 只在 libssh2 完全找不到时才 FATAL_ERROR（cmake/SelectSSH.cmake:19-21）。
# 一旦机器上存在另一份 libssh2（例如系统包通过 pkg-config 被找到），配置照样
# 成功，产出的却是一个与模块捆绑版本不匹配的 git2 库 —— 这个错误要等到链接
# 引擎、甚至运行时才暴露。所以要从 cmake 的输出里确认它解析到了哪一个。
_LIBSSH2_RESOLUTION_PATTERNS = (
    re.compile(r"Found\s+LibSSH2\s*:\s*(.+)$", re.IGNORECASE),
    re.compile(r"Resolved libraries\s*:\s*(.+)$", re.IGNORECASE),
)


def check_libssh2_resolution(ctx, output, expected):
    expected_norm = _norm_path(expected)
    mentioned = []

    for line in output.splitlines():
        for pattern in _LIBSSH2_RESOLUTION_PATTERNS:
            hit = pattern.search(line)
            if hit:
                mentioned.extend(part.strip() for part in hit.group(1).split(";"))

    mentioned = [m for m in mentioned if m]
    normalized = [_norm_path(m) for m in mentioned]

    if expected_norm in normalized:
        log("  libssh2 resolved to: %s (OK)" % expected)
        return

    others = [m for m in mentioned if "ssh2" in _norm_path(m)]
    if others:
        die(
            "libgit2 resolved a DIFFERENT libssh2 than the vendored one.\n"
            "        expected : %s\n"
            "        got      : %s\n"
            "        A system libssh2 (most likely via pkg-config) took precedence.\n"
            "        Keep it out of reach and re-run: unset PKG_CONFIG_PATH or\n"
            "        remove its .pc file, then delete\n"
            "        %s/CMakeCache.txt and try again." % (expected, ", ".join(others), ctx.git2_bld)
        )

    # 控制台的措辞因生成器/版本而异，而且**重复配置时那行根本不打印**，
    # 所以这里不靠猜：改从 CMakeCache.txt 取证（每次配置都会写）。
    verdict, detail = _libssh2_from_cache(ctx, expected)
    if verdict == "ok":
        log("  libssh2 resolved to: %s (OK, from CMakeCache.txt)" % detail)
        return
    if verdict == "foreign":
        die(detail)

    # 连缓存也读不到（例如生成器把缓存放在别处）：仍然只警告，不误报。
    log(
        "  [WARN] could not tell which libssh2 was used (%s);\n"
        "         continuing. Expected library was: %s" % (detail, expected)
    )


def _norm_path(value):
    return str(value).replace("\\", "/").lower()


# pkg-config 命中时 pkg_check_modules 会把这些 INTERNAL 标记填上内容，没命中就是
# 空串。只要有一个非空，libssh2 就是从系统里找到的，而不是我们编的那份。
_LIBSSH2_PKGCONFIG_MARKERS = (
    "LIBSSH2_FOUND",
    "LIBSSH2_MODULE_NAME",
    "LIBSSH2_LIBS",
    "LIBSSH2_LIBDIR",
    "LIBSSH2_LIBS_PATHS",
)


def _cmake_cache_entries(cache_path, prefix):
    """读 CMakeCache.txt 里以 prefix 开头的项：名字 -> 值。文件读不到返回 None。

    缓存行是 `NAME:TYPE=VALUE`，注释行以 // 开头；只取我们点名要看的那几个名字，
    所以不需要完整的 CMake 语法解析。
    """
    try:
        text = cache_path.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    entries = {}
    for line in text.splitlines():
        if line.startswith("//") or line.startswith("#"):
            continue
        name_type, sep, value = line.partition("=")
        if not sep:
            continue
        name = name_type.partition(":")[0]
        if name.startswith(prefix):
            entries[name] = value.strip()
    return entries


def _libssh2_from_cache(ctx, expected):
    """从 CMakeCache.txt 判断 libgit2 究竟会链哪一份 libssh2。

    返回 ("ok" | "foreign" | "unknown", 说明)。

    为什么要看缓存：cmake 只在**首次**配置时打印 "Found LibSSH2: ..."，重复配置
    （增量运行后的常态）控制台里根本没有那行，光靠抓输出会退化成"拿不到证据"。
    缓存是每次配置都写的，而且直接对应 SelectSSH.cmake 的两条分支：
      - 走 find_package（我们要的）：用的就是缓存里的 LIBSSH2_LIBRARY；
      - 走 pkg-config（危险的那条）：pkg_check_modules 会留下非空的 INTERNAL 标记。
    把两类证据合起来看，结论是确定的：pkg-config 没命中 + 缓存里的库就是本地那份，
    就等于 SelectSSH.cmake:15 那句 set(LIBSSH2_LIBRARIES ${LIBSSH2_LIBRARY})。
    """
    entries = _cmake_cache_entries(ctx.git2_bld / "CMakeCache.txt", "LIBSSH2")
    if entries is None:
        return "unknown", "CMakeCache.txt is not readable"

    hits = [k for k in _LIBSSH2_PKGCONFIG_MARKERS if entries.get(k)]
    if hits:
        return "foreign", (
            "libgit2 resolved libssh2 through pkg-config.\n"
            "        expected : %s\n"
            "        got      : %s\n"
            "        A system libssh2 (most likely via pkg-config) took precedence.\n"
            "        Keep it out of reach and re-run: unset PKG_CONFIG_PATH or remove\n"
            "        its .pc file, then delete %s/CMakeCache.txt and try again."
            % (
                expected,
                ", ".join("%s=%s" % (k, entries[k]) for k in hits),
                ctx.git2_bld,
            )
        )

    actual = entries.get("LIBSSH2_LIBRARY")
    if not actual:
        return "unknown", "LIBSSH2_LIBRARY is not recorded in CMakeCache.txt"
    if _norm_path(actual) != _norm_path(expected):
        return "foreign", (
            "libgit2 will link a DIFFERENT libssh2 than the vendored one.\n"
            "        expected : %s\n"
            "        got      : %s\n"
            "        delete %s/CMakeCache.txt and try again."
            % (expected, actual, ctx.git2_bld)
        )
    return "ok", actual


# ==============================================================================
# OpenSSL 的增量：什么时候可以跳过 Configure
#
# OpenSSL 是"Configure 生成 Makefile + nmake 增量编译"两段式，问题出在前一段。
# 只要重跑一次 Configure，configdata.pm 就被改写；而 <bld>/include/openssl 下
# 那 27 个公开头文件（源树里只有 .h.in 模板）的规则依赖 configdata.pm
# （Makefile:1615），于是头文件全部重新生成；nmake 的 depend 阶段又把这些头挂到
# 几乎所有 obj 上，1094 个 obj 因此全判过期。实测：参数一字未变地重跑一次
# Configure，整个脚本 981 秒，其中约 15 分钟耗在这里。
#
# 所以配置没变就跳过 Configure，把增量交给 nmake。两条判据都成立才跳过：
#   1) configdata.pm 里记着的 perlargv（上次 Configure 收到的完整参数）与本次
#      要传的完全一致 —— 参数改了必须重配；
#   2) 没有"参与配置"的源文件比 configdata.pm 更新 —— 源树动了必须重配
#      （例如子模块换了 tag、手工改了 Configurations/*.conf）。
# 第 2 条刻意用 mtime 而不是内容哈希：OpenSSL 自己生成的 Makefile 就是这么判的
# （Makefile:1569 的 `makefile:` 与 :1579 的 `configdata.pm:` 规则，命中就
# `perl configdata.pm -r` 再 exit 1）。用同一套判据，脚本与 Makefile 不会各说
# 各话：宁可多配一次，也不会漏配。（代价是 git checkout 回到同样的内容也会重配
# 一次，这是"保守但正确"的方向。）
#
# 逃生口：--reconfigure 无条件重跑配置；--clean 删掉构建目录（只删本次目标的，
# 见 clean()），自然也重配。
# ==============================================================================

# 上次 Configure 的参数记在 configdata.pm 的 %config{perlargv} 里
# （configdata.pm:257，形如 "perlargv" => [ "VC-WIN64A", "no-shared", ... ]）。
_PERLARGV_RE = re.compile(r'"perlargv"\s*=>\s*\[(.*?)\]', re.DOTALL)
_PERL_STRING_RE = re.compile(r'"((?:[^"\\]|\\.)*)"')
_PERL_ESCAPES = {"\\": "\\", '"': '"', "$": "$", "n": "\n", "t": "\t", "r": "\r"}


def _perl_unquote(value):
    """还原 Perl 双引号字符串里的转义（路径里的 \\ 会写成 \\\\）。"""
    out = []
    index = 0
    while index < len(value):
        char = value[index]
        if char == "\\" and index + 1 < len(value):
            nxt = value[index + 1]
            out.append(_PERL_ESCAPES.get(nxt, nxt))
            index += 2
        else:
            out.append(char)
            index += 1
    return "".join(out)


def recorded_configure_args(configdata):
    """读回上次 Configure 的参数列表；读不出来返回 None（调用方按"需重配"处理）。"""
    try:
        text = configdata.read_text(encoding="utf-8", errors="replace")
    except OSError:
        return None
    match = _PERLARGV_RE.search(text)
    if not match:
        return None
    return [_perl_unquote(s) for s in _PERL_STRING_RE.findall(match.group(1))]


def changed_config_sources(configdata):
    """参与配置、且比 configdata.pm 更新的源文件列表（用于说明为什么要重配）。

    监视集合与 OpenSSL 自己那份 Makefile 的规则对齐：
      <src>/Configure + 全树 build.info（configdata.pm 的依赖）
      <src>/Configurations/*（Makefile 模板与目标定义）
    """
    try:
        stamp = configdata.stat().st_mtime
    except OSError:
        return ["configdata.pm is missing"]
    watched = [OPENSSL_SRC / "Configure"]
    watched += sorted(OPENSSL_SRC.rglob("build.info"))
    watched += sorted((OPENSSL_SRC / "Configurations").glob("*"))
    changed = []
    for path in watched:
        try:
            if path.stat().st_mtime > stamp:
                changed.append(path)
        except OSError:
            changed.append(path)
    return changed


def openssl_configure_argv(ctx, target):
    """本次要交给 Configure 的完整参数（不含 perl 与 Configure 脚本自身）。

    单独抽出来是因为它同时也是"配置有没有变"的比较基准，必须和真正传出去的
    是同一份，不能两处各写一遍。
    """
    options = list(OPENSSL_OPTIONS)
    if ctx.is_windows:
        options.append("enable-capieng")
    return [target] + options + [
        "--prefix=%s" % ctx.openssl_dest,
        "--openssldir=%s" % ctx.openssl_dest,
    ]


def openssl_configure_needed(ctx, argv):
    """返回 (是否需要重跑 Configure, 原因)。"""
    if ctx.args.reconfigure:
        return True, "--reconfigure"

    build_dir = ctx.openssl_bld
    configdata = build_dir / "configdata.pm"
    if not configdata.is_file() or not (build_dir / "Makefile").is_file():
        return True, "not configured yet"

    recorded = recorded_configure_args(configdata)
    if recorded is None:
        return True, "cannot read the previous options back"
    if recorded != list(argv):
        return True, "options changed"

    changed = changed_config_sources(configdata)
    if changed:
        return True, "%d config source(s) changed, e.g. %s" % (
            len(changed),
            changed[0].name,
        )
    return False, ""


def openssl_install_stale(ctx):
    """dest 里装好的库与头文件是否落后于构建产物。返回 (是否落后, 说明)。

    install_sw 是"无条件覆盖"：哪怕什么都没编，跑一次也会刷新 dest 下所有文件的
    时间戳，而 libssh2 / libgit2 正是拿 dest/include 的头和这份 .lib 去编译的，
    于是它们会被带着重编重链（实测约 2 分钟）。所以只在确实落后时才装。
    """
    for src in (ctx.openssl_ssl, ctx.openssl_crypto):
        dst = ctx.openssl_dest / "lib" / src.name
        try:
            if not dst.is_file() or dst.stat().st_mtime < src.stat().st_mtime:
                return True, "%s is older than the build output" % src.name
        except OSError:
            return True, "%s cannot be inspected" % src.name

    # 公开头文件有两个来源：构建目录里生成的（28 个，源树只有 .h.in 模板），
    # 以及源树里直接放着的。只认小写 .h —— 源树里那两个 VMS 专用头是
    # __DECC_INCLUDE_*.H，OpenSSL 的 install 规则显式跳过它们，dest 里永远没有
    # 对应文件；不排掉它们就会永远判成"落后"，install_sw 每次都会白跑一趟
    # （顺带把 libssh2/libgit2 拖去重编）。Windows 上 glob("*.h") 大小写不敏感，
    # 所以这里必须再按后缀过滤一次，不能只靠 glob。
    dest_headers = ctx.openssl_dest / "include" / "openssl"
    for src_dir in (ctx.openssl_bld / "include" / "openssl", OPENSSL_SRC / "include" / "openssl"):
        if not src_dir.is_dir():
            continue
        for src in sorted(src_dir.glob("*.h")):
            if src.suffix != ".h":
                continue
            dst = dest_headers / src.name
            try:
                if not dst.is_file() or dst.stat().st_mtime < src.stat().st_mtime:
                    return True, "header %s is older than the build output" % src.name
            except OSError:
                return True, "header %s cannot be inspected" % src.name
    return False, ""


# ==============================================================================
# 三个构建步骤
# ==============================================================================


def build_openssl(ctx):
    log("")
    log("=" * 60)
    log(" [1/3] OpenSSL -> libssl%s, libcrypto%s" % (ctx.lib_ext, ctx.lib_ext))
    log("=" * 60)

    if not ctx.args.openssl_target:
        key = (ctx.platform, ctx.arch)
        if key not in OPENSSL_TARGETS:
            die(
                "no OpenSSL target is known for %s/%s.\n"
                "        Look one up in thirdparty/openssl/Configurations/*.conf and\n"
                "        pass it as --openssl-target=<name>." % key
            )
        target = OPENSSL_TARGETS[key]
    else:
        target = ctx.args.openssl_target
    log("  Configure target: %s" % target)

    ctx.openssl_bld.mkdir(parents=True, exist_ok=True)

    # OpenSSL 不是 CMake 工程：Configure 在构建目录里跑，指向源码树。
    argv = openssl_configure_argv(ctx, target)

    # 配置没变就不重跑：这一句是整套增量的关键，重跑一次会让 1094 个 obj
    # 全部过期（因果链见上面"OpenSSL 的增量"一节）。
    reconfigure, reason = openssl_configure_needed(ctx, argv)
    if reconfigure:
        log("  configure  : running (%s)" % reason)
        rc, _ = run(
            ctx,
            [ctx.perl, str(OPENSSL_SRC / "Configure")] + argv,
            cwd=ctx.openssl_bld,
        )
        if rc != 0:
            die("OpenSSL: Configure failed.")
    else:
        log("  configure  : up to date, skipped (--reconfigure forces it)")

    # 串行构建。这里的 make 不加 -j：OpenSSL 自己生成的 Makefile 是一套
    # 生成式规则，串行是唯一不需要额外验证的用法，而这几分钟的差别无关紧要。
    rc, _ = run(ctx, [ctx.make], cwd=ctx.openssl_bld)
    if rc != 0:
        die(
            "OpenSSL: build failed.\n"
            '        If the output says "Detected changed" or asks you to run the same\n'
            "        make command again, the sources no longer match the recorded\n"
            "        configuration: re-run with --reconfigure."
        )

    # install_sw = 只装库与头文件。dest/include 是 libssh2 / libgit2 要找的
    # 头文件位置；它们链接的 .lib/.a 是构建目录根部那份，不是 dest/lib 下的拷贝。
    stale, why = openssl_install_stale(ctx)
    if reconfigure or stale:
        log("  install    : running (%s)" % (why or "the configuration just changed"))
        rc, _ = run(ctx, [ctx.make, "install_sw"], cwd=ctx.openssl_bld)
        if rc != 0:
            die("OpenSSL: install_sw failed.")
    else:
        log("  install    : dest is already in sync, skipped")

    require_file(ctx, ctx.openssl_ssl, "OpenSSL: libssl%s" % ctx.lib_ext)
    require_file(ctx, ctx.openssl_crypto, "OpenSSL: libcrypto%s" % ctx.lib_ext)
    require_file(ctx, ctx.openssl_dest / "include" / "openssl" / "ssl.h", "OpenSSL: headers")


def build_libssh2(ctx):
    log("")
    log("=" * 60)
    log(" [2/3] libssh2 -> libssh2%s" % ctx.lib_ext)
    log("=" * 60)

    ctx.ssh2_bld.mkdir(parents=True, exist_ok=True)

    # ENABLE_DEBUG_LOGGING 让 _libssh2_debug() 成为真实函数，运行期才能用
    # LIBSSH2_TRACE 打出会话 trace（环境变量在 libssh2/src/session.c 里读）。
    # 变量未设时没有额外开销；要更小的库可以用 --no-debug-logging 关掉。
    debug_logging = "OFF" if ctx.args.no_debug_logging else "ON"

    cmd = [
        ctx.cmake,
        "-G", ctx.generator,
        *CMAKE_COMMON_ARGS,
        "-DCMAKE_BUILD_TYPE=Release",
        "-DBUILD_SHARED_LIBS=OFF",
        "-DBUILD_STATIC_LIBS=ON",
        "-DBUILD_EXAMPLES=OFF",
        "-DBUILD_TESTING=OFF",
        "-DCRYPTO_BACKEND=OpenSSL",
        "-DENABLE_ZLIB_COMPRESSION=OFF",
        "-DENABLE_WERROR=OFF",
        "-DENABLE_DEBUG_LOGGING=%s" % debug_logging,
        "-DOPENSSL_ROOT_DIR=%s" % ctx.openssl_bld,
        "-DOPENSSL_INCLUDE_DIR=%s" % (ctx.openssl_dest / "include"),
        "-DOPENSSL_SSL_LIBRARY=%s" % ctx.openssl_ssl,
        "-DOPENSSL_CRYPTO_LIBRARY=%s" % ctx.openssl_crypto,
        "-DOPENSSL_USE_STATIC_LIBS=ON",
        str(SSH2_SRC),
    ]
    if ctx.is_windows:
        # 与引擎一致，静态 CRT（/MT）。
        #
        # 只给 CMAKE_MSVC_RUNTIME_LIBRARY 是**不够**的：libssh2 的 CMakeLists 声明的是
        # cmake_minimum_required(VERSION 3.1)，按该声明策略 CMP0091 默认是 OLD，
        # 而 OLD 下 CMAKE_MSVC_RUNTIME_LIBRARY 被**静默忽略** —— 实际仍走 CMake 的
        # MSVC 默认 /MD（实测 flags.make 里 C_FLAGS 就是 `/MD`）。后果是链接引擎时
        # LNK4098（msvcrt.lib 与其他库冲突）+ libssh2 每个 obj 各一条 LNK4286
        # （把 UCRT 当 dllimport），libssh2 与引擎/libgit2/OpenSSL 各用一套 CRT 堆。
        # CMAKE_POLICY_DEFAULT_CMP0091=NEW 把该策略的默认值改成 NEW，变量才生效
        # （实测 C_FLAGS 由 `/MD` 变为 `-MT`）。libgit2 不需要这个：它有自己的
        # STATIC_CRT 选项且默认 ON，本来就是 /MT。
        cmd[3:3] = [
            "-DCMAKE_POLICY_DEFAULT_CMP0091=NEW",
            "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded",
        ]
    else:
        # 静态库要被链进可执行文件，也可能被链进 Godot 的 shared_library 构建，
        # 位置无关代码两种情况都成立。
        cmd.insert(3, "-DCMAKE_POSITION_INDEPENDENT_CODE=ON")

    rc, _ = run(ctx, cmd, cwd=ctx.ssh2_bld)
    if rc != 0:
        die("libssh2: cmake configure failed.")

    rc, _ = cmake_build(ctx, ctx.ssh2_bld)
    if rc != 0:
        die("libssh2: build failed.")

    require_file(ctx, ctx.ssh2_lib, "libssh2: libssh2%s" % ctx.lib_ext)


def build_libgit2(ctx):
    log("")
    log("=" * 60)
    log(" [3/3] libgit2 -> %s" % ctx.git2_lib.name)
    log("=" * 60)

    ctx.git2_bld.mkdir(parents=True, exist_ok=True)

    # libgit2 自带 cmake/FindLibSSH2.cmake，find_package 先走 MODULE 模式，
    # 而那个模块读的是**单数** LIBSSH2_INCLUDE_DIR / LIBSSH2_LIBRARY
    # （find_path / find_library 的变量名，预置成有效值后二者都会跳过搜索）。
    #
    # 不要传复数的 LIBSSH2_INCLUDE_DIRS / LIBSSH2_LIBRARIES：cmake/SelectSSH.cmake
    # 会用单数变量把复数覆盖掉（SelectSSH.cmake:13-15），传了也会被丢弃。
    cmd = [
        ctx.cmake,
        "-G", ctx.generator,
        *CMAKE_COMMON_ARGS,
        "-DCMAKE_BUILD_TYPE=Release",
        "-DBUILD_SHARED_LIBS=OFF",
        "-DBUILD_TESTS=OFF",
        "-DBUILD_CLI=OFF",
        "-DBUILD_EXAMPLES=OFF",
        "-DUSE_SSH=ON",
        "-DUSE_HTTPS=OpenSSL",
        "-DUSE_THREADS=ON",
        "-DLIBSSH2_INCLUDE_DIR=%s" % (SSH2_SRC / "include"),
        "-DLIBSSH2_LIBRARY=%s" % ctx.ssh2_lib,
        "-DOPENSSL_ROOT_DIR=%s" % ctx.openssl_bld,
        "-DOPENSSL_INCLUDE_DIR=%s" % (ctx.openssl_dest / "include"),
        "-DOPENSSL_SSL_LIBRARY=%s" % ctx.openssl_ssl,
        "-DOPENSSL_CRYPTO_LIBRARY=%s" % ctx.openssl_crypto,
        "-DOPENSSL_USE_STATIC_LIBS=ON",
        str(GIT2_SRC),
    ]
    if ctx.is_windows:
        cmd.insert(3, "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded")
    else:
        cmd.insert(3, "-DCMAKE_POSITION_INDEPENDENT_CODE=ON")

    rc, output = run(ctx, cmd, cwd=ctx.git2_bld, capture=True)
    if rc != 0:
        die("libgit2: cmake configure failed.")

    check_libssh2_resolution(ctx, output, ctx.ssh2_lib)

    rc, _ = cmake_build(ctx, ctx.git2_bld)
    if rc != 0:
        die("libgit2: build failed.")

    require_file(ctx, ctx.git2_lib, "libgit2: %s" % ctx.git2_lib.name)


STEPS = {
    "openssl": build_openssl,
    "libssh2": build_libssh2,
    "libgit2": build_libgit2,
}
ORDER = ("openssl", "libssh2", "libgit2")


# ==============================================================================


def clean(ctx, targets):
    """删除**本次要构建的目标**的构建目录。

    语义按 target 走：`--clean libssh2` 只清 libssh2，不会连带删掉 OpenSSL 那个
    十分钟量级的构建目录（清掉就意味着 OpenSSL 从头重编）。
    """
    dirs = {
        "openssl": ctx.openssl_bld,
        "libssh2": ctx.ssh2_bld,
        "libgit2": ctx.git2_bld,
    }
    for name in targets:
        path = dirs[name]
        if path.exists():
            log("  removing %s" % path)
            shutil.rmtree(path, ignore_errors=True)


def parse_args(argv):
    parser = argparse.ArgumentParser(
        prog="build_thirdparty.py",
        description=(
            "构建 godot_git 链接的三个第三方静态库（OpenSSL / libssh2 / libgit2）。\n"
            "所有位置都在运行期推算，脚本里没有任何写死的路径。"
        ),
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog=(
            "环境变量（都是可选的覆盖入口）：\n"
            "  CMAKE             cmake 可执行文件；默认从 PATH 找\n"
            "  PERL              perl 可执行文件；默认从 PATH 找\n"
            "  MAKE              make/nmake；Windows 默认 nmake，其余默认 make\n"
            "  CMAKE_GENERATOR   CMake 生成器；默认 Windows 用 NMake Makefiles，\n"
            "                    类 Unix 优先 Ninja，否则 Unix Makefiles\n"
            "  VCVARSALL         vcvarsall.bat 的完整路径；Windows 默认用 vswhere 找\n"
            "  VCVARSALL_ARCH    交给 vcvarsall.bat 的架构名；默认由 --arch 映射得到\n"
            "                    （x86_64->x64 等），自定义架构名时才需要设置\n"
            "\n"
            "示例：\n"
            "  python build_thirdparty.py\n"
            "  python build_thirdparty.py libgit2\n"
            "  python build_thirdparty.py              # 重复运行只做增量，秒级\n"
            "  python build_thirdparty.py --clean all          # 三个都清掉重来（20–40 分钟）\n"
            "  python build_thirdparty.py --clean libssh2      # 只清 libssh2，不碰 OpenSSL 的产物\n"
            "  python build_thirdparty.py --reconfigure all    # 强制重跑 OpenSSL 配置\n"
            "  python build_thirdparty.py --depth 1 all    # 首次拉源码走浅克隆\n"
            "  CMAKE=/path/to/cmake/bin/cmake python build_thirdparty.py\n"
        ),
    )
    parser.add_argument(
        "target",
        nargs="?",
        default="all",
        help="all | openssl | libssh2 | libgit2（默认 all）",
    )
    parser.add_argument("--platform", help="覆盖目标平台名（windows/linuxbsd/macos/freebsd）")
    parser.add_argument("--arch", help="覆盖目标架构名（x86_64/x86_32/arm64/arm32/rv64）")
    parser.add_argument("--generator", help="覆盖 CMake 生成器")
    parser.add_argument("--openssl-target", help="覆盖 OpenSSL Configure 的目标名")
    parser.add_argument("--jobs", type=int, help="并行任务数（默认 = CPU 核数）")
    parser.add_argument(
        "--no-debug-logging",
        action="store_true",
        help="关闭 libssh2 的 DEBUG_LOGGING（库更小，但 LIBSSH2_TRACE 不再生效）",
    )
    parser.add_argument(
        "--no-init",
        action="store_true",
        help="源码缺失时不要自动初始化子模块（默认会执行 git submodule update --init）",
    )
    parser.add_argument(
        "--depth",
        type=int,
        default=0,
        metavar="N",
        help=(
            "子模块浅克隆深度；0 = 完整历史（默认）。浅克隆拉得快，代价是历史被截断，"
            "之后无法 checkout 任意 tag（例如把 libssh2 对齐到 1.11.1）"
        ),
    )
    parser.add_argument(
        "--clean",
        action="store_true",
        help="构建前删除构建目录；只删本次 target 的（--clean libssh2 不碰 OpenSSL）",
    )
    parser.add_argument(
        "--reconfigure",
        action="store_true",
        help=(
            "强制重跑 OpenSSL 的 Configure（正常情况下配置没变就跳过，见脚本头部说明）。"
            "只影响 OpenSSL：libssh2 / libgit2 的 cmake configure 每次都会重跑"
        ),
    )
    return parser.parse_args(argv)


def main(argv):
    # 命令行里偶尔会混进环境里来的非 UTF-8 字节（例如旧路径），
    # 别让它把一次构建变成 UnicodeEncodeError。
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            try:
                stream.reconfigure(errors="replace")
            except Exception:
                pass

    args = parse_args(argv)
    if args.target not in TARGETS:
        die('unknown target "%s". Use: %s' % (args.target, " | ".join(TARGETS)))

    ctx = Context(args)

    log("")
    log("module root : %s" % MODULE_ROOT)
    log("target      : %s" % args.target)
    log("")

    selected = selected_targets(args.target)
    check_sources(ctx, selected)
    if args.clean:
        log("cleaning build directories:")
        clean(ctx, selected)
        log("")
    resolve_tools(ctx)

    for name in selected:
        STEPS[name](ctx)

    log("")
    log("=" * 60)
    log(" done.")
    log("=" * 60)
    log(" Rebuild the engine now so it relinks against the refreshed libraries.")
    log(" Module sources, if changed, are compiled as part of that same build.")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
