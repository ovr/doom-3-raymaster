/*
===============================================================================

	A second copy of the LCP solvers compiled without the SSE code paths,
	so the tests can compare the SIMD implementation with the plain C++ one.
	The classes are renamed to idLCP_Generic, idLCP_Square_Generic and
	idLCP_Symmetric_Generic.

	NOTE: idlib/sys/sys_defines.h does not define ID_WIN_X86_SSE_INTRIN or
	ID_WIN_X86_SSE2_INTRIN, so the engine itself already runs the plain C++
	paths and both copies are the same code. The comparison becomes useful
	again when the SSE paths are enabled.

===============================================================================
*/

#pragma hdrstop
#include "../idlib/precompiled.h"

#undef ID_WIN_X86_SSE_INTRIN
#undef ID_WIN_X86_SSE2_INTRIN

#undef __MATH_LCP_H__
#define idLCP				idLCP_Generic
#define idLCP_Square		idLCP_Square_Generic
#define idLCP_Symmetric		idLCP_Symmetric_Generic
#define lcp_showFailures	lcp_showFailures_generic

#include "../idlib/math/Lcp.h"
#include "../idlib/math/Lcp.cpp"
