#include "Guard.h"

#include <Windows.h>

#include <filesystem>
#include <format>

namespace Game::Guard
{
	namespace
	{
		constexpr DWORD kCxxException = 0xE06D7363;  // MSVC C++ throw: let C++ handlers deal with it

		int Filter(EXCEPTION_POINTERS* a_ep, Fault* a_fault)
		{
			const DWORD code = a_ep->ExceptionRecord->ExceptionCode;
			if (code == kCxxException) {
				return EXCEPTION_CONTINUE_SEARCH;
			}
			a_fault->code = code;
			a_fault->address = a_ep->ExceptionRecord->ExceptionAddress;
			return EXCEPTION_EXECUTE_HANDLER;
		}
	}

	// No C++ objects with destructors may live in this frame (C2712); keep it that way.
	bool Run(void (*a_fn)(void*), void* a_ctx, Fault& a_fault)
	{
		__try {
			a_fn(a_ctx);
			return true;
		} __except (Filter(GetExceptionInformation(), &a_fault)) {
			return false;
		}
	}

	std::string Describe(const void* a_address)
	{
		HMODULE module = nullptr;
		if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
				static_cast<LPCWSTR>(a_address), &module) && module) {
			wchar_t path[MAX_PATH]{};
			GetModuleFileNameW(module, path, MAX_PATH);
			const auto offset = reinterpret_cast<std::uintptr_t>(a_address) - reinterpret_cast<std::uintptr_t>(module);
			return std::format("{}+{:07X}", std::filesystem::path{ path }.filename().string(), offset);
		}
		return std::format("{:#x}", reinterpret_cast<std::uintptr_t>(a_address));
	}
}
