import os
import sys


def is_desktop(platform):
    return platform in ["windows", "macos", "linuxbsd"]


def is_unix_like(platform):
    return platform in ["macos", "linuxbsd", "android", "ios"]


def module_supports_tools_on(platform):
    # PINAYAGAN ANG ANDROID DITO
    return is_desktop(platform) or platform == "android"


def configure(env, env_mono):
    is_android = env["platform"] == "android"
    is_ios = env["platform"] == "ios"
    is_web = env["platform"] == "web"

    if env.editor_build:
        if not module_supports_tools_on(env["platform"]):
            raise RuntimeError("This module does not currently support building for this platform for editor builds.")
        env_mono.Append(CPPDEFINES=["GD_MONO_HOT_RELOAD"])

    if is_android:
        env_mono.Append(CPPDEFINES=["ANDROID_ENABLED"])
    elif is_ios:
        env_mono.Append(CPPDEFINES=["IOS_ENABLED"])
    elif is_web:
        env_mono.Append(CPPDEFINES=["JAVASCRIPT_ENABLED"])

    if is_unix_like(env["platform"]):
        env_mono.Append(CPPDEFINES=["UNIX_ENABLED"])

    if env["platform"] == "windows":
        env_mono.Append(CPPDEFINES=["WINDOWS_ENABLED"])
    elif env["platform"] == "macos":
        env_mono.Append(CPPDEFINES=["MACOS_ENABLED"])
    elif env["platform"] == "linuxbsd":
        env_mono.Append(CPPDEFINES=["LINUXBSD_ENABLED"])
