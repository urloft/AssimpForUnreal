// Copyright (c) 2026 Pratik Kumar. Licensed under the MIT License.

using System;
using System.IO;
using UnrealBuildTool;

/// <summary>
/// External module exposing the vendored Assimp shared library to the rest of the plugin.
/// </summary>
/// <remarks>
/// This module deliberately exposes <em>only</em> Assimp's public <c>include/</c> directory. It does
/// not add Assimp's <c>code/</c> directory to any include path: that holds Assimp's private
/// implementation headers, and putting it on a public include path leaks Assimp's internal
/// <c>Windows.h</c> usage into every consumer that transitively includes this module. Only
/// AssimpCore's private translation units ever include an Assimp header, so nothing outside
/// AssimpCore sees Assimp at all.
///
/// The binaries are prebuilt and committed via Git LFS so the plugin compiles on clone.
/// Regenerate them with <c>Scripts/BuildAssimp.ps1</c>.
/// </remarks>
public class AssimpLibrary : ModuleRules
{
	/// <summary>Bare filename of the Assimp shared library, as recorded in its import library.</summary>
	/// <remarks>
	/// Scripts/BuildAssimp.ps1 configures Assimp with <c>-DLIBRARY_SUFFIX=</c> so the output is a
	/// plain "assimp.dll" rather than a toolset-tagged "assimp-vc143-mt.dll". That means this name
	/// is stable across MSVC toolsets and does not have to be discovered at build time.
	/// </remarks>
	private const string WindowsDllName = "assimp.dll";

	public AssimpLibrary(ReadOnlyTargetRules Target) : base(Target)
	{
		Type = ModuleType.External;

		// Assimp's public headers only. See the class remarks for why code/ is excluded.
		PublicIncludePaths.Add(Path.Combine(ModuleDirectory, "include"));

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			ConfigureWindows();
		}
		else
		{
			throw new BuildException(
				"AssimpForUnreal does not support {0} yet. Only Win64 is supported in this version. " +
				"To add a platform, extend Scripts/BuildAssimp.ps1 to produce binaries for it and " +
				"add a branch here.", Target.Platform);
		}
	}

	private void ConfigureWindows()
	{
		string LibPath = Path.Combine(ModuleDirectory, "lib", "Win64", "assimp.lib");
		string DllPath = Path.Combine(ModuleDirectory, "bin", "Win64", WindowsDllName);

		VerifyVendoredBinary(LibPath, "import library");
		VerifyVendoredBinary(DllPath, "shared library");

		PublicAdditionalLibraries.Add(LibPath);

		// Delay-load so the DLL is resolved by AssimpCore at module startup from the plugin's own
		// binaries directory, rather than by the OS loader from the working directory or PATH.
		//
		// This MUST be the bare filename exactly as it appears in the import library's name table.
		// Passing a full path here (as some other Assimp integrations do) does not match any import
		// entry, so the delay-load hook never fires and the load silently falls back to the default
		// search order -- which is why such plugins tend to work in-editor and then fail to find the
		// DLL in a packaged build.
		PublicDelayLoadDLLs.Add(WindowsDllName);

		// Stage the DLL into the PLUGIN's own Binaries directory, matching the engine's
		// ThirdPartyLibrary plugin template.
		//
		// Not $(TargetOutputDir): for an editor target against an installed engine that resolves to
		// Engine/Binaries/Win64, i.e. inside the engine installation. Staging there would write into
		// Program Files (needing elevation, and silently doing nothing without it), pollute the
		// engine install with a plugin's dependency, and leave the DLL behind when the plugin is
		// removed. Keeping it under $(PluginDir) means the binary travels with the plugin and is
		// staged into packaged builds by the same declaration.
		string StagedDllPath = System.String.Join("/",
			"$(PluginDir)", "Binaries", "ThirdParty", "AssimpLibrary", "Win64", WindowsDllName);

		RuntimeDependencies.Add(StagedDllPath, DllPath);

		// Stage any dependency DLLs sitting beside it.
		//
		// Assimp's contrib dependencies obey the global BUILD_SHARED_LIBS, so Draco ships as its own
		// draco.dll that assimp.dll imports. Missing it means assimp.dll does not load AT ALL, which
		// disables every format rather than only the Draco ones -- so the whole directory is staged
		// rather than a hard-coded list, and Scripts/BuildAssimp.ps1 curates what lands there.
		string BinDirectory = Path.GetDirectoryName(DllPath);
		foreach (string Dependency in Directory.GetFiles(BinDirectory, "*.dll"))
		{
			string DependencyName = Path.GetFileName(Dependency);
			if (DependencyName == WindowsDllName)
			{
				continue;
			}

			RuntimeDependencies.Add(
				System.String.Join("/", "$(PluginDir)", "Binaries", "ThirdParty", "AssimpLibrary",
					"Win64", DependencyName),
				Dependency);
		}

		// AssimpCore resolves the delay-loaded library explicitly at startup. Both the bare name and
		// the staged location it should look in are published here so the path exists in exactly one
		// place and the two modules cannot drift apart.
		PublicDefinitions.Add("ASSIMP_SHARED_LIBRARY_NAME=TEXT(\"" + WindowsDllName + "\")");
		PublicDefinitions.Add("ASSIMP_SHARED_LIBRARY_PLUGIN_SUBPATH=TEXT(\"Binaries/ThirdParty/AssimpLibrary/Win64\")");
	}

	/// <summary>
	/// Fails the build with an actionable message when a vendored binary is missing, instead of
	/// letting it surface later as an opaque linker or loader error.
	/// </summary>
	private void VerifyVendoredBinary(string FilePath, string Description)
	{
		if (File.Exists(FilePath))
		{
			return;
		}

		throw new BuildException(
			"AssimpForUnreal: the vendored Assimp {0} is missing:\n" +
			"    {1}\n\n" +
			"These binaries are tracked with Git LFS. Most often this means LFS objects were not " +
			"fetched, so the file on disk is still a text pointer.\n\n" +
			"Fix it with either:\n" +
			"    git lfs install && git lfs pull\n" +
			"or rebuild Assimp from source:\n" +
			"    ./Scripts/BuildAssimp.ps1\n",
			Description, FilePath);
	}
}
