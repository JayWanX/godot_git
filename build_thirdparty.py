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

前置条件
    - 三个子模块已 checkout：git submodule update --init --recursive
    - CMake 与 Perl 可调用（Perl 是 OpenSSL 的 Configure 需要的，绕不开）
    - Windows 下还需在 "x64 Native Tools Command Prompt for VS" 里运行
      （cl.exe 与 nmake.exe 在 PATH 上；那个命令行同时把 MSVC 的
      include/lib 环境摆好，cmake 靠它识别编译器）
    - 不需要 NASM：OpenSSL 统一以 no-asm 配置（与仓库里已验证的那份产出一致）

重复运行是安全的：cmake 复用既有 cache，nmake/make 做增量编译，改一行
子模块源码只会重编那一个文件。
"""

import argparse
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
    if uses_nmake:
        if not which("cl"):
            die(
                "cl.exe is not on PATH, so the NMake generator cannot work.\n"
                '        Open an "x64 Native Tools Command Prompt for VS" and run this\n'
                "        script from there. That shell also sets the MSVC include/lib\n"
                "        environment, which is how cmake finds the compiler."
            )
        if not which("nmake"):
            die(
                "nmake.exe is not on PATH, so the NMake generator cannot work.\n"
                '        Open an "x64 Native Tools Command Prompt for VS" and run this\n'
                "        script from there."
            )

    ctx.cmake, cmake_src = resolve_tool(
        "CMAKE",
        ["cmake"],
        ["--version"],
        "Install CMake and put it on PATH (https://cmake.org/download/).",
    )
    ctx.perl, perl_src = resolve_tool(
        "PERL",
        ["perl"],
        ["-v"],
        "OpenSSL's Configure is a Perl script and cannot run without it.\n        "
        "Install Perl and put it on PATH (Windows: Strawberry Perl or the\n        "
        "portable build from the same project).",
    )

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
    log("perl       : %s (%s)" % (ctx.perl, perl_src))
    log("make       : %s" % ctx.make)
    log("generator  : %s" % ctx.generator)
    log("platform   : %s / %s" % (ctx.platform, ctx.arch))
    log("jobs       : %d" % ctx.jobs)


def check_sources():
    missing = []
    if not (OPENSSL_SRC / "Configure").is_file():
        missing.append(str(OPENSSL_SRC))
    if not (SSH2_SRC / "CMakeLists.txt").is_file():
        missing.append(str(SSH2_SRC))
    if not (GIT2_SRC / "CMakeLists.txt").is_file():
        missing.append(str(GIT2_SRC))
    if missing:
        die(
            "third-party sources are missing:\n        %s\n"
            "        Run: git submodule update --init --recursive" % "\n        ".join(missing)
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

    # 拿不到证据就不下结论：不同生成器/版本的输出措辞不一样，
    # 这里宁可放过也不要误报。
    log(
        "  [WARN] could not tell from the cmake output which libssh2 was used;\n"
        "         continuing. Expected library was: %s" % expected
    )


def _norm_path(value):
    return str(value).replace("\\", "/").lower()


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
    options = list(OPENSSL_OPTIONS)
    if ctx.is_windows:
        options.append("enable-capieng")
    cmd = [
        ctx.perl,
        str(OPENSSL_SRC / "Configure"),
        target,
    ] + options + [
        "--prefix=%s" % ctx.openssl_dest,
        "--openssldir=%s" % ctx.openssl_dest,
    ]
    rc, _ = run(ctx, cmd, cwd=ctx.openssl_bld)
    if rc != 0:
        die("OpenSSL: Configure failed.")

    # 串行构建。这里的 make 不加 -j：OpenSSL 自己生成的 Makefile 是一套
    # 生成式规则，串行是唯一不需要额外验证的用法，而这几分钟的差别无关紧要。
    rc, _ = run(ctx, [ctx.make], cwd=ctx.openssl_bld)
    if rc != 0:
        die("OpenSSL: build failed.")

    # install_sw = 只装库与头文件。dest/include 是 libssh2 / libgit2 要找的
    # 头文件位置；它们链接的 .lib/.a 是构建目录根部那份，不是 dest/lib 下的拷贝。
    rc, _ = run(ctx, [ctx.make, "install_sw"], cwd=ctx.openssl_bld)
    if rc != 0:
        die("OpenSSL: install_sw failed.")

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
        # 与引擎一致，静态 CRT（/MT）
        cmd.insert(3, "-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded")
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


def clean(ctx):
    for name, path in (
        ("openssl", ctx.openssl_bld),
        ("libssh2", ctx.ssh2_bld),
        ("libgit2", ctx.git2_bld),
    ):
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
            "\n"
            "示例：\n"
            "  python build_thirdparty.py\n"
            "  python build_thirdparty.py libgit2\n"
            "  python build_thirdparty.py --clean all\n"
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
    parser.add_argument("--clean", action="store_true", help="构建前删除本平台的构建目录")
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

    check_sources()
    if args.clean:
        log("cleaning build directories:")
        clean(ctx)
        log("")
    resolve_tools(ctx)

    selected = ORDER if args.target == "all" else (args.target,)
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
