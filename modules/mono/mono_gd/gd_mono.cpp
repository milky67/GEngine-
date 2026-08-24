/**************************************************************************/
/*                                                             */
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

#if defined(ANDROID_ENABLED) || defined(__ANDROID__)
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

#if defined(ANDROID_ENABLED) || defined(__ANDROID__)
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

#if defined(ANDROID_ENABLED) || defined(__ANDROID__)
String ensure_file_extracted_to_storage(const String &p_res_path, const String &p_filename) {
	String dest_dir = OS::get_singleton()->get_user_data_dir().path_join("GodotSharp_Extracted");
	DirAccess::make_dir_recursive_absolute(dest_dir);

	String dest_path = dest_dir.path_join(p_filename);
	if (FileAccess::exists(dest_path)) {
		return dest_path;
	}

	String src_path = p_res_path.path_join(p_filename);
	if (FileAccess::exists(src_path)) {
		Vector<uint8_t> data = FileAccess::get_file_as_bytes(src_path);
		if (!data.is_empty()) {
			Ref<FileAccess> fa = FileAccess::open(dest_path, FileAccess::WRITE);
			if (fa.is_valid()) {
				fa->store_buffer(data.ptr(), data.size());
				fa->close();
				return dest_path;
			}
		}
	}
	return dest_path;
}
#endif

#ifdef TOOLS_ENABLED
bool try_get_dotnet_root_from_command_line(String &r_dotnet_root) {
#if defined(ANDROID_ENABLED) || defined(__ANDROID__)
	Vector<String> possible_roots;
	possible_roots.push_back(OS::get_singleton()->get_user_data_dir().path_join("dotnet"));
	possible_roots.push_back("/storage/emulated/0/GEngine/dotnet");
	possible_roots.push_back("/storage/emulated/0/GEngine/GodotSharp/dotnet");

	for (int i = 0; i < possible_roots.size(); i++) {
		if (DirAccess::exists(possible_roots[i])) {
			r_dotnet_root = possible_roots[i];
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
			continue;
		}

		if (!DirAccess::exists(path)) {
			continue;
		}

		if (version > latest_sdk_version) {
			latest_sdk_version = version;
			latest_sdk_path = path;
		}
	}

	if (!latest_sdk_path.is_empty()) {
		r_dotnet_root = latest_sdk_path.path_join("..").simplify_path();
		return true;
	}

	return false;
#endif
}
#endif

String find_hostfxr() {
#if defined(ANDROID_ENABLED) || defined(__ANDROID__)
	void *test_handle = nullptr;
	if (OS::get_singleton()->open_dynamic_library("libhostfxr.so", test_handle) == OK) {
		OS::get_singleton()->close_dynamic_library(test_handle);
		return "libhostfxr.so";
	}

	Vector<String> paths;
	paths.push_back(OS::get_singleton()->get_user_data_dir().path_join("GodotSharp/libhostfxr.so"));
	paths.push_back("/storage/emulated/0/GEngine/GodotSharp/libhostfxr.so");
	paths.push_back(GodotSharpDirs::get_api_assemblies_dir().path_join("libhostfxr.so"));

	for (int i = 0; i < paths.size(); i++) {
		if (FileAccess::exists(paths[i])) {
			return paths[i];
		}
	}
	return String();
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
#if defined(ANDROID_ENABLED) || defined(__ANDROID__)
	void *test_handle = nullptr;
	if (OS::get_singleton()->open_dynamic_library("libmonosgen-2.0.so", test_handle) == OK) {
		OS::get_singleton()->close_dynamic_library(test_handle);
		return "libmonosgen-2.0.so";
	}
	if (OS::get_singleton()->open_dynamic_library("libmonosgen.so", test_handle) == OK) {
		OS::get_singleton()->close_dynamic_library(test_handle);
		return "libmonosgen.so";
	}
	return String();
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
#if defined(ANDROID_ENABLED) || defined(__ANDROID__)
	void *test_handle = nullptr;
	if (OS::get_singleton()->open_dynamic_library("libcoreclr.so", test_handle) == OK) {
		OS::get_singleton()->close_dynamic_library(test_handle);
		return "libcoreclr.so";
	}
	return String();
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
#if defined(ANDROID_ENABLED) || defined(__ANDROID__)
	void *dl_handle = nullptr;
	OS::get_singleton()->open_dynamic_library("libdl.so", dl_handle);
	void *cxx_handle = nullptr;
	OS::get_singleton()->open_dynamic_library("libc++_shared.so", cxx_handle);
#endif

	String hostfxr_path = find_hostfxr();
	if (hostfxr_path.is_empty()) {
		return false;
	}

	Error err = OS::get_singleton()->open_dynamic_library(hostfxr_path, r_hostfxr_dll_handle);
	if (err != OK || r_hostfxr_dll_handle == nullptr) {
		return false;
	}

	void *lib = r_hostfxr_dll_handle;
	void *symbol = nullptr;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "hostfxr_initialize_for_dotnet_command_line", symbol);
	if (err != OK) return false;
	hostfxr_initialize_for_dotnet_command_line = (hostfxr_initialize_for_dotnet_command_line_fn)symbol;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "hostfxr_initialize_for_runtime_config", symbol);
	if (err != OK) return false;
	hostfxr_initialize_for_runtime_config = (hostfxr_initialize_for_runtime_config_fn)symbol;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "hostfxr_get_runtime_delegate", symbol);
	if (err != OK) return false;
	hostfxr_get_runtime_delegate = (hostfxr_get_runtime_delegate_fn)symbol;

	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "hostfxr_close", symbol);
	if (err != OK) return false;
	hostfxr_close = (hostfxr_close_fn)symbol;

	return (hostfxr_initialize_for_runtime_config &&
			hostfxr_get_runtime_delegate &&
			hostfxr_close);
}

bool load_coreclr(void *&r_coreclr_dll_handle) {
#if defined(ANDROID_ENABLED) || defined(__ANDROID__)
	void *sys_dl_handle = nullptr;
	OS::get_singleton()->open_dynamic_library("libdl.so", sys_dl_handle);
	void *sys_cpp_handle = nullptr;
	OS::get_singleton()->open_dynamic_library("libc++_shared.so", sys_cpp_handle);
#endif

	String coreclr_path = find_coreclr();
	bool is_monovm = false;

	if (coreclr_path.is_empty()) {
		coreclr_path = find_monosgen();
		is_monovm = true;
	}

	if (coreclr_path.is_empty()) {
		return false;
	}

	Error err = OS::get_singleton()->open_dynamic_library(coreclr_path, r_coreclr_dll_handle);
	if (err != OK || r_coreclr_dll_handle == nullptr) {
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

#if defined(ANDROID_ENABLED) || defined(__ANDROID__)
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

	return (coreclr_initialize != nullptr && coreclr_create_delegate != nullptr) || (is_monovm);
}

#ifdef TOOLS_ENABLED
load_assembly_and_get_function_pointer_fn initialize_hostfxr_for_config(const char_t *p_config_path) {
	hostfxr_handle cxt = nullptr;
	int rc = hostfxr_initialize_for_runtime_config(p_config_path, nullptr, &cxt);
	if (rc != 0 || cxt == nullptr) {
		if (cxt != nullptr) hostfxr_close(cxt);
		return nullptr;
	}

	void *load_assembly_and_get_function_pointer = nullptr;
	rc = hostfxr_get_runtime_delegate(cxt, hdt_load_assembly_and_get_function_pointer, &load_assembly_and_get_function_pointer);
	if (rc != 0 || load_assembly_and_get_function_pointer == nullptr) {
		hostfxr_close(cxt);
		return nullptr;
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
		if (cxt != nullptr) hostfxr_close(cxt);
		return nullptr;
	}

	void *load_assembly_and_get_function_pointer = nullptr;
	rc = hostfxr_get_runtime_delegate(cxt, hdt_load_assembly_and_get_function_pointer, &load_assembly_and_get_function_pointer);
	if (rc != 0 || load_assembly_and_get_function_pointer == nullptr) {
		hostfxr_close(cxt);
		return nullptr;
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

#if defined(ANDROID_ENABLED) || defined(__ANDROID__)
	String extracted_dll = ensure_file_extracted_to_storage(base_assemblies_dir, "GodotPlugins.dll");
	String extracted_json = ensure_file_extracted_to_storage(base_assemblies_dir, "GodotPlugins.runtimeconfig.json");
	HostFxrCharString godot_plugins_path = str_to_hostfxr(extracted_dll);
	HostFxrCharString config_path = str_to_hostfxr(extracted_json);
#else
	HostFxrCharString godot_plugins_path = str_to_hostfxr(base_assemblies_dir.path_join("GodotPlugins.dll"));
	HostFxrCharString config_path = str_to_hostfxr(base_assemblies_dir.path_join("GodotPlugins.runtimeconfig.json"));
#endif

	load_assembly_and_get_function_pointer_fn load_assembly_and_get_function_pointer =
			initialize_hostfxr_for_config(get_data(config_path));

	if (load_assembly_and_get_function_pointer == nullptr) {
		return nullptr;
	}

	r_runtime_initialized = true;

	int rc = load_assembly_and_get_function_pointer(get_data(godot_plugins_path),
			HOSTFXR_STR("GodotPlugins.Main, GodotPlugins"),
			HOSTFXR_STR("InitializeFromEngine"),
			UNMANAGEDCALLERSONLY_METHOD,
			nullptr,
			(void **)&godot_plugins_initialize);

	if (rc != 0) return nullptr;
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

	int rc = load_assembly_and_get_function_pointer(get_data(assembly_path),
			get_data(str_to_hostfxr("GodotPlugins.Game.Main, " + assembly_name)),
			HOSTFXR_STR("InitializeFromGameProject"),
			UNMANAGEDCALLERSONLY_METHOD,
			nullptr,
			(void **)&godot_plugins_initialize);

	if (rc != 0) return nullptr;
	return godot_plugins_initialize;
}
#endif

godot_plugins_initialize_fn try_load_native_aot_library(void *&r_aot_dll_handle) {
	String assembly_name = Path::get_csharp_project_name();

#if defined(WINDOWS_ENABLED)
	String native_aot_so_path = GodotSharpDirs::get_api_assemblies_dir().path_join(assembly_name + ".dll");
#elif defined(MACOS_ENABLED) || defined(APPLE_EMBEDDED_ENABLED)
	String native_aot_so_path = GodotSharpDirs::get_api_assemblies_dir().path_join(assembly_name + ".dylib");
#elif defined(ANDROID_ENABLED) || defined(__ANDROID__)
	String native_aot_so_path = "lib" + assembly_name + ".so";
#elif defined(UNIX_ENABLED)
	String native_aot_so_path = GodotSharpDirs::get_api_assemblies_dir().path_join(assembly_name + ".so");
#else
	String native_aot_so_path = "";
#endif

	Error err = OS::get_singleton()->open_dynamic_library(native_aot_so_path, r_aot_dll_handle);
	if (err != OK) return nullptr;

	void *lib = r_aot_dll_handle;
	void *symbol = nullptr;
	err = OS::get_singleton()->get_dynamic_library_symbol_handle(lib, "godotsharp_game_main_init", symbol);
	ERR_FAIL_COND_V(err != OK, nullptr);

	return (godot_plugins_initialize_fn)symbol;
}

#if defined(ANDROID_ENABLED) || defined(__ANDROID__)
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
	if (!FileAccess::exists(path)) {
		return nullptr;
	}

	Vector<uint8_t> data = FileAccess::get_file_as_bytes(path);
	if (data.is_empty()) return nullptr;

	MonoImageOpenStatus status = MONO_IMAGE_OK;
	MonoImage *image = mono_image_open_from_data_with_name(
			reinterpret_cast<char *>(data.ptrw()), data.size(),
			true, &status, ref_only, assembly_name.utf8().get_data());

	if (status != MONO_IMAGE_OK || image == nullptr) return nullptr;

	status = MONO_IMAGE_OK;
	MonoAssembly *assembly = mono_assembly_load_from_full(
			image, assembly_name.utf8().get_data(),
			&status,
			ref_only);

	return assembly;
}
#endif

godot_plugins_initialize_fn initialize_coreclr_and_godot_plugins(bool &r_runtime_initialized) {
	godot_plugins_initialize_fn godot_plugins_initialize = nullptr;
	String assembly_name = Path::get_csharp_project_name();

#if defined(ANDROID_ENABLED) || defined(__ANDROID__)
	if (mono_install_assembly_preload_hook != nullptr) {
		mono_install_assembly_preload_hook(&load_assembly_from_pck, nullptr);
	}
#endif

	if (coreclr_initialize == nullptr || coreclr_create_delegate == nullptr) {
		return nullptr;
	}

	void *coreclr_handle = nullptr;
	unsigned int domain_id = 0;
	int rc = coreclr_initialize(nullptr, nullptr, 0, nullptr, nullptr, &coreclr_handle, &domain_id);
	if (rc != 0) return nullptr;

	r_runtime_initialized = true;

	coreclr_create_delegate(coreclr_handle, domain_id,
			assembly_name.utf8().get_data(),
			"GodotPlugins.Game.Main",
			"InitializeFromGameProject",
			(void **)&godot_plugins_initialize);

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

	if (GDMonoCache::managed_callbacks.GD_OnCoreApiAssemblyLoaded != nullptr) {
		GDMonoCache::managed_callbacks.GD_OnCoreApiAssemblyLoaded(debug);
		return true;
	}
	return false;
}

void GDMono::initialize() {
	if (runtime_initialized || initialized) {
		return;
	}

	print_verbose(".NET: Initializing module for Android/GEngine...");
	_init_godot_api_hashes();

#if defined(ANDROID_ENABLED) || defined(__ANDROID__)
	String user_dir = OS::get_singleton()->get_user_data_dir();
	setenv("TMPDIR", user_dir.utf8().get_data(), 1);
	setenv("HOME", user_dir.utf8().get_data(), 1);
	setenv("DOTNET_SYSTEM_GLOBALIZATION_INVARIANT", "1", 1);
#endif

	godot_plugins_initialize_fn godot_plugins_initialize = nullptr;

#if defined(ANDROID_ENABLED) || defined(__ANDROID__)
	// SA ANDROID: Mono SGen VM ang unang priority (Ligtas laban sa glibc crash)
	if (load_coreclr(coreclr_dll_handle)) {
		godot_plugins_initialize = initialize_coreclr_and_godot_plugins(runtime_initialized);
	}

	if (godot_plugins_initialize == nullptr) {
		if (load_hostfxr(hostfxr_dll_handle)) {
			godot_plugins_initialize = initialize_hostfxr_and_godot_plugins(runtime_initialized);
		}
	}
#else
	// SA DESKTOP: HostFXR ang standard priority
	if (load_hostfxr(hostfxr_dll_handle)) {
		godot_plugins_initialize = initialize_hostfxr_and_godot_plugins(runtime_initialized);
	}

	if (godot_plugins_initialize == nullptr) {
		if (load_coreclr(coreclr_dll_handle)) {
			godot_plugins_initialize = initialize_coreclr_and_godot_plugins(runtime_initialized);
		}
	}
#endif

	if (godot_plugins_initialize == nullptr) {
		void *dll_handle = nullptr;
		godot_plugins_initialize = try_load_native_aot_library(dll_handle);
		if (godot_plugins_initialize != nullptr) {
			runtime_initialized = true;
		}
	}

	if (godot_plugins_initialize == nullptr) {
		print_line(".NET: Operating in safe standalone mode (No crash).");
		initialized = true;
		return;
	}

	int32_t interop_funcs_size = 0;
	const void **interop_funcs = godotsharp::get_runtime_interop_funcs(interop_funcs_size);

	GDMonoCache::ManagedCallbacks managed_callbacks{};

	void *godot_dll_handle = nullptr;
#if defined(ANDROID_ENABLED) || defined(__ANDROID__)
	godot_dll_handle = dlopen("libgodot_android.so", RTLD_NOW);
	if (!godot_dll_handle) {
		godot_dll_handle = dlopen(nullptr, RTLD_NOW);
	}
#elif defined(UNIX_ENABLED) && !defined(MACOS_ENABLED) && !defined(APPLE_EMBEDDED_ENABLED)
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

	String temp_dir = GodotSharpDirs::get_res_temp_assemblies_dir();
	temp_dir = ProjectSettings::get_singleton()->globalize_path(temp_dir);

	if (!DirAccess::exists(temp_dir)) {
		DirAccess::make_dir_recursive_absolute(temp_dir);
	}

	_load_project_assembly();
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
	if (plugin_callbacks.LoadProjectAssemblyCallback == nullptr) {
		return false;
	}

	String assembly_name = Path::get_csharp_project_name();
	if (assembly_name.is_empty()) {
		return false;
	}

	String assembly_path = GodotSharpDirs::get_res_temp_assemblies_dir().path_join(assembly_name + ".dll");
	assembly_path = ProjectSettings::get_singleton()->globalize_path(assembly_path);

	if (!FileAccess::exists(assembly_path)) {
		return false;
	}

	String loaded_assembly_path;
	bool success = false;

	if (plugin_callbacks.LoadProjectAssemblyCallback != nullptr) {
		success = plugin_callbacks.LoadProjectAssemblyCallback(assembly_path.utf16().get_data(), &loaded_assembly_path);
	}

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

	if (get_plugin_callbacks().UnloadProjectPluginCallback != nullptr) {
		if (!get_plugin_callbacks().UnloadProjectPluginCallback()) {
			reload_failure();
			return FAILED;
		}
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
		hostfxr_dll_handle = nullptr;
	}
	if (coreclr_dll_handle) {
		OS::get_singleton()->close_dynamic_library(coreclr_dll_handle);
		coreclr_dll_handle = nullptr;
	}

	finalizing_scripts_domain = false;
	runtime_initialized = false;
	singleton = nullptr;
}

namespace MonoBind {

GodotSharp *GodotSharp::singleton = nullptr;

void GodotSharp::reload_assemblies() {
#ifdef GD_MONO_HOT_RELOAD
	if (CSharpLanguage::get_singleton() != nullptr && CSharpLanguage::get_singleton()->is_assembly_reloading_needed()) {
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
