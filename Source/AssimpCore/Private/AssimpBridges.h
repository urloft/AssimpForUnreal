// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

#pragma once

#include "AssimpIncludes.h"
#include "AssimpScene.h"
#include "CoreMinimal.h"

/**
 * Bridges between Assimp's extension points and Unreal's equivalents.
 *
 * Assimp is designed to be embedded: it delegates file IO, logging, and progress reporting to
 * interfaces the host implements. Implementing all three is what makes the plugin behave like part
 * of the engine rather than a library bolted onto it -- imports can read from Unreal's virtual file
 * system, their diagnostics land in Unreal's log with correct severities, and long imports can be
 * cancelled.
 */

/**
 * Read-only IOStream backed by an Unreal FArchive.
 *
 * Streams rather than loading the whole file into memory: Assimp performs many small reads and seeks
 * while parsing, which maps cleanly onto FArchive, and large source files stay off the heap.
 */
class FAssimpArchiveStream final : public Assimp::IOStream
{
public:
	/** Takes ownership of Archive. */
	explicit FAssimpArchiveStream(TUniquePtr<FArchive> InArchive);
	virtual ~FAssimpArchiveStream() override;

	//~ Begin Assimp::IOStream
	virtual size_t Read(void* Buffer, size_t Size, size_t Count) override;
	virtual size_t Write(const void* Buffer, size_t Size, size_t Count) override;
	virtual aiReturn Seek(size_t Offset, aiOrigin Origin) override;
	virtual size_t Tell() const override;
	virtual size_t FileSize() const override;
	virtual void Flush() override;
	//~ End Assimp::IOStream

private:
	TUniquePtr<FArchive> Archive;
	int64 TotalSize = 0;
};

/**
 * IOSystem backed by Unreal's IFileManager.
 *
 * Routing Assimp's file access through IFileManager rather than the C runtime means an import can
 * read anything Unreal can read, and that path handling matches the rest of the engine. It also
 * means a model referencing a sibling file (an .obj naming its .mtl, or a material naming a texture)
 * resolves through the same code path as the model itself.
 */
class FAssimpFileManagerIO final : public Assimp::IOSystem
{
public:
	/**
	 * @param InBaseDirectory Directory that relative paths are resolved against, normally the
	 *                        directory holding the source file.
	 */
	explicit FAssimpFileManagerIO(const FString& InBaseDirectory);
	virtual ~FAssimpFileManagerIO() override;

	//~ Begin Assimp::IOSystem
	virtual bool Exists(const char* File) const override;
	virtual char getOsSeparator() const override;
	virtual Assimp::IOStream* Open(const char* File, const char* Mode = "rb") override;
	virtual void Close(Assimp::IOStream* File) override;
	//~ End Assimp::IOSystem

private:
	/**
	 * Turns a path as recorded in a source file into something openable.
	 *
	 * Source files are unreliable here: they contain absolute paths from the authoring machine,
	 * Windows separators on Unix and vice versa, and paths relative to the model. This normalises
	 * separators, then tries the path as given, relative to the base directory, and finally the bare
	 * filename in the base directory -- which is what rescues a model whose texture paths point at
	 * a directory layout that no longer exists.
	 */
	FString ResolvePath(const char* File) const;

	FString BaseDirectory;
};

/**
 * ProgressHandler that reports progress to a delegate and lets it cancel the import.
 *
 * Assimp polls this during parsing and post-processing; returning false aborts the load and Assimp
 * unwinds cleanly. Set per Importer instance, so unlike logging this needs no thread-local state.
 */
class FAssimpProgressBridge final : public Assimp::ProgressHandler
{
public:
	explicit FAssimpProgressBridge(const FAssimpProgressDelegate& InDelegate);
	virtual ~FAssimpProgressBridge() override;

	//~ Begin Assimp::ProgressHandler
	/** @return false to abort the import. */
	virtual bool Update(float Percentage) override;
	//~ End Assimp::ProgressHandler

	/** True when the delegate asked to cancel. */
	bool WasCancelled() const { return bCancelled; }

private:
	FAssimpProgressDelegate Delegate;
	bool bCancelled = false;
};

/**
 * Routes Assimp's log output into Unreal's log, and optionally into a per-load diagnostic array.
 *
 * Assimp's logger is a process-wide singleton (Assimp::DefaultLogger), so there is no way to attach
 * a logger to a single Importer. The stream is therefore installed once for the process, and
 * per-load capture is done with a thread-local sink: a load pushes its diagnostics array as the
 * active sink for the duration, and any message logged on that thread is appended to it as well as
 * to the Unreal log. That keeps concurrent imports on different threads from mixing their
 * diagnostics.
 */
class FAssimpLogBridge
{
public:
	/** Installs the log stream. Idempotent; safe to call from module startup. */
	static void Install();

	/** Removes the log stream. Idempotent. */
	static void Uninstall();

	/**
	 * Scope guard that captures Assimp log output on the current thread into Diagnostics.
	 *
	 * Nesting is supported: the previous sink is restored on destruction.
	 */
	class FScopedCapture
	{
	public:
		explicit FScopedCapture(TArray<FAssimpDiagnostic>& Diagnostics);
		~FScopedCapture();

		FScopedCapture(const FScopedCapture&) = delete;
		FScopedCapture& operator=(const FScopedCapture&) = delete;

	private:
		TArray<FAssimpDiagnostic>* PreviousSink = nullptr;
	};

	/** Appends a diagnostic to the active sink, if any, and always writes to the Unreal log. */
	static void Report(EAssimpDiagnosticSeverity Severity, const FString& Message);
};
