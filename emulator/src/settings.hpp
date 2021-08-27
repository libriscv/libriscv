#pragma once

#if defined(EMULATOR_MODE_LINUX)
	static constexpr bool full_linux_guest = true;
#else
	static constexpr bool full_linux_guest = false;
#endif
#if defined(EMULATOR_MODE_NEWLIB)
	static constexpr bool newlib_mini_guest = true;
#else
	static constexpr bool newlib_mini_guest = false;
#endif
#if defined(EMULATOR_MODE_MICRO)
	static constexpr bool micro_guest = true;
#else
	static constexpr bool micro_guest = false;
#endif
