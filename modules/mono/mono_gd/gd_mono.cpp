/**************************************************************************/
/*  gd_mono.cpp                                                           */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/

#include "gd_mono.h"

#include "../glue/runtime_interop.h"
#include "../godotsharp_dirs.h"
#include "../thirdparty/coreclr_delegates.h"
#include "../thirdparty/hostfxr.h"
#include "../utils/path_utils.h"
#include "gd_mono_cache.h"

#ifdef DEBUG_ENABLED
#include "core/object/class_db.h"
#endif

#ifdef TOOLS_ENABLED
#include "../editor/hostfxr_resolver.h"
#include "../editor/semver.h"
#endif

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/io/dir_access.h"
#include "core/io/file_access.h"
#include "core/os/os.h"
#include "core/os/thread.h"

#ifdef UNIX_ENABLED
#include <dlfcn.h>
#endif

#if defined(ANDROID_ENABLED)
#include "../thirdparty/mono_delegates.h"
#endif

GDMono *GDMono::singleton = nullptr;

namespace {
hostfxr_initialize_for_dotnet_command_line_fn hostfxr_initialize_for_dotnet_command_line = nullptr;
hostfxr_initialize_for_runtime_config_fn hostfxr_initialize_for_runtime_config = nullptr;
hostfxr_get_runtime_delegate_fn hostfxr_get_runtime_delegate = nullptr;
hostfxr_close_fn hostfxr_close = nullptr;

typedef int(CORECLR_DELEGATE_CALLTYPE *coreclr_create_delegate_fn)(void *hostHandle, unsigned int domainId, const char *entryPointAssemblyName, const char *entryPointTypeName, const char *entryPointMethodName, void **delegate);
typedef int(CORECLR_DELEGATE_CALLTYPE *coreclr_initialize_fn)(const char *exePath, const char *appDomainFriendlyName, int propertyCount, const char **propertyKeys, const char **propertyValues, void **hostHandle, unsigned int *domainId);

coreclr_create_delegate_fn coreclr_create_delegate = nullptr;
coreclr_initialize_fn coreclr_initialize = nullptr;

#ifdef ANDROID_ENABLED
mono_install_assembly_preload_hook_fn mono_install_assembly_preload_hook = nullptr;
mono_assembly_name_get_name_fn mono_assembly_name_get_name = nullptr;
mono_assembly_name_get_culture_fn mono_assembly_name_get_culture = nullptr;
mono_image_open_from_data_with_name_fn mono_image_open_from_data_with_name = nullptr;
mono_assembly_load_from_full_fn mono_assembly_load_from_full = nullptr;
#endif

#ifdef _WIN32
static_assert(sizeof(char_t) == sizeof(char16_t));
using HostFxrCharString = Char16String;
#define HOSTFXR_STR(m_str) L##m_str
#else
static_assert(sizeof(char_t) == sizeof(char));
using HostFxrCharString = CharString;
#define HOSTFXR_STR(m_str) m_str
#endif

HostFxrCharString str_to_hostfxr(const String &p_str) {
#ifdef _WIN32
	return p_str.utf16();
#else
	return p_str.utf8();
#endif
}

const char_t *get_data(const HostFxrCharString &p_char_str) {
	return (const char_t *)p_char_str.get_data();
}

#ifdef TOOLS_ENABLED
bool try_get_dotnet_root_from_command_line(String &r_dotnet_root) {
#if defined(ANDROID_ENABLED)
	Vector<String> possible_roots;
	possible_roots.push_back("/storage/emulated/0/GEngine/dotnet");
	possible_roots.push_back("/storage/emulated/0/GEngine/GodotSharp/dotnet");
	possible_roots.push_back("/storage/emulated/0/Android/data/org.godotengine.editor.v4/files/dotnet");
	possible_roots.push_back("/storage/emulated/0/libs/dotnet");
	possible_roots.push_back("/storage/emulated/0/GodotSharp/dotnet");
	possible_roots.push_back("/sdcard/GEngine/dotnet");

	for (const String &path : possible_roots) {
		if (DirAccess::exists(path)) {
			r_dotnet_root = path;
			print_verbose(".NET: Found Android dotnet root at: " + r_dotnet_root);
			return true;
		}
	}
	return false;
#else
	String pipe;
	List<String> args;
	args.push_back("--list-sdks");

	int exitcode;
	Error err = OS::get_singleton()->execute("dotnet", args, &pipe, &exitcode, true);

	ERR_FAIL_COND_V_MSG(err != OK, false, String(".NET failed to get list of installed SDKs. Error: ") + error_names[err]);
	ERR_FAIL_COND_V_MSG(exitcode != 0, false, pipe);

	Vector<String> sdks = pipe.strip_edges().replace("\r\n", "\n").split("\n", false);

	godotsharp::SemVerParser sem_ver_parser;
	godotsharp::SemVer latest_sdk_version;
	String latest_sdk_path;

	for (const String &sdk : sdks) {
		String version_string = sdk.get_slice(" ", 0);
		String path = sdk.get_slice(" ", 1);
		path = path.substr(1, path.length() - 2);

		godotsharp::SemVer version;
		if (!sem_ver_parser.parse(version_string, version)) {
			WARN_PRINT("Unable to parse .NET SDK version '" + version_string + "'.");
			continue;
		}

		if (!DirAccess::exists(path)) {
			WARN_PRINT("Found .NET SDK version '" + version_string + "' with invalid path '" + path + "'.");
			continue;
		}

		if (version > latest_sdk_version) {
			latest_sdk_version = version;
			latest_sdk_path = path;
		}
	}

	if (!latest_sdk_path.is_empty()) {
		print_verbose("Found .NET SDK at " + latest_sdk_path);
		r_dotnet_root = latest_sdk_path.path_join("..").simplify_path();
		return true;
	}

	return false;
#endif
}
#endif

String find_hostfxr() {
#if defined(ANDROID_ENABLED)
	// Tier 1: Subukang buksan mula sa APK native libraries directory
	void *test_handle = nullptr;
	if (OS::get_singleton()->open_dynamic_library("libhostfxr.so", test_handle) == OK) {
		OS::get_singleton()->close_dynamic_library(test_handle);
		print_verbose(".NET: Found libhostfxr.so in APK internal native libraries!");
		return "libhostfxr.so";
	}

	// Tier 2, 3, 4: Subukan sa mga storage locations
	Vector<String> android_candidates;
	android_candidates.push_back("/storage/emulated/0/GEngine/GodotSharp/libhostfxr.so");
	android_candidates.push_back("/storage/emulated/0/GEngine/GodotSharp/host/fxr");
	android_candidates.push_back("/storage/emulated/0/Android/data/org.godotengine.editor.v4/files/GodotSharp/libhostfxr.so");
	android_candidates.push_back("/storage/emulated/0/libs/libhostfxr.so");
	android_candidates.push_back("/storage/emulated/0/libs/GodotSharp/libhostfxr.so");
	android_candidates.push_back("/storage/emulated/0/GodotSharp/libhostfxr.so");

	for (const String &path : android_candidates) {
		if (FileAccess::exists(path)) {
			print_verbose(".NET: Found hostfxr at storage: " + path);
			return path;
		}
		if (DirAccess::exists(path)) {
			Ref<DirAccess> da = DirAccess::open(path);
			if (da.is_valid()) {
				da->list_dir_begin();
				String dir_name = da->get_next();
				while (!dir_name.is_empty()) {
					if (da->current_is_dir() && !dir_name.begins_with(".")) {
						String full_path = path.path_join(dir_name).path_join("libhostfxr.so");
						if (FileAccess::exists(full_path)) {
							print_verbose(".NET: Found hostfxr in subfolder: " + full_path);
							return full_path;
						}
					}
					dir_name = da->get_next();
				}
			}
		}
	}

	return "libhostfxr.so";
#else
#ifdef TOOLS_ENABLED
	String dotnet_root;
	String fxr_path;
	if (godotsharp::hostfxr_resolver::try_get_path(dotnet_root, fxr_path)) {
		return fxr_path;
	}

	if (try_get_dotnet_root_from_command_line(dotnet_root)) {
		if (godotsharp::hostfxr_resolver::try_get_path_from_dotnet_root(dotnet_root, fxr_path)) {
			return fxr_path;
		}
	}

	ERR_PRINT(String() + ".NET: One of the dependent libraries is missing. " +
			"Typically when the `hostfxr`, `hostpolicy` or `coreclr` dynamic " +
			"libraries are not present in the expected locations.");

	return String();
#else
#if defined(WINDOWS_ENABLED)
	String probe_path = GodotSharpDirs::get_api_assemblies_dir().path_join("hostfxr.dll");
#elif defined(MACOS_ENABLED)
	String probe_path = GodotSharpDirs::get_api_assemblies_dir().path_join("libhostfxr.dylib");
#elif defined(UNIX_ENABLED)
	String probe_path = GodotSharpDirs::get_api_assemblies_dir().path_join("libhostfxr.so");
#else
	String probe_path = "";
#endif

	if (FileAccess::exists(probe_path)) {
		return probe_path;
	}
	return String();
#endif
#endif
}

String find_monosgen() {
#if defined(ANDROID_ENABLED)
	return "libmonosgen-2.0.so";
#else
#if defined(WINDOWS_ENABLED)
	String probe_path = GodotSharpDirs::get_api_assemblies_dir().path_join("monosgen-2.0.dll");
#elif defined(MACOS_ENABLED)
	String probe_path = GodotSharpDirs::get_api_assemblies_dir().path_join("libmonosgen-2.0.dylib");
#elif defined(UNIX_ENABLED)
	String probe_path = GodotSharpDirs::get_api_assemblies_dir().path_join("libmonosgen-2.0.so");
#else
	String probe_path = "";
#endif
	if (FileAccess::exists(probe_path)) {
		return probe_path;
	}
	return String();
#endif
}

String find_coreclr() {
#if defined(ANDROID_ENABLED)
	return "libcoreclr.so";
#else
#if defined(WINDOWS_ENABLED)
	String probe_path = GodotSharpDirs::get_api_assemblies_dir().path_join("coreclr.dll");
#elif defined(MACOS_ENABLED)
	String probe_path = GodotSharpDirs::get_api_assemblies_dir().path_join("libcoreclr.dylib");
#elif defined(UNIX_ENABLED)
	String probe_path = GodotSharpDirs::get_api_assemblies_dir().path_join("libcoreclr.so");
#else
	String probe_path = "";
#endif
	if (FileAccess::exists(probe_path)) {
		return probe_path;
	}
	return String();
#endif
}

bool load_hostfxr(void *&r_hostfxr_dll_handle) {
	String hostfxr_path = find_hostfxr();
	if (hostfxr_path.is_empty()) {
		return false;
	}

	print_verbose("Found hostfxr target: " + hostfxr_path);
	Error err = OS::get_singleton()->open_dynamic_library(hostfxr_path, r_hostfxr_dll_handle);
	if (err != OK) {
		return false;
	}

	void *lib = r_hostfxr_dll_handle;
	void *symbol = nullptr;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "hostfxr_initialize_for_dotnet_command_line", symbol);
	ERR_FAIL_COND_V(err != OK, false);
	hostfxr_initialize_for_dotnet_command_line = (hostfxr_initialize_for_dotnet_command_line_fn)symbol;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "hostfxr_initialize_for_runtime_config", symbol);
	ERR_FAIL_COND_V(err != OK, false);
	hostfxr_initialize_for_runtime_config = (hostfxr_initialize_for_runtime_config_fn)symbol;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "hostfxr_get_runtime_delegate", symbol);
	ERR_FAIL_COND_V(err != OK, false);
	hostfxr_get_runtime_delegate = (hostfxr_get_runtime_delegate_fn)symbol;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "hostfxr_close", symbol);
	ERR_FAIL_COND_V(err != OK, false);
	hostfxr_close = (hostfxr_close_fn)symbol;

	return (hostfxr_initialize_for_runtime_config &&
			hostfxr_get_runtime_delegate &&
			hostfxr_close);
}

bool load_coreclr(void *&r_coreclr_dll_handle) {
	String coreclr_path = find_coreclr();
	bool is_monovm = false;
	if (coreclr_path.is_empty()) {
		coreclr_path = find_monosgen();
		is_monovm = true;
	}

	if (coreclr_path.is_empty()) {
		return false;
	}

	const String coreclr_name = is_monovm ? "monosgen" : "coreclr";
	print_verbose("Found " + coreclr_name + ": " + coreclr_path);

	Error err = OS::get_singleton()->open_dynamic_library(coreclr_path, r_coreclr_dll_handle);
	if (err != OK) {
		return false;
	}

	void *lib = r_coreclr_dll_handle;
	void *symbol = nullptr;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "coreclr_initialize", symbol);
	if (err == OK) {
		coreclr_initialize = (coreclr_initialize_fn)symbol;
	}

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "coreclr_create_delegate", symbol);
	if (err == OK) {
		coreclr_create_delegate = (coreclr_create_delegate_fn)symbol;
	}

#ifdef ANDROID_ENABLED
	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "mono_install_assembly_preload_hook", symbol);
	if (err == OK) mono_install_assembly_preload_hook = (mono_install_assembly_preload_hook_fn)symbol;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "mono_assembly_name_get_name", symbol);
	if (err == OK) mono_assembly_name_get_name = (mono_assembly_name_get_name_fn)symbol;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "mono_assembly_name_get_culture", symbol);
	if (err == OK) mono_assembly_name_get_culture = (mono_assembly_name_get_culture_fn)symbol;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "mono_image_open_from_data_with_name", symbol);
	if (err == OK) mono_image_open_from_data_with_name = (mono_image_open_from_data_with_name_fn)symbol;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "mono_assembly_load_from_full", symbol);
	if (err == OK) mono_assembly_load_from_full = (mono_assembly_load_from_full_fn)symbol;
#endif

	return (coreclr_initialize && coreclr_create_delegate);
}

#ifdef TOOLS_ENABLED
load_assembly_and_get_function_pointer_fn initialize_hostfxr_for_config(const char_t *p_config_path) {
	hostfxr_handle cxt = nullptr;
	int rc = hostfxr_initialize_for_runtime_config(p_config_path, nullptr, &cxt);
	if (rc != 0 || cxt == nullptr) {
		hostfxr_close(cxt);
		ERR_FAIL_V_MSG(nullptr, "hostfxr_initialize_for_runtime_config failed with code: " + itos(rc));
	}

	void *load_assembly_and_get_function_pointer = nullptr;
	rc = hostfxr_get_runtime_delegate(cxt, hdt_load_assembly_and_get_function_pointer, &load_assembly_and_get_function_pointer);
	if (rc != 0 || load_assembly_and_get_function_pointer == nullptr) {
		ERR_FAIL_V_MSG(nullptr, "hostfxr_get_runtime_delegate failed with code: " + itos(rc));
	}

	hostfxr_close(cxt);
	return (load_assembly_and_get_function_pointer_fn)load_assembly_and_get_function_pointer;
}
#else
load_assembly_and_get_function_pointer_fn initialize_hostfxr_self_contained(const char_t *p_main_assembly_path) {
	hostfxr_handle cxt = nullptr;
	List<String> cmdline_args = OS::get_singleton()->get_cmdline_args();
	List<HostFxrCharString> argv_store;
	Vector<const char_t *> argv;
	argv.resize(cmdline_args.size() + 1);

	argv.write[0] = p_main_assembly_path;

	int i = 1;
	for (const String &E : cmdline_args) {
		HostFxrCharString &stored = argv_store.push_back(str_to_hostfxr(E))->get();
		argv.write[i] = get_data(stored);
		i++;
	}

	int rc = hostfxr_initialize_for_dotnet_command_line(argv.size(), argv.ptrw(), nullptr, &cxt);
	if (rc != 0 || cxt == nullptr) {
		hostfxr_close(cxt);
		ERR_FAIL_V_MSG(nullptr, "hostfxr_initialize_for_dotnet_command_line failed with code: " + itos(rc));
	}

	void *load_assembly_and_get_function_pointer = nullptr;
	rc = hostfxr_get_runtime_delegate(cxt, hdt_load_assembly_and_get_function_pointer, &load_assembly_and_get_function_pointer);
	if (rc != 0 || load_assembly_and_get_function_pointer == nullptr) {
		ERR_FAIL_V_MSG(nullptr, "hostfxr_get_runtime_delegate failed with code: " + itos(rc));
	}

	hostfxr_close(cxt);
	return (load_assembly_and_get_function_pointer_fn)load_assembly_and_get_function_pointer;
}
#endif

#ifdef TOOLS_ENABLED
using godot_plugins_initialize_fn = bool (*)(void *, bool, gdmono::PluginCallbacks *, GDMonoCache::ManagedCallbacks *, const void **, int32_t);
#else
using godot_plugins_initialize_fn = bool (*)(void *, GDMonoCache::ManagedCallbacks *, const void **, int32_t);
#endif

#ifdef TOOLS_ENABLED
godot_plugins_initialize_fn initialize_hostfxr_and_godot_plugins(bool &r_runtime_initialized) {
	godot_plugins_initialize_fn godot_plugins_initialize = nullptr;
	String base_assemblies_dir = GodotSharpDirs::get_api_assemblies_dir();

#if defined(ANDROID_ENABLED)
	// 5-Tier Fallback check para sa GodotPlugins.dll
	Vector<String> probe_dirs;
	probe_dirs.push_back("/storage/emulated/0/GEngine/GodotSharp");
	probe_dirs.push_back("/storage/emulated/0/GEngine/GodotSharp/Tools");
	probe_dirs.push_back("/storage/emulated/0/Android/data/org.godotengine.editor.v4/files/GodotSharp");
	probe_dirs.push_back("/storage/emulated/0/libs/GodotSharp");
	probe_dirs.push_back("/storage/emulated/0/libs");
	probe_dirs.push_back("/storage/emulated/0/GodotSharp");
	probe_dirs.push_back("res://GodotSharp");

	for (const String &p : probe_dirs) {
		if (FileAccess::exists(p.path_join("GodotPlugins.dll"))) {
			base_assemblies_dir = p;
			print_verbose(".NET: Found GodotPlugins.dll at: " + base_assemblies_dir);
			break;
		}
	}
#endif

	HostFxrCharString godot_plugins_path = str_to_hostfxr(base_assemblies_dir.path_join("GodotPlugins.dll"));
	HostFxrCharString config_path = str_to_hostfxr(base_assemblies_dir.path_join("GodotPlugins.runtimeconfig.json"));

	load_assembly_and_get_function_pointer_fn load_assembly_and_get_function_pointer =
			initialize_hostfxr_for_config(get_data(config_path));

	if (load_assembly_and_get_function_pointer == nullptr) {
		print_error(".NET: Could not initialize hostfxr with config. Continuing to fallbacks.");
		return nullptr;
	}

	r_runtime_initialized = true;
	print_verbose(".NET: hostfxr initialized");

	int rc = load_assembly_and_get_function_pointer(get_data(godot_plugins_path),
			HOSTFXR_STR("GodotPlugins.Main, GodotPlugins"),
			HOSTFXR_STR("InitializeFromEngine"),
			UNMANAGEDCALLERSONLY_METHOD,
			nullptr,
			(void **)&godot_plugins_initialize);
	ERR_FAIL_COND_V_MSG(rc != 0, nullptr, ".NET: Failed to get GodotPlugins initialization function pointer");

	return godot_plugins_initialize;
}
#else
godot_plugins_initialize_fn initialize_hostfxr_and_godot_plugins(bool &r_runtime_initialized) {
	godot_plugins_initialize_fn godot_plugins_initialize = nullptr;
	String assembly_name = Path::get_csharp_project_name();

	HostFxrCharString assembly_path = str_to_hostfxr(GodotSharpDirs::get_api_assemblies_dir().path_join(assembly_name + ".dll"));
	load_assembly_and_get_function_pointer_fn load_assembly_and_get_function_pointer =
			initialize_hostfxr_self_contained(get_data(assembly_path));
	ERR_FAIL_NULL_V(load_assembly_and_get_function_pointer, nullptr);

	r_runtime_initialized = true;
	print_verbose(".NET: hostfxr initialized");

	int rc = load_assembly_and_get_function_pointer(get_data(assembly_path),
			get_data(str_to_hostfxr("GodotPlugins.Game.Main, " + assembly_name)),
			HOSTFXR_STR("InitializeFromGameProject"),
			UNMANAGEDCALLERSONLY_METHOD,
			nullptr,
			(void **)&godot_plugins_initialize);
	ERR_FAIL_COND_V_MSG(rc != 0, nullptr, ".NET: Failed to get GodotPlugins initialization function pointer");

	return godot_plugins_initialize;
}
#endif

godot_plugins_initialize_fn try_load_native_aot_library(void *&r_aot_dll_handle) {
	String assembly_name = Path::get_csharp_project_name();

#if defined(WINDOWS_ENABLED)
	String native_aot_so_path = GodotSharpDirs::get_api_assemblies_dir().path_join(assembly_name + ".dll");
#elif defined(MACOS_ENABLED) || defined(APPLE_EMBEDDED_ENABLED)
	String native_aot_so_path = GodotSharpDirs::get_api_assemblies_dir().path_join(assembly_name + ".dylib");
#elif defined(ANDROID_ENABLED)
	String native_aot_so_path = "lib" + assembly_name + ".so";
#elif defined(UNIX_ENABLED)
	String native_aot_so_path = GodotSharpDirs::get_api_assemblies_dir().path_join(assembly_name + ".so");
#else
	String native_aot_so_path = "";
#endif

	Error err = OS::get_singleton()->open_dynamic_library(native_aot_so_path, r_aot_dll_handle);
	if (err != OK) {
		return nullptr;
	}

	void *lib = r_aot_dll_handle;
	void *symbol = nullptr;
	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "godotsharp_game_main_init", symbol);
	ERR_FAIL_COND_V(err != OK, nullptr);
	return (godot_plugins_initialize_fn)symbol;
}

#ifdef ANDROID_ENABLED
MonoAssembly *load_assembly_from_pck(MonoAssemblyName *p_assembly_name, char **p_assemblies_path, void *p_user_data) {
	constexpr bool ref_only = false;
	const char *name = mono_assembly_name_get_name(p_assembly_name);
	const char *culture = mono_assembly_name_get_culture(p_assembly_name);

	String assembly_name;
	if (culture && strcmp(culture, "")) {
		assembly_name += culture;
		assembly_name += "/";
	}
	assembly_name += name;
	if (!assembly_name.ends_with(".dll")) {
		assembly_name += ".dll";
	}

	String path = GodotSharpDirs::get_api_assemblies_dir().path_join(assembly_name);
	print_verbose(".NET: Loading assembly '" + assembly_name + "' from '" + path + "'.");

	if (!FileAccess::exists(path)) {
		return nullptr;
	}

	Vector<uint8_t> data = FileAccess::get_file_as_bytes(path);
	ERR_FAIL_COND_V_MSG(data.is_empty(), nullptr, ".NET: Could not read assembly in '" + path + "'.");

	MonoImageOpenStatus status = MONO_IMAGE_OK;
	MonoImage *image = mono_image_open_from_data_with_name(
			reinterpret_cast<char *>(data.ptrw()), data.size(),
			true, &status, ref_only, assembly_name.utf8().get_data());

	ERR_FAIL_COND_V_MSG(status != MONO_IMAGE_OK || image == nullptr, nullptr, ".NET: Failed to open assembly image.");
	status = MONO_IMAGE_OK;
	MonoAssembly *assembly = mono_assembly_load_from_full(image, assembly_name.utf8().get_data(), &status, ref_only);
	ERR_FAIL_COND_V_MSG(status != MONO_IMAGE_OK || assembly == nullptr, nullptr, ".NET: Failed to load assembly from image.");

	return assembly;
}
#endif

godot_plugins_initialize_fn initialize_coreclr_and_godot_plugins(bool &r_runtime_initialized) {
	godot_plugins_initialize_fn godot_plugins_initialize = nullptr;
	String assembly_name = Path::get_csharp_project_name();

#ifdef ANDROID_ENABLED
	if (mono_install_assembly_preload_hook != nullptr) {
		mono_install_assembly_preload_hook(&load_assembly_from_pck, nullptr);
	}
#endif

	void *coreclr_handle = nullptr;
	unsigned int domain_id = 0;
	int rc = coreclr_initialize(nullptr, nullptr, 0, nullptr, nullptr, &coreclr_handle, &domain_id);
	ERR_FAIL_COND_V_MSG(rc != 0, nullptr, ".NET: Failed to initialize CoreCLR.");

	r_runtime_initialized = true;
	print_verbose(".NET: CoreCLR initialized");

	coreclr_create_delegate(coreclr_handle, domain_id,
			assembly_name.utf8().get_data(),
			"GodotPlugins.Game.Main",
			"InitializeFromGameProject",
			(void **)&godot_plugins_initialize);
	ERR_FAIL_NULL_V_MSG(godot_plugins_initialize, nullptr, ".NET: Failed to get GodotPlugins initialization function pointer");

	return godot_plugins_initialize;
}

} // namespace

bool GDMono::should_initialize() {
#ifdef TOOLS_ENABLED
	return true;
#else
	return OS::get_singleton()->has_feature("dotnet");
#endif
}

static bool _on_core_api_assembly_loaded() {
	if (!GDMonoCache::godot_api_cache_updated) {
		return false;
	}
	bool debug = false;
#ifdef DEBUG_ENABLED
	debug = true;
#endif
	GDMonoCache::managed_callbacks.GD_OnCoreApiAssemblyLoaded(debug);
	return true;
}

void GDMono::initialize() {
	print_verbose(".NET: Initializing module on Android/Engine...");

	_init_godot_api_hashes();

	godot_plugins_initialize_fn godot_plugins_initialize = nullptr;

	String assemblies_dir = GodotSharpDirs::get_api_assemblies_dir();

#if defined(ANDROID_ENABLED)
	// 5-Tier Directory Resolution
	if (!DirAccess::exists(assemblies_dir)) {
		Vector<String> paths;
		paths.push_back("/storage/emulated/0/GEngine/GodotSharp/Api");
		paths.push_back("/storage/emulated/0/GEngine/GodotSharp");
		paths.push_back("/storage/emulated/0/Android/data/org.godotengine.editor.v4/files/GodotSharp");
		paths.push_back("/storage/emulated/0/libs/GodotSharp");
		paths.push_back("/storage/emulated/0/libs");
		paths.push_back("res://GodotSharp/Api");

		for (const String &p : paths) {
			if (DirAccess::exists(p)) {
				assemblies_dir = p;
				print_verbose(".NET: Resolved assemblies directory to: " + assemblies_dir);
				break;
			}
		}
	}
#endif

	// 1. Unahin ang HostFXR loader
	if (load_hostfxr(hostfxr_dll_handle)) {
		godot_plugins_initialize = initialize_hostfxr_and_godot_plugins(runtime_initialized);
	}

	// 2. Fallback: Subukan ang CoreCLR
	if (godot_plugins_initialize == nullptr) {
		if (load_coreclr(coreclr_dll_handle)) {
			godot_plugins_initialize = initialize_coreclr_and_godot_plugins(runtime_initialized);
		}
	}

	// 3. Fallback: Native AOT library
	if (godot_plugins_initialize == nullptr) {
		void *dll_handle = nullptr;
		godot_plugins_initialize = try_load_native_aot_library(dll_handle);
		if (godot_plugins_initialize != nullptr) {
			runtime_initialized = true;
		}
	}

	// Non-fatal safety barrier:
	if (godot_plugins_initialize == nullptr) {
		WARN_PRINT(".NET: C# runtime could not be loaded across all 5 tiers. Opening editor without C# runtime active.");
		initialized = true;
		return;
	}

	int32_t interop_funcs_size = 0;
	const void **interop_funcs = godotsharp::get_runtime_interop_funcs(interop_funcs_size);
	GDMonoCache::ManagedCallbacks managed_callbacks{};
	void *godot_dll_handle = nullptr;

#if defined(UNIX_ENABLED) && !defined(MACOS_ENABLED) && !defined(APPLE_EMBEDDED_ENABLED)
	godot_dll_handle = dlopen(nullptr, RTLD_NOW);
#endif

#ifdef TOOLS_ENABLED
	gdmono::PluginCallbacks plugin_callbacks_res;
	bool init_ok = godot_plugins_initialize(godot_dll_handle,
			Engine::get_singleton()->is_editor_hint(),
			&plugin_callbacks_res, &managed_callbacks,
			interop_funcs, interop_funcs_size);
	if (init_ok) {
		plugin_callbacks = plugin_callbacks_res;
	}
#else
	bool init_ok = godot_plugins_initialize(godot_dll_handle, &managed_callbacks,
			interop_funcs, interop_funcs_size);
#endif

	if (init_ok) {
		GDMonoCache::update_godot_api_cache(managed_callbacks);
		print_verbose(".NET: GodotPlugins successfully loaded!");
		_on_core_api_assembly_loaded();
	}

#ifdef TOOLS_ENABLED
	_try_load_project_assembly();
#endif

	initialized = true;
}

#ifdef TOOLS_ENABLED
void GDMono::_try_load_project_assembly() {
	if (Engine::get_singleton()->is_project_manager_hint()) {
		return;
	}
	if (!_load_project_assembly()) {
		if (OS::get_singleton()->is_stdout_verbose()) {
			print_error(".NET: Failed to load project assembly");
		}
	}
}
#endif

void GDMono::_init_godot_api_hashes() {
#ifdef DEBUG_ENABLED
	get_api_core_hash();
#ifdef TOOLS_ENABLED
	get_api_editor_hash();
#endif
#endif
}

#ifdef DEBUG_ENABLED
uint64_t GDMono::get_api_core_hash() {
	if (api_core_hash == 0) {
		api_core_hash = ClassDB::get_api_hash(ClassDB::API_CORE);
	}
	return api_core_hash;
}
#ifdef TOOLS_ENABLED
uint64_t GDMono::get_api_editor_hash() {
	if (api_editor_hash == 0) {
		api_editor_hash = ClassDB::get_api_hash(ClassDB::API_EDITOR);
	}
	return api_editor_hash;
}
#endif
#endif

#ifdef TOOLS_ENABLED
bool GDMono::_load_project_assembly() {
	String assembly_name = Path::get_csharp_project_name();
	String assembly_path = GodotSharpDirs::get_res_temp_assemblies_dir().path_join(assembly_name + ".dll");
	assembly_path = ProjectSettings::get_singleton()->globalize_path(assembly_path);

	if (!FileAccess::exists(assembly_path)) {
		return false;
	}

	String loaded_assembly_path;
	bool success = plugin_callbacks.LoadProjectAssemblyCallback(assembly_path.utf16().get_data(), &loaded_assembly_path);
	if (success) {
		project_assembly_path = loaded_assembly_path.simplify_path();
		project_assembly_modified_time = FileAccess::get_modified_time(loaded_assembly_path);
	}
	return success;
}
#endif

#ifdef GD_MONO_HOT_RELOAD
void GDMono::reload_failure() {
	if (++project_load_failure_count >= (int)GLOBAL_GET("dotnet/project/assembly_reload_attempts")) {
		project_load_failure_count = 0;
		ERR_PRINT_ED(".NET: Giving up on assembly reloading.");

		String assembly_name = Path::get_csharp_project_name();
		String assembly_path = GodotSharpDirs::get_res_temp_assemblies_dir().path_join(assembly_name + ".dll");
		assembly_path = ProjectSettings::get_singleton()->globalize_path(assembly_path);
		project_assembly_path = assembly_path.simplify_path();
		project_assembly_modified_time = FileAccess::get_modified_time(assembly_path);
	}
}

Error GDMono::reload_project_assemblies() {
	ERR_FAIL_COND_V(!runtime_initialized, ERR_BUG);
	finalizing_scripts_domain = true;

	if (!get_plugin_callbacks().UnloadProjectPluginCallback()) {
		reload_failure();
		return FAILED;
	}

	finalizing_scripts_domain = false;

	if (!_load_project_assembly()) {
		reload_failure();
		return ERR_CANT_OPEN;
	}

	if (project_load_failure_count > 0) {
		project_load_failure_count = 0;
		ERR_PRINT_ED(".NET: Assembly reloading succeeded after failures.");
	}

	return OK;
}
#endif

GDMono::GDMono() {
	singleton = this;
}

GDMono::~GDMono() {
	finalizing_scripts_domain = true;

	if (hostfxr_dll_handle) {
		OS::get_singleton()->close_dynamic_library(hostfxr_dll_handle);
	}
	if (coreclr_dll_handle) {
		OS::get_singleton()->close_dynamic_library(coreclr_dll_handle);
	}

	finalizing_scripts_domain = false;
	runtime_initialized = false;
	singleton = nullptr;
}

namespace MonoBind {

GodotSharp *GodotSharp::singleton = nullptr;

void GodotSharp::reload_assemblies() {
#ifdef GD_MONO_HOT_RELOAD
	CRASH_COND(CSharpLanguage::get_singleton() == nullptr);
	if (CSharpLanguage::get_singleton()->is_assembly_reloading_needed()) {
		CSharpLanguage::get_singleton()->reload_assemblies();
	}
#endif
}

GodotSharp::GodotSharp() {
	singleton = this;
}

GodotSharp::~GodotSharp() {
	singleton = nullptr;
}

} // namespace MonoBind
