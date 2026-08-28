// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#include "AssimpBridges.h"

#include "AssimpCore.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"

// =================================================================================================
// FAssimpArchiveStream
// =================================================================================================

FAssimpArchiveStream::FAssimpArchiveStream(TUniquePtr<FArchive> InArchive)
	: Archive(MoveTemp(InArchive))
{
	if (Archive.IsValid())
	{
		TotalSize = Archive->TotalSize();
	}
}

FAssimpArchiveStream::~FAssimpArchiveStream()
{
	if (Archive.IsValid())
	{
		Archive->Close();
	}
}

size_t FAssimpArchiveStream::Read(void* Buffer, size_t Size, size_t Count)
{
	if (!Archive.IsValid() || Buffer == nullptr || Size == 0 || Count == 0)
	{
		return 0;
	}

	// Clamp to what is actually left. FArchive::Serialize past the end sets an error flag rather
	// than short-reading, and Assimp's parsers rely on a short read to detect end-of-file.
	const int64 Remaining = TotalSize - Archive->Tell();
	if (Remaining <= 0)
	{
		return 0;
	}

	const int64 Requested = static_cast<int64>(Size) * static_cast<int64>(Count);
	const int64 Readable = FMath::Min(Requested, Remaining);

	// Assimp counts in whole elements, so never hand back a partial element.
	const int64 ElementsToRead = Readable / static_cast<int64>(Size);
	if (ElementsToRead <= 0)
	{
		return 0;
	}

	Archive->Serialize(Buffer, ElementsToRead * static_cast<int64>(Size));

	return static_cast<size_t>(ElementsToRead);
}

size_t FAssimpArchiveStream::Write(const void* /*Buffer*/, size_t /*Size*/, size_t /*Count*/)
{
	// Import only. Assimp is built with ASSIMP_NO_EXPORT, so nothing should ever reach this.
	return 0;
}

aiReturn FAssimpArchiveStream::Seek(size_t Offset, aiOrigin Origin)
{
	if (!Archive.IsValid())
	{
		return aiReturn_FAILURE;
	}

	int64 Target = 0;
	switch (Origin)
	{
	case aiOrigin_SET: Target = static_cast<int64>(Offset);                 break;
	case aiOrigin_CUR: Target = Archive->Tell() + static_cast<int64>(Offset); break;
	case aiOrigin_END: Target = TotalSize - static_cast<int64>(Offset);     break;
	default:           return aiReturn_FAILURE;
	}

	if (Target < 0 || Target > TotalSize)
	{
		return aiReturn_FAILURE;
	}

	Archive->Seek(Target);
	return aiReturn_SUCCESS;
}

size_t FAssimpArchiveStream::Tell() const
{
	return Archive.IsValid() ? static_cast<size_t>(Archive->Tell()) : 0;
}

size_t FAssimpArchiveStream::FileSize() const
{
	return static_cast<size_t>(TotalSize);
}

void FAssimpArchiveStream::Flush()
{
	if (Archive.IsValid())
	{
		Archive->Flush();
	}
}

// =================================================================================================
// FAssimpFileManagerIO
// =================================================================================================

FAssimpFileManagerIO::FAssimpFileManagerIO(const FString& InBaseDirectory)
	: BaseDirectory(InBaseDirectory)
{
}

FAssimpFileManagerIO::~FAssimpFileManagerIO() = default;

FString FAssimpFileManagerIO::ResolvePath(const char* File) const
{
	if (File == nullptr)
	{
		return FString();
	}

	FString Requested(UTF8_TO_TCHAR(File));

	// Source files routinely record Windows separators regardless of platform.
	Requested.ReplaceInline(TEXT("\\"), TEXT("/"));

	// 1. As given. Covers absolute paths that still resolve, and paths already relative to the CWD.
	if (IFileManager::Get().FileExists(*Requested))
	{
		return Requested;
	}

	if (!BaseDirectory.IsEmpty())
	{
		// 2. Relative to the source file's directory. The common case for sibling .mtl and textures.
		const FString Relative = FPaths::Combine(BaseDirectory, Requested);
		if (IFileManager::Get().FileExists(*Relative))
		{
			return Relative;
		}

		// 3. Bare filename in the source directory. This is what rescues a model recording an
		//    absolute path from the machine it was authored on -- the directory no longer exists,
		//    but the texture usually sits next to the model.
		const FString CleanName = FPaths::GetCleanFilename(Requested);
		if (!CleanName.IsEmpty() && CleanName != Requested)
		{
			const FString Sibling = FPaths::Combine(BaseDirectory, CleanName);
			if (IFileManager::Get().FileExists(*Sibling))
			{
				return Sibling;
			}
		}
	}

	// Nothing matched. Return the normalised request so the caller's Exists/Open reports the path
	// that was actually looked for.
	return Requested;
}

bool FAssimpFileManagerIO::Exists(const char* File) const
{
	const FString Resolved = ResolvePath(File);
	return !Resolved.IsEmpty() && IFileManager::Get().FileExists(*Resolved);
}

char FAssimpFileManagerIO::getOsSeparator() const
{
	// ResolvePath normalises everything to forward slashes, which every supported platform accepts.
	return '/';
}

Assimp::IOStream* FAssimpFileManagerIO::Open(const char* File, const char* Mode)
{
	// Import only: reject write modes rather than silently returning a stream that discards writes.
	if (Mode != nullptr && (FCStringAnsi::Strchr(Mode, 'w') != nullptr || FCStringAnsi::Strchr(Mode, 'a') != nullptr))
	{
		UE_LOG(LogAssimp, Warning,
			TEXT("Assimp requested write access to '%s'; refused (import-only file system)."),
			*FString(UTF8_TO_TCHAR(File)));
		return nullptr;
	}

	const FString Resolved = ResolvePath(File);
	if (Resolved.IsEmpty())
	{
		return nullptr;
	}

	TUniquePtr<FArchive> Archive(IFileManager::Get().CreateFileReader(*Resolved));
	if (!Archive.IsValid())
	{
		UE_LOG(LogAssimp, Verbose, TEXT("Could not open '%s' for reading."), *Resolved);
		return nullptr;
	}

	UE_LOG(LogAssimp, VeryVerbose, TEXT("Opened '%s' (%lld bytes)."), *Resolved, Archive->TotalSize());

	return new FAssimpArchiveStream(MoveTemp(Archive));
}

void FAssimpFileManagerIO::Close(Assimp::IOStream* File)
{
	delete File;
}

// =================================================================================================
// FAssimpProgressBridge
// =================================================================================================

FAssimpProgressBridge::FAssimpProgressBridge(const FAssimpProgressDelegate& InDelegate)
	: Delegate(InDelegate)
{
}

FAssimpProgressBridge::~FAssimpProgressBridge() = default;

bool FAssimpProgressBridge::Update(float Percentage)
{
	if (!Delegate.IsBound())
	{
		return true;
	}

	// Assimp is loose about this value: some importers report -1 when they cannot estimate progress,
	// and a few overshoot 1.0. Clamp so callers can treat it as a well-formed fraction.
	const float Fraction = FMath::IsFinite(Percentage) ? FMath::Clamp(Percentage, 0.0f, 1.0f) : 0.0f;

	const bool bContinue = Delegate.Execute(Fraction);
	if (!bContinue)
	{
		bCancelled = true;
	}

	return bContinue;
}

// =================================================================================================
// FAssimpLogBridge
// =================================================================================================

namespace
{
	/**
	 * Diagnostics array capturing Assimp output on this thread, or null.
	 *
	 * Thread-local because Assimp's logger is process-wide but imports may run concurrently on
	 * several threads; a shared sink would interleave unrelated imports' diagnostics.
	 */
	thread_local TArray<FAssimpDiagnostic>* GActiveDiagnosticSink = nullptr;

	/** True once the log streams have been attached to Assimp's global logger. */
	bool GLogBridgeInstalled = false;

	/** Forwards one Assimp severity into Unreal. */
	class FAssimpSeverityStream final : public Assimp::LogStream
	{
	public:
		explicit FAssimpSeverityStream(EAssimpDiagnosticSeverity InSeverity)
			: Severity(InSeverity)
		{
		}

		virtual void write(const char* Message) override
		{
			if (Message == nullptr)
			{
				return;
			}

			FString Text(UTF8_TO_TCHAR(Message));

			// Assimp terminates its messages with a newline; Unreal's log adds its own.
			Text.TrimEndInline();
			if (Text.IsEmpty())
			{
				return;
			}

			FAssimpLogBridge::Report(Severity, Text);
		}

	private:
		EAssimpDiagnosticSeverity Severity;
	};
}

void FAssimpLogBridge::Install()
{
	if (GLogBridgeInstalled)
	{
		return;
	}

	// Assimp's logger is a singleton. NORMAL verbosity omits Assimp's very chatty debug channel;
	// VERBOSE would flood the Unreal log on any non-trivial file.
	if (Assimp::DefaultLogger::get() == nullptr)
	{
		Assimp::DefaultLogger::create("", Assimp::Logger::NORMAL, 0);
	}

	Assimp::Logger* Logger = Assimp::DefaultLogger::get();
	if (Logger == nullptr)
	{
		UE_LOG(LogAssimp, Warning, TEXT("Could not create Assimp's logger; import diagnostics will be unavailable."));
		return;
	}

	// One stream per severity, so the mapping to Unreal's verbosity levels is exact rather than
	// inferred by scraping the message text. The logger takes ownership of each stream.
	Logger->attachStream(new FAssimpSeverityStream(EAssimpDiagnosticSeverity::Error),   Assimp::Logger::Err);
	Logger->attachStream(new FAssimpSeverityStream(EAssimpDiagnosticSeverity::Warning), Assimp::Logger::Warn);
	Logger->attachStream(new FAssimpSeverityStream(EAssimpDiagnosticSeverity::Info),    Assimp::Logger::Info);

	GLogBridgeInstalled = true;
}

void FAssimpLogBridge::Uninstall()
{
	if (!GLogBridgeInstalled)
	{
		return;
	}

	// Destroys the logger and every attached stream.
	Assimp::DefaultLogger::kill();
	GLogBridgeInstalled = false;
}

void FAssimpLogBridge::Report(EAssimpDiagnosticSeverity Severity, const FString& Message)
{
	if (GActiveDiagnosticSink != nullptr)
	{
		GActiveDiagnosticSink->Add(FAssimpDiagnostic{ Severity, Message });
	}

	switch (Severity)
	{
	case EAssimpDiagnosticSeverity::Error:
		UE_LOG(LogAssimp, Error, TEXT("%s"), *Message);
		break;
	case EAssimpDiagnosticSeverity::Warning:
		UE_LOG(LogAssimp, Warning, TEXT("%s"), *Message);
		break;
	case EAssimpDiagnosticSeverity::Info:
	default:
		UE_LOG(LogAssimp, Verbose, TEXT("%s"), *Message);
		break;
	}
}

FAssimpLogBridge::FScopedCapture::FScopedCapture(TArray<FAssimpDiagnostic>& Diagnostics)
{
	PreviousSink = GActiveDiagnosticSink;
	GActiveDiagnosticSink = &Diagnostics;
}

FAssimpLogBridge::FScopedCapture::~FScopedCapture()
{
	GActiveDiagnosticSink = PreviousSink;
}
