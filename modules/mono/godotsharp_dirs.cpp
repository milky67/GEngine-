/**************************************************************************/
/*  godotsharp_dirs.cpp                                                   */
/**************************************************************************/

#include "godotsharp_dirs.h"

#include "mono_gd/gd_mono.h"

#ifndef TOOLS_ENABLED
#include "utils/path_utils.h"
#endif

#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/os/os.h"

#ifdef TOOLS_ENABLED
#include "editor/file_system/editor_paths.h"
#endif

#ifndef TOOLS_ENABLED
#include "core/config/engine.h"
#ifndef ANDROID_ENABLED
#include "core/io/file_access.h"
#endif
#endif

namespace GodotSharpDirs {

String _get_expected_build_config() {
#ifdef TOOLS_ENABLED
	return "Debug";
#else

#ifdef DEBUG_ENABLED
	return "ExportDebug";
#else
	return "ExportRelease";
#endif

#endif
}

String _get_mono_user_dir() {
#ifdef TOOLS_ENABLED
	if (EditorPaths::get_singleton()) {
		return EditorPaths::get_singleton()->get_data_dir().path_join("mono");
	} else {
		String user_dir = OS::get_singleton()->get_user_data_dir();
		return user_dir.path_join("mono");
	}
#else
	return OS::get_singleton()->get_user_data_dir().path_join("mono");
#endif
}

#if !TOOLS_ENABLED
static const char *platform_name_map[][2] = {
	{ "Windows", "windows" },
	{ "macOS", "macos" },
	{ "Linux", "linuxbsd" },
	{ "FreeBSD", "linuxbsd" },
	{ "NetBSD", "linuxbsd" },
	{ "BSD", "linuxbsd" },
	{ "Android", "android" },
	{ "iOS", "ios" },
	{ "Web", "web" },
	{ nullptr, nullptr }
};

String _get_platform_name() {
	String platform_name = OS::get_singleton()->get_name();

	int idx = 0;
	while (platform_name_map[idx][0] != nullptr) {
		if (platform_name_map[idx][0] == platform_name) {
			return platform_name_map[idx][1];
		}
		idx++;
	}

	return "";
}
#endif

class _GodotSharpDirs {
public:
	String res_metadata_dir;
	String res_temp_assemblies_dir;
	String mono_user_dir;
	String api_assemblies_dir;

#ifdef TOOLS_ENABLED
	String build_logs_dir;
	String data_editor_tools_dir;
#endif

private:
	void _ensure_directory_exists(const String &p_dir) {
		if (p_dir.is_empty()) {
			return;
		}

		String global_dir = p_dir;
		if (ProjectSettings::get_singleton()) {
			global_dir = ProjectSettings::get_singleton()->globalize_path(p_dir);
		}

		Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
		if (da.is_valid() && !da->dir_exists(global_dir)) {
			da->make_dir_recursive(global_dir);
		}
	}

	_GodotSharpDirs() {
		String res_data_dir = ProjectSettings::get_singleton() ? ProjectSettings::get_singleton()->get_project_data_path().path_join("mono") : "res://.godot/mono";
		res_metadata_dir = res_data_dir.path_join("metadata");
		res_temp_assemblies_dir = res_data_dir.path_join("temp").path_join("bin").path_join(_get_expected_build_config());

		mono_user_dir = _get_mono_user_dir();
		_ensure_directory_exists(mono_user_dir);

		String exe_dir = OS::get_singleton()->get_executable_path().get_base_dir();

#ifdef TOOLS_ENABLED
		String data_dir_root = OS::get_singleton()->get_user_data_dir().path_join("GodotSharp");
		data_editor_tools_dir = data_dir_root.path_join("Tools");
		String api_assemblies_base_dir = data_dir_root.path_join("Api");
		build_logs_dir = mono_user_dir.path_join("build_logs");

#if defined(ANDROID_ENABLED) || defined(__ANDROID__)
		Vector<String> probe_roots;
		// Priority 1: App internal user data (100% permitted nang walang OS popup)
		probe_roots.push_back(OS::get_singleton()->get_user_data_dir().path_join("GodotSharp"));
		// Priority 2: APK Assets
		probe_roots.push_back("res://GodotSharp");
		// Priority 3: External Storage
		probe_roots.push_back("/storage/emulated/0/GEngine/GodotSharp");

		for (int i = 0; i < probe_roots.size(); i++) {
			String candidate = probe_roots[i];
			String global_candidate = ProjectSettings::get_singleton() ? ProjectSettings::get_singleton()->globalize_path(candidate) : candidate;

			Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
			if (da.is_valid() && da->dir_exists(global_candidate)) {
				data_dir_root = global_candidate;
				data_editor_tools_dir = data_dir_root.path_join("Tools");
				api_assemblies_base_dir = data_dir_root.path_join("Api");
				break;
			}
		}
#elif defined(MACOS_ENABLED)
		String res_dir = OS::get_singleton()->get_bundle_resource_dir();
		if (!DirAccess::exists(data_editor_tools_dir)) {
			data_editor_tools_dir = res_dir.path_join("GodotSharp").path_join("Tools");
		}
		if (!DirAccess::exists(api_assemblies_base_dir)) {
			api_assemblies_base_dir = res_dir.path_join("GodotSharp").path_join("Api");
		}
#endif
		String candidate_config_dir = api_assemblies_base_dir.path_join(GDMono::get_expected_api_build_config());
		Ref<DirAccess> da = DirAccess::create(DirAccess::ACCESS_FILESYSTEM);
		if (da.is_valid() && da->dir_exists(candidate_config_dir)) {
			api_assemblies_dir = candidate_config_dir;
		} else {
			api_assemblies_dir = api_assemblies_base_dir;
		}
#else // TOOLS_ENABLED
		String platform = _get_platform_name();
		String arch = Engine::get_singleton()->get_architecture_name();
		String appname_safe = Path::get_csharp_project_name();
		String packed_path = "res://.godot/mono/publish/" + arch;
#if defined(ANDROID_ENABLED) || defined(__ANDROID__)
		api_assemblies_dir = packed_path;
#else
		api_assemblies_dir = exe_dir.path_join("data_" + appname_safe + "_" + platform + "_" + arch);
#endif
#endif
	}

public:
	static _GodotSharpDirs &get_singleton() {
		static _GodotSharpDirs singleton;
		return singleton;
	}
};

String get_res_metadata_dir() {
	return _GodotSharpDirs::get_singleton().res_metadata_dir;
}

String get_res_temp_assemblies_dir() {
	return _GodotSharpDirs::get_singleton().res_temp_assemblies_dir;
}

String get_api_assemblies_dir() {
	return _GodotSharpDirs::get_singleton().api_assemblies_dir;
}

String get_mono_user_dir() {
	return _GodotSharpDirs::get_singleton().mono_user_dir;
}

#ifdef TOOLS_ENABLED
String get_build_logs_dir() {
	return _GodotSharpDirs::get_singleton().build_logs_dir;
}

String get_data_editor_tools_dir() {
	return _GodotSharpDirs::get_singleton().data_editor_tools_dir;
}
#endif

} // namespace GodotSharpDirs
