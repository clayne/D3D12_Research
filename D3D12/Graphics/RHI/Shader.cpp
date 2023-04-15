#include "stdafx.h"
#include "Shader.h"
#include "Core/Paths.h"
#include "Core/CommandLine.h"
#include "Core/FileWatcher.h"
#include "dxc/dxcapi.h"
#include "dxc/d3d12shader.h"
#include "D3D.h"
#include "Core/Serializer.h"
#include "Graphics/Profiler.h"

namespace ShaderCompiler
{
	constexpr const char* pCompilerPath = "dxcompiler.dll";
	constexpr const char* pShaderSymbolsPath = "Saved/ShaderSymbols/";

	static RefCountPtr<IDxcUtils> pUtils;
	static RefCountPtr<IDxcCompiler3> pCompiler3;
	static RefCountPtr<IDxcValidator> pValidator;
	static RefCountPtr<IDxcIncludeHandler> pDefaultIncludeHandler;
	std::mutex m_CacheMutex;

	struct CompileJob
	{
		std::string FilePath;
		std::string EntryPoint;
		std::string Target;
		Span<ShaderDefine> Defines;
		std::vector<std::string> IncludeDirs;
		uint8 MajVersion;
		uint8 MinVersion;
		bool EnableDebugMode;
		bool EnableSymbols;
	};

	struct CompileResult
	{
		static constexpr int Version = 4;

		std::string ErrorMessage;
		ShaderBlob pBlob;
		RefCountPtr<IUnknown> pReflection;
		std::vector<std::string> Includes;
		uint64 ShaderHash[2];
		bool HasSymbols;
		bool IsDebug;

		bool Success() const { return pBlob.Get() && ErrorMessage.length() == 0; }
	};

	constexpr const char* GetShaderTarget(ShaderType type)
	{
		switch (type)
		{
		case ShaderType::Vertex:		return "vs";
		case ShaderType::Pixel:			return "ps";
		case ShaderType::Geometry:		return "gs";
		case ShaderType::Compute:		return "cs";
		case ShaderType::Mesh:			return "ms";
		case ShaderType::Amplification: return "as";
		default:						return "lib";
		}
	}

	void LoadDXC()
	{
		FN_PROC(DxcCreateInstance);

		HMODULE lib = LoadLibraryA(pCompilerPath);
		DxcCreateInstanceFn.Load(lib);

		VERIFY_HR(DxcCreateInstanceFn(CLSID_DxcUtils, IID_PPV_ARGS(pUtils.GetAddressOf())));
		VERIFY_HR(DxcCreateInstanceFn(CLSID_DxcCompiler, IID_PPV_ARGS(pCompiler3.GetAddressOf())));
		VERIFY_HR(DxcCreateInstanceFn(CLSID_DxcValidator, IID_PPV_ARGS(pValidator.GetAddressOf())));
		VERIFY_HR(pUtils->CreateDefaultIncludeHandler(pDefaultIncludeHandler.GetAddressOf()));
		E_LOG(Info, "Loaded %s", pCompilerPath);
	}

	bool ResolveFilePath(const CompileJob& job, std::string& outPath)
	{
		for (const std::string& includeDir : job.IncludeDirs)
		{
			outPath = Paths::Combine(includeDir, job.FilePath);
			if (Paths::FileExists(outPath.c_str()))
			{
				return true;
			}
		}
		outPath = "";
		return false;
	}

	bool TryLoadFromCache(const char* pCachePath, const CompileJob& compileJob, CompileResult& result)
	{
		std::lock_guard lock(m_CacheMutex);

		// See if the cache file exists
		if (!Paths::FileExists(pCachePath))
			return false;

		std::string shaderFullPath;
		if(!ResolveFilePath(compileJob, shaderFullPath))
			return false;

		// Check if the shader source is not newer than the cached file
		uint64 cacheTime, temp;
		Paths::GetFileTime(pCachePath, temp, temp, cacheTime);

		auto TestFileTime = [&](const char* pFilePath) {
			uint64 shaderTime;
			Paths::GetFileTime(pFilePath, temp, temp, shaderTime);
			return cacheTime >= shaderTime;
		};

		if (!TestFileTime(shaderFullPath.c_str()))
			return false;
		
		Serializer s;
		s.Open(pCachePath, Serializer::Mode::Read);
		uint32 version = 0;
		s.Serialize(version);
		if (version != CompileResult::Version)
			return false;

		s.Serialize(result.ShaderHash);
		s.Serialize(result.HasSymbols);
		if (result.HasSymbols != compileJob.EnableSymbols)
			return false;
		s.Serialize(result.IsDebug);
		if (result.IsDebug != compileJob.EnableDebugMode)
			return false;

		s.Serialize(result.Includes);

		// Test if includes sources are not newer than the cached file
		for (std::string& include : result.Includes)
		{
			if (!TestFileTime(include.c_str()))
				return false;
		}

		uint32 size = 0;
		void* pData = nullptr;
		s.Serialize(pData, size);
		pUtils->CreateBlob(pData, size, DXC_CP_ACP, (IDxcBlobEncoding**)result.pBlob.GetAddressOf());
		delete[] pData;

		return true;
	}

	bool SaveToCache(const char* pCachePath, const CompileJob& compileJob, CompileResult& result)
	{
		std::lock_guard lock(m_CacheMutex);

		Paths::CreateDirectoryTree(pCachePath);

		Serializer s;
		s.Open(pCachePath, Serializer::Mode::Write);
		uint32 version = CompileResult::Version;
		s.Serialize(version);
		s.Serialize(result.ShaderHash);
		s.Serialize(result.HasSymbols);
		s.Serialize(result.IsDebug);
		s.Serialize(result.Includes);
		void* pBlob = result.pBlob->GetBufferPointer();
		uint32 size = (uint32)result.pBlob->GetBufferSize();
		s.Serialize(pBlob, size);
		return true;
	}

	CompileResult Compile(const CompileJob& compileJob)
	{
		CompileResult result;

		std::string defineKey;
		for (const ShaderDefine& define : compileJob.Defines)
		{
			defineKey += define.Value;
		}
		StringHash hash(defineKey.c_str());

		std::string cachePath = Sprintf(
			"%s%s_%s_%x.bin",
			Paths::ShaderCacheDir().c_str(),
			Paths::GetFileNameWithoutExtension(compileJob.FilePath).c_str(),
			compileJob.EntryPoint.c_str(),
			hash.m_Hash
		);
		Paths::CreateDirectoryTree(cachePath);

		if (TryLoadFromCache(cachePath.c_str(), compileJob, result))
		{
			E_LOG(Info, "Loaded shader '%s.%s' from cache.", compileJob.FilePath.c_str(), compileJob.EntryPoint.c_str());
			return result;
		}

		TimeScope timer;
		RefCountPtr<IDxcBlobEncoding> pSource;
		std::string fullPath;
		if (!ResolveFilePath(compileJob, fullPath))
		{
			result.ErrorMessage = Sprintf("Failed to open file '%s'", compileJob.FilePath.c_str());
			return result;
		}

		if (!SUCCEEDED(pUtils->LoadFile(MULTIBYTE_TO_UNICODE(fullPath.c_str()), 0, pSource.GetAddressOf())))
		{
			result.ErrorMessage = Sprintf("Failed to load file '%s'", fullPath.c_str());
			return result;
		}

		class CompileArguments
		{
		public:
			void AddArgument(const char* pArgument, const char* pValue = nullptr)
			{
				m_Arguments.push_back(MULTIBYTE_TO_UNICODE(pArgument));
				if (pValue)
				{
					m_Arguments.push_back(MULTIBYTE_TO_UNICODE(pValue));
				}
			}
			void AddArgument(const wchar_t* pArgument, const wchar_t* pValue = nullptr)
			{
				m_Arguments.push_back(pArgument);
				if (pValue)
				{
					m_Arguments.push_back(pValue);
				}
			}

			void AddDefine(const char* pDefine, const char* pValue = nullptr)
			{
				if (strstr(pDefine, "=") != nullptr)
				{
					AddArgument("-D", pDefine);
				}
				else
				{
					AddArgument("-D", Sprintf("%s=%s", pDefine, pValue ? pValue : "1").c_str());
				}
			}

			const wchar_t** GetArguments()
			{
				m_ArgumentArr.reserve(GetNumArguments());
				for (const auto& arg : m_Arguments)
				{
					m_ArgumentArr.push_back(arg.c_str());
				}
				return m_ArgumentArr.data();
			}

			size_t GetNumArguments() const
			{
				return m_Arguments.size();
			}

			std::string ToString() const
			{
				std::string str;
				for (const std::wstring& arg : m_Arguments)
				{
					str += Sprintf(" %s", UNICODE_TO_MULTIBYTE(arg.c_str()));
				}
				return str;
			}

		private:
			std::vector<const wchar_t*> m_ArgumentArr;
			std::vector<std::wstring> m_Arguments;
		} arguments;

		std::string target = Sprintf("%s_%d_%d", compileJob.Target.c_str(), compileJob.MajVersion, compileJob.MinVersion);
		arguments.AddArgument(Paths::GetFileNameWithoutExtension(compileJob.FilePath).c_str());
		arguments.AddArgument("-E", compileJob.EntryPoint.c_str());
		arguments.AddArgument("-T", target.c_str());
		arguments.AddArgument(DXC_ARG_ALL_RESOURCES_BOUND);
		arguments.AddArgument(DXC_ARG_WARNINGS_ARE_ERRORS);
		arguments.AddArgument(DXC_ARG_PACK_MATRIX_ROW_MAJOR);

		arguments.AddArgument("-HV", "2021");

#if 0
		if (majVersion >= 6 && minVersion >= 6)
		{
			arguments.AddArgument("-enable-payload-qualifiers");
			arguments.AddDefine("_PAYLOAD_QUALIFIERS", "1");
		}
		else
#endif
		{
			arguments.AddArgument("-disable-payload-qualifiers");
			arguments.AddDefine("_PAYLOAD_QUALIFIERS", "0");
		}

		result.HasSymbols = compileJob.EnableSymbols;
		result.IsDebug = compileJob.EnableDebugMode;
		bool enableSymbols = result.HasSymbols || result.IsDebug;
		bool stripSymbols = enableSymbols && compileJob.EnableSymbols;

		if(compileJob.EnableDebugMode)
			arguments.AddArgument(DXC_ARG_SKIP_OPTIMIZATIONS);
		else
			arguments.AddArgument(DXC_ARG_OPTIMIZATION_LEVEL3);

		if (enableSymbols)
		{
			arguments.AddArgument(DXC_ARG_DEBUG);
			arguments.AddArgument(stripSymbols ? "-Qstrip_debug" : "-Qembed_debug");

			std::string pdbPath = Sprintf("%s.pdb", Paths::GetFileNameWithoutExtension(cachePath).c_str());
			arguments.AddArgument("-Fd", pdbPath.c_str());
		}

		arguments.AddArgument("-I", Paths::GetDirectoryPath(fullPath).c_str());
		for (const std::string& includeDir : compileJob.IncludeDirs)
		{
			arguments.AddArgument("-I", includeDir.c_str());
		}

		arguments.AddDefine(Sprintf("_SM_MAJ=%d", compileJob.MajVersion).c_str());
		arguments.AddDefine(Sprintf("_SM_MIN=%d", compileJob.MinVersion).c_str());
		arguments.AddDefine("_DXC");

		for (const ShaderDefine& define : compileJob.Defines)
		{
			arguments.AddDefine(define.Value.c_str());
		}

		DxcBuffer sourceBuffer;
		sourceBuffer.Ptr = pSource->GetBufferPointer();
		sourceBuffer.Size = pSource->GetBufferSize();
		sourceBuffer.Encoding = 0;

		class CustomIncludeHandler : public IDxcIncludeHandler
		{
		public:
			HRESULT STDMETHODCALLTYPE LoadSource(_In_ LPCWSTR pFilename, _COM_Outptr_result_maybenull_ IDxcBlob** ppIncludeSource) override
			{
				RefCountPtr<IDxcBlobEncoding> pEncoding;
				std::string path = Paths::Normalize(UNICODE_TO_MULTIBYTE(pFilename));
				check(Paths::ResolveRelativePaths(path));

				if (!Paths::FileExists(path.c_str()))
				{
					*ppIncludeSource = nullptr;
					return E_FAIL;
				}

				auto existingInclude = std::find_if(IncludedFiles.begin(), IncludedFiles.end(), [&path](const std::string& include) {
					return CString::StrCmp(include.c_str(), path.c_str(), false);
				});

				if (existingInclude != IncludedFiles.end())
				{
					static const char nullStr[] = " ";
					pUtils->CreateBlob(nullStr, ARRAYSIZE(nullStr), CP_UTF8, pEncoding.GetAddressOf());
					*ppIncludeSource = pEncoding.Detach();
					return S_OK;
				}

				if (!IsValidIncludePath(path.c_str()))
				{
					E_LOG(Warning, "Include path '%s' does not have a valid extension", path.c_str());
					return E_FAIL;
				}

				HRESULT hr = pUtils->LoadFile(pFilename, nullptr, pEncoding.GetAddressOf());
				if (SUCCEEDED(hr))
				{
					IncludedFiles.push_back(path);
					*ppIncludeSource = pEncoding.Detach();
				}
				else
				{
					*ppIncludeSource = nullptr;
				}
				return hr;
			}

			bool IsValidIncludePath(const char* pFilePath) const
			{
				std::string extension = Paths::GetFileExtenstion(pFilePath);
				CString::ToLower(extension.c_str(), extension.data());
				constexpr const char* pValidExtensions[] = { "hlsli", "h" };
				for (uint32 i = 0; i < ARRAYSIZE(pValidExtensions); ++i)
				{
					if (strcmp(pValidExtensions[i], extension.c_str()) == 0)
					{
						return true;
					}
				}
				return false;
			}

			HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, _COM_Outptr_ void __RPC_FAR* __RPC_FAR* ppvObject) override
			{
				return pDefaultIncludeHandler->QueryInterface(riid, ppvObject);
			}

			void Reset()
			{
				IncludedFiles.clear();
			}

			ULONG STDMETHODCALLTYPE AddRef(void) override {	return 0; }
			ULONG STDMETHODCALLTYPE Release(void) override { return 0; }

			std::vector<std::string> IncludedFiles;
		};

		if (CommandLine::GetBool("dumpshaders"))
		{
			// Preprocessed source
			RefCountPtr<IDxcResult> pPreprocessOutput;
			CompileArguments preprocessArgs = arguments;
			preprocessArgs.AddArgument("-P", ".");
			CustomIncludeHandler preprocessIncludeHandler;
			if (SUCCEEDED(pCompiler3->Compile(&sourceBuffer, preprocessArgs.GetArguments(), (uint32)preprocessArgs.GetNumArguments(), &preprocessIncludeHandler, IID_PPV_ARGS(pPreprocessOutput.GetAddressOf()))))
			{
				RefCountPtr<IDxcBlobUtf8> pHLSL;
				if(SUCCEEDED(pPreprocessOutput->GetOutput(DXC_OUT_HLSL, IID_PPV_ARGS(pHLSL.GetAddressOf()), nullptr)))
				{
					std::string filePathBase = Paths::GetFileNameWithoutExtension(cachePath);
					{
						std::ofstream str(Sprintf("%s%s.hlsl", Paths::ShaderCacheDir().c_str(), filePathBase.c_str()));
						str.write(pHLSL->GetStringPointer(), pHLSL->GetStringLength());
					}
					{
						std::ofstream str(Sprintf("%s%s.bat", Paths::ShaderCacheDir().c_str(), filePathBase.c_str()));
						str << Sprintf("dxc.exe %s -Fo %s.shaderbin %s.hlsl", arguments.ToString().c_str(), filePathBase.c_str(), filePathBase.c_str());
					}
				}
			}
		}

		CustomIncludeHandler includeHandler;
		RefCountPtr<IDxcResult> pCompileResult;
		VERIFY_HR(pCompiler3->Compile(&sourceBuffer, arguments.GetArguments(), (uint32)arguments.GetNumArguments(), &includeHandler, IID_PPV_ARGS(pCompileResult.GetAddressOf())));

		RefCountPtr<IDxcBlobUtf8> pErrors;
		if (SUCCEEDED(pCompileResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(pErrors.GetAddressOf()), nullptr)))
		{
			if (pErrors && pErrors->GetStringLength())
			{
				result.ErrorMessage = (char*)pErrors->GetStringPointer();
				return result;
			}
		}

		//Shader object
		{
			VERIFY_HR(pCompileResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(result.pBlob.GetAddressOf()), nullptr));
		}

		//Validation
		{
			RefCountPtr<IDxcOperationResult> pResult;
			VERIFY_HR(pValidator->Validate((IDxcBlob*)result.pBlob.Get(), DxcValidatorFlags_InPlaceEdit, pResult.GetAddressOf()));
			HRESULT validationResult;
			pResult->GetStatus(&validationResult);
			if (validationResult != S_OK)
			{
				RefCountPtr<IDxcBlobEncoding> pPrintBlob;
				RefCountPtr<IDxcBlobUtf8> pPrintBlobUtf8;
				pResult->GetErrorBuffer(pPrintBlob.GetAddressOf());
				pUtils->GetBlobAsUtf8(pPrintBlob.Get(), pPrintBlobUtf8.GetAddressOf());
				result.ErrorMessage = pPrintBlobUtf8->GetStringPointer();
				return result;
			}
		}

		// Hash
		{
			RefCountPtr<IDxcBlob> pHash;
			if (SUCCEEDED(pCompileResult->GetOutput(DXC_OUT_SHADER_HASH, IID_PPV_ARGS(pHash.GetAddressOf()), nullptr)))
			{
				DxcShaderHash* pHashBuf = (DxcShaderHash*)pHash->GetBufferPointer();
				memcpy(result.ShaderHash, pHashBuf->HashDigest, sizeof(uint64) * 2);
			}
		}

		//Symbols
		{
			RefCountPtr<IDxcBlobUtf16> pDebugDataPath;
			RefCountPtr<IDxcBlob> pSymbolsBlob;
			if (SUCCEEDED(pCompileResult->GetOutput(DXC_OUT_PDB, IID_PPV_ARGS(pSymbolsBlob.GetAddressOf()), pDebugDataPath.GetAddressOf())))
			{
				RefCountPtr<IDxcBlobUtf8> pDebugDataPathUTF8;
				pUtils->GetBlobAsUtf8(pDebugDataPath.Get(), pDebugDataPathUTF8.GetAddressOf());
				std::ofstream str(Sprintf("%s%s", Paths::ShaderCacheDir().c_str(), pDebugDataPathUTF8->GetStringPointer()), std::ios::binary);
				str.write((char*)pSymbolsBlob->GetBufferPointer(), pSymbolsBlob->GetBufferSize());
			}
		}

		//Reflection
		{
			RefCountPtr<IDxcBlob> pReflectionData;
			if (SUCCEEDED(pCompileResult->GetOutput(DXC_OUT_REFLECTION, IID_PPV_ARGS(pReflectionData.GetAddressOf()), nullptr)))
			{
				DxcBuffer reflectionBuffer;
				reflectionBuffer.Ptr = pReflectionData->GetBufferPointer();
				reflectionBuffer.Size = pReflectionData->GetBufferSize();
				reflectionBuffer.Encoding = 0;
				VERIFY_HR(pUtils->CreateReflection(&reflectionBuffer, IID_PPV_ARGS(result.pReflection.GetAddressOf())));
			}
		}

		result.Includes.push_back(fullPath);
		for (const std::string& includePath : includeHandler.IncludedFiles)
		{
			result.Includes.push_back(includePath);
		}

		check(SaveToCache(cachePath.c_str(), compileJob, result));
		E_LOG(Warning, "Missing cached shader. Compile time: %.1fms ('%s.%s')", timer.Stop() * 1000, compileJob.FilePath.c_str(), compileJob.EntryPoint.c_str());

		return result;
	}
}

ShaderManager::ShaderStringHash ShaderManager::GetEntryPointHash(const char* pEntryPoint, const Span<ShaderDefine>& defines)
{
	ShaderStringHash hash(pEntryPoint);
	for (const ShaderDefine& define : defines)
	{
		hash.Combine(ShaderStringHash(define.Value));
	}
	return hash;
}

void ShaderManager::RecompileFromFileChange(const std::string& filePath)
{
	auto it = m_IncludeDependencyMap.find(ShaderStringHash(filePath));
	if (it != m_IncludeDependencyMap.end())
	{
		E_LOG(Info, "Modified \"%s\". Recompiling dependencies...", filePath.c_str());
		const std::unordered_set<std::string>& dependencies = it->second;
		for (const std::string& dependency : dependencies)
		{
			auto objectMapIt = m_FilepathToObjectMap.find(ShaderStringHash(dependency));
			if (objectMapIt != m_FilepathToObjectMap.end())
			{
				ShadersInFileMap objectMap = objectMapIt->second;
				for (auto shader : objectMap.Shaders)
				{
					Shader* pOldShader = shader.second;
					Shader* pNewShader = GetShader(dependency.c_str(), pOldShader->Type, pOldShader->EntryPoint.c_str(), pOldShader->Defines, true);
					if (pNewShader)
					{
						check(pOldShader == pNewShader);
						m_OnShaderRecompiledEvent.Broadcast(pNewShader);
					}
				}
			}
		}
	}
}

ShaderManager::ShaderManager(uint8 shaderModelMaj, uint8 shaderModelMin)
	: m_ShaderModelMajor(shaderModelMaj), m_ShaderModelMinor(shaderModelMin)
{
	m_pFileWatcher = std::make_unique<FileWatcher>();
	ShaderCompiler::LoadDXC();
}

ShaderManager::~ShaderManager()
{
	for (Shader* pShader : m_Shaders)
		delete pShader;
}

void ShaderManager::ConditionallyReloadShaders()
{
	if (m_pFileWatcher)
	{
		FileEvent fileEvent;
		while (m_pFileWatcher->GetNextChange(fileEvent))
		{
			switch (fileEvent.EventType)
			{
			case FileEvent::Type::Modified:
				RecompileFromFileChange(fileEvent.Path);
				break;
			case FileEvent::Type::Added:
				break;
			case FileEvent::Type::Removed:
				break;
			}
		}
	}
}

void ShaderManager::AddIncludeDir(const std::string& includeDir)
{
	m_IncludeDirs.push_back(includeDir);
	if (m_pFileWatcher)
	{
		if (m_pFileWatcher->StartWatching(includeDir.c_str(), true))
		{
			E_LOG(Info, "Shader Hot-Reload enabled for: \"%s\"", includeDir.c_str());
		}
		else
		{
			E_LOG(Warning, "Shader Hot-Reload for \"%s\" failed.", includeDir.c_str());
		}
	}
}

Shader* ShaderManager::GetShader(const char* pShaderPath, ShaderType shaderType, const char* pEntryPoint, const Span<ShaderDefine>& defines /*= {}*/, bool force /*= false*/)
{
	// Libs have no entry point
	if (!pEntryPoint)
		pEntryPoint = "";

	ShaderStringHash pathHash(pShaderPath);
	ShaderStringHash hash = GetEntryPointHash(pEntryPoint, defines);

	Shader* pShader = nullptr;

	{
		std::lock_guard lock(m_CompileMutex);
		auto& shaderMap = m_FilepathToObjectMap[pathHash].Shaders;
		auto it = shaderMap.find(hash);
		if (it != shaderMap.end())
		{
			pShader = it->second;
		}
	}

	if (!force && pShader)
		return pShader;

	ShaderCompiler::CompileJob job;
	job.Defines = defines;
	job.EntryPoint = pEntryPoint;
	job.FilePath = pShaderPath;
	job.IncludeDirs = m_IncludeDirs;
	job.MajVersion = m_ShaderModelMajor;
	job.MinVersion = m_ShaderModelMinor;
	job.Target = ShaderCompiler::GetShaderTarget(shaderType);
	job.EnableDebugMode = CommandLine::GetBool("debugshaders");
	job.EnableSymbols = CommandLine::GetBool("shadersymbols");

	ShaderCompiler::CompileResult result = ShaderCompiler::Compile(job);

	if (!result.Success())
	{
		E_LOG(Warning, "Failed to compile shader \"%s:%s\": %s", pShaderPath, pEntryPoint, result.ErrorMessage.c_str());
		check(false);
		return nullptr;
	}

	{
		std::lock_guard lock(m_CompileMutex);

		if(!pShader)
			pShader = m_Shaders.emplace_back(new Shader());

		pShader->Defines = defines.Copy();
		pShader->EntryPoint = pEntryPoint;
		pShader->Type = shaderType;
		pShader->pByteCode = result.pBlob;
		memcpy(pShader->Hash, result.ShaderHash, sizeof(uint64) * 2);

		for (const std::string& include : result.Includes)
		{
			m_IncludeDependencyMap[ShaderStringHash(include)].insert(pShaderPath);
		}
		m_FilepathToObjectMap[pathHash].Shaders[hash] = pShader;
	}
	return pShader;
}
