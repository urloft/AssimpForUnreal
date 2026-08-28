// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#include "AssimpReadGuard.h"

namespace AssimpReadGuard
{
	namespace
	{
		/** Plain arguments, so the guarded frame owns nothing that needs unwinding. */
		struct FReadArgs
		{
			Assimp::Importer* Importer;
			const char* FilePath;
			const void* Data;
			size_t DataSize;
			const char* FormatHint;
			unsigned int Flags;
		};

		/**
		 * The actual Assimp call, in a function with no C++ objects of its own.
		 *
		 * Kept separate from the __try frame because MSVC rejects SEH in any function requiring
		 * object unwinding, and Assimp's own internals certainly do.
		 */
		const aiScene* DoRead(const FReadArgs& Args)
		{
			if (Args.FilePath != nullptr)
			{
				return Args.Importer->ReadFile(Args.FilePath, Args.Flags);
			}

			return Args.Importer->ReadFileFromMemory(
				Args.Data, Args.DataSize, Args.Flags, Args.FormatHint);
		}

#if PLATFORM_WINDOWS
		/**
		 * Decides which structured exceptions to intercept.
		 *
		 * Access violations and misaligned reads are the ones a malformed file provokes, and the ones
		 * it is meaningful to abandon the import over.
		 *
		 * Stack overflow is deliberately excluded. By the time it is raised the guard page is gone,
		 * so continuing is unsound; letting it terminate is the honest outcome. C++ exceptions
		 * (0xE06D7363) are excluded too, so the caller's catch blocks still see them.
		 */
		int32 ShouldHandle(uint32 ExceptionCode)
		{
			switch (ExceptionCode)
			{
			case 0xC0000005: // EXCEPTION_ACCESS_VIOLATION
			case 0xC000001D: // EXCEPTION_ILLEGAL_INSTRUCTION
			case 0xC0000096: // EXCEPTION_PRIV_INSTRUCTION
			case 0xC000008C: // EXCEPTION_ARRAY_BOUNDS_EXCEEDED
			case 0xC0000094: // EXCEPTION_INT_DIVIDE_BY_ZERO
			case 0x80000002: // EXCEPTION_DATATYPE_MISALIGNMENT
				return 1; // EXCEPTION_EXECUTE_HANDLER
			default:
				return 0; // EXCEPTION_CONTINUE_SEARCH
			}
		}
#endif
	}

	const aiScene* Read(
		Assimp::Importer& Importer,
		const char* Utf8FilePath,
		TArrayView<const uint8> Buffer,
		const char* Utf8FormatHint,
		unsigned int Flags,
		bool& bOutHardwareFault)
	{
		bOutHardwareFault = false;

		FReadArgs Args;
		Args.Importer = &Importer;
		Args.FilePath = Utf8FilePath;
		Args.Data = Buffer.GetData();
		Args.DataSize = static_cast<size_t>(Buffer.Num());
		Args.FormatHint = Utf8FormatHint;
		Args.Flags = Flags;

#if PLATFORM_WINDOWS
		__try
		{
			return DoRead(Args);
		}
		__except (ShouldHandle(GetExceptionCode()))
		{
			bOutHardwareFault = true;
			return nullptr;
		}
#else
		// No SEH equivalent on other platforms; a fault there remains fatal.
		return DoRead(Args);
#endif
	}
}
