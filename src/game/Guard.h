#pragma once

#include <cstdint>
#include <string>

// Structured-exception guard for read-only perception requests. An agent probing an unusual
// object should get an error, not crash the player's game. Faults are logged with module+offset
// so they still get reported and fixed. Not used for actions: resuming after a fault mid-action
// could leave engine state half-changed.
namespace Game::Guard
{
	struct Fault
	{
		std::uint32_t code{ 0 };
		void*         address{ nullptr };
	};

	// Runs a_fn(a_ctx). Returns false (with the fault filled in) if a structured exception such
	// as an access violation escaped it. C++ exceptions are not intercepted.
	bool Run(void (*a_fn)(void*), void* a_ctx, Fault& a_fault);

	// "SkyrimSE.exe+03D3E15" / "Animunculi.dll+00649D3"
	std::string Describe(const void* a_address);
}
