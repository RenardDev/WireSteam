
// C++
#include <cstdio>
#include <exception>

// Project
#include "platform.hpp"

int wmain(int nArgumentCount, wchar_t** ppszArguments) {
	try {
		return ws::ServiceHelperMain(nArgumentCount, ppszArguments);
	} catch (std::exception const& Exception) {
		// Best effort at the process boundary; failure cannot be reported elsewhere.
		std::fprintf(stderr, "WireSteam service helper: %s\n", Exception.what());
		return 1;
	} catch (...) {
		std::fputs("WireSteam service helper: unexpected exception\n", stderr);
		return 1;
	}
}
