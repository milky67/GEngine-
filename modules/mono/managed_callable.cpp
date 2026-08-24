/**************************************************************************/
/*  managed_callable.cpp                                                  */
/**************************************************************************/

#include "managed_callable.h"

#include "csharp_script.h"
#include "mono_gd/gd_mono_cache.h"

#ifdef GD_MONO_HOT_RELOAD
SelfList<ManagedCallable>::List ManagedCallable::instances;
RBMap<ManagedCallable *, Array> ManagedCallable::instances_pending_reload;
Mutex ManagedCallable::instances_mutex;
#endif

bool ManagedCallable::compare_equal(const CallableCustom *p_a, const CallableCustom *p_b) {
	if (p_a == p_b) {
		return true;
	}
	if (!p_a || !p_b) {
		return false;
	}

	const ManagedCallable *a = static_cast<const ManagedCallable *>(p_a);
	const ManagedCallable *b = static_cast<const ManagedCallable *>(p_b);

	if (a->delegate_handle.value == b->delegate_handle.value && a->trampoline == b->trampoline && a->object_id == b->object_id) {
		return true;
	}
	if (!a->delegate_handle.value || !b->delegate_handle.value) {
		return false;
	}

	if (!GDMonoCache::godot_api_cache_updated || GDMonoCache::managed_callbacks.DelegateUtils_DelegateEquals == nullptr) {
		return a->delegate_handle.value == b->delegate_handle.value;
	}

	return GDMonoCache::managed_callbacks.DelegateUtils_DelegateEquals(
			a->delegate_handle, b->delegate_handle);
}

bool ManagedCallable::compare_less(const CallableCustom *p_a, const CallableCustom *p_b) {
	if (!p_a || !p_b) {
		return p_a < p_b;
	}
	if (compare_equal(p_a, p_b)) {
		return false;
	}
	return p_a < p_b;
}

uint32_t ManagedCallable::hash() const {
	if (delegate_handle.value == nullptr) {
		return 0;
	}

	if (!GDMonoCache::godot_api_cache_updated || GDMonoCache::managed_callbacks.DelegateUtils_DelegateHash == nullptr) {
		return (uint32_t)(uint64_t)delegate_handle.value;
	}
	return GDMonoCache::managed_callbacks.DelegateUtils_DelegateHash(delegate_handle);
}

String ManagedCallable::get_as_text() const {
	return "Delegate::Invoke";
}

CallableCustom::CompareEqualFunc ManagedCallable::get_compare_equal_func() const {
	return compare_equal_func_ptr;
}

CallableCustom::CompareLessFunc ManagedCallable::get_compare_less_func() const {
	return compare_less_func_ptr;
}

ObjectID ManagedCallable::get_object() const {
	if (object_id != ObjectID()) {
		return object_id;
	}
	if (CSharpLanguage::get_singleton() && CSharpLanguage::get_singleton()->get_managed_callable_middleman()) {
		return CSharpLanguage::get_singleton()->get_managed_callable_middleman()->get_instance_id();
	}
	return ObjectID();
}

int ManagedCallable::get_argument_count(bool &r_is_valid) const {
	if (!GDMonoCache::godot_api_cache_updated || GDMonoCache::managed_callbacks.DelegateUtils_GetArgumentCount == nullptr || delegate_handle.value == nullptr) {
		r_is_valid = false;
		return 0;
	}
	return GDMonoCache::managed_callbacks.DelegateUtils_GetArgumentCount(delegate_handle, &r_is_valid);
}

void ManagedCallable::call(const Variant **p_arguments, int p_argcount, Variant &r_return_value, Callable::CallError &r_call_error) const {
	r_call_error.error = Callable::CallError::CALL_ERROR_INVALID_METHOD;
	r_return_value = Variant();

	if (delegate_handle.value == nullptr) {
		r_call_error.error = Callable::CallError::CALL_ERROR_INSTANCE_IS_NULL;
		return;
	}

	if (!GDMonoCache::godot_api_cache_updated || GDMonoCache::managed_callbacks.DelegateUtils_InvokeWithVariantArgs == nullptr) {
		r_call_error.error = Callable::CallError::CALL_ERROR_INSTANCE_IS_NULL;
		return;
	}

	GDMonoCache::managed_callbacks.DelegateUtils_InvokeWithVariantArgs(
			delegate_handle, trampoline, p_arguments, p_argcount, &r_return_value);

	r_call_error.error = Callable::CallError::CALL_OK;
}

void ManagedCallable::release_delegate_handle() {
	if (delegate_handle.value != nullptr) {
		if (GDMonoCache::godot_api_cache_updated && GDMonoCache::managed_callbacks.GCHandleBridge_FreeGCHandle != nullptr) {
			GDMonoCache::managed_callbacks.GCHandleBridge_FreeGCHandle(delegate_handle);
		}
		delegate_handle = { nullptr };
	}
}

/* clang-format off */
ManagedCallable::ManagedCallable(GCHandleIntPtr p_delegate_handle, void *p_trampoline, ObjectID p_object_id) :
		delegate_handle(p_delegate_handle), trampoline(p_trampoline), object_id(p_object_id) {
#ifdef GD_MONO_HOT_RELOAD
	{
		MutexLock lock(instances_mutex);
		instances.add(&self_instance);
	}
#endif
}
/* clang-format on */

ManagedCallable::~ManagedCallable() {
#ifdef GD_MONO_HOT_RELOAD
	{
		MutexLock lock(instances_mutex);
		instances.remove(&self_instance);
		instances_pending_reload.erase(this);
	}
#endif

	release_delegate_handle();
}
