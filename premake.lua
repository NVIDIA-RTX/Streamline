require("vstudio")

local ROOT = "./"

nvcfg = {}


nvcfg.SL_BUILD_DLSS_DN = true

-- Whether or not DLSS-D should be available publicly at an SDK level.
--
-- When this flag is false, DLSS-D related content will not be included into the
-- public Streamline SDK archives. Content may include the following:
-- - Documentation
-- - Headers
-- - Libraries
-- - Symbols
--
-- Note: DLSS-D will still be supported by these builds at an sl.interposer
-- level so that developers can make and test changes unrelated to DLSS-D whilst
-- maintaining DLSS-D support.
nvcfg.SL_DLSS_DN_PUBLIC_SDK = true




nvcfg.SL_BUILD_DEEPDVC = true

nvcfg.SL_DEEPDVC_PUBLIC_SDK = true

nvcfg.SL_BUILD_LATEWARP = true






nvcfg.SL_DIRECTSR_PUBLIC_SDK = true
-- SL DirectSR plugin build config option. Enabling this will enable the DirectSR plugin build
nvcfg.SL_BUILD_DIRECTSR = true


newoption {
	trigger = "with-nvllvk",
	description = "Use NvLowLatencyVk",
	allowed = {
		{"yes", "yes"},
		{"no", "no"}
	},
	default = "yes"
}

newoption {
	trigger = "with-toolset",
	description = "Specify premake toolset"
}

workspace "streamline"

	-- _ACTION is the argument you passed into premake5 when you ran it.
	local project_action = "UNDEFINED"
	if _ACTION ~= nill then project_action = _ACTION end

	-- Where the project files (vs project, solution, etc) go
	location( ROOT .. "_project/" .. project_action)

	configurations { "Debug", "Develop", "Production" }
	platforms {
		"x64",
	}
	architecture "x64"
	language "c++"
	preferredtoolarchitecture "x86_64"
	if (_OPTIONS["with-toolset"]) then
		toolset (_OPTIONS["with-toolset"])
	end
	
		  
	local externaldir = (ROOT .."external/")

	includedirs 
	{ 
		".", ROOT,
		externaldir .. "json/include/",
		externaldir .. "perf-sdk/include",
		externaldir .. "perf-sdk/include",
		externaldir .. "perf-sdk/include/windows-desktop-x64",
		externaldir .. "perf-sdk/NvPerfUtility/include",
		externaldir .. "vulkan/Include"
	}
   	 
	if os.host() == "windows" then
		systemversion "latest"
		defines { "SL_SDK", "SL_WINDOWS", "WIN32" , "WIN64" , "_CONSOLE", "NOMINMAX"}
	else
		defines { "SL_SDK", "SL_LINUX" }
		-- stop on first error and also downgrade checks for casting due to ReShade 
		buildoptions {"-std=c++17", "-Wfatal-errors", "-fpermissive"}
	end

	filter "platforms:x64"
		architecture "x64"
	filter{}

	-- when building any visual studio project
	filter {"system:windows", "action:vs*"}
		flags { "MultiProcessorCompile", "NoMinimalRebuild"}		
		
	flags { "FatalWarnings" }
	-- Enable additional warnings: https://premake.github.io/docs/warnings
	--warnings "Extra"

	-- building makefiles
	cppdialect "C++20"
	
	filter "configurations:Debug"
		defines { "DEBUG", "_DEBUG", "SL_ENABLE_TIMING=1", "SL_DEBUG" }
		symbols "Full"
				
	filter "configurations:Develop"
		defines { "NDEBUG","SL_ENABLE_TIMING=0","SL_ENABLE_PROFILING=0", "SL_DEVELOP" }
		optimize "On"
		flags { "LinkTimeOptimization" }

	filter "configurations:Production"
		defines { "NDEBUG","SL_ENABLE_TIMING=0","SL_ENABLE_PROFILING=0","SL_PRODUCTION" }
		optimize "On"
		flags { "LinkTimeOptimization" }

	filter { "files:**.hlsl" }
		buildmessage 'Compiling shader %{file.relpath} to DXBC/SPIRV with slang'
        buildcommands {				
			path.translate("../../external/slang/bin/windows-x64/release/")..'slangc "%{file.relpath}" -entry main -target spirv -o "../../_artifacts/shaders/%{file.basename}.spv"',
			path.translate("../../external/slang/bin/windows-x64/release/")..'slangc "%{file.relpath}" -profile sm_5_0 -entry main -target dxbc -o "../../_artifacts/shaders/%{file.basename}.cs"',
			'pushd '..path.translate("../../_artifacts/shaders"),
			'powershell.exe -NoProfile -ExecutionPolicy Bypass '..path.translate("../../tools/")..'bin2cheader.ps1 -i "%{file.basename}.spv"  > "%{file.basename}_spv.h"',
			'powershell.exe -NoProfile -ExecutionPolicy Bypass '..path.translate("../../tools/")..'bin2cheader.ps1 -i "%{file.basename}.cs"  > "%{file.basename}_cs.h"',
			'popd'
		 }	  
		 -- One or more outputs resulting from the build (required)
		 buildoutputs { ROOT .. "_artifacts/shaders/%{file.basename}.spv", ROOT .. "_artifacts/shaders/%{file.basename}.cs" }	  
		 -- One or more additional dependencies for this build command (optional)
		 --buildinputs { 'path/to/file1.ext', 'path/to/file2.ext' }
	
	filter { "files:**.json" }
		buildmessage 'Compiling %{file.relpath} to %{file.basename}_json.h'
		buildcommands {
			'copy "%{file.relpath}" "../../_artifacts/json/%{file.name}"',
			'pushd '..path.translate("../../_artifacts/json"),
			'powershell.exe -NoProfile -ExecutionPolicy Bypass '..path.translate("../../tools/")..'bin2cheader.ps1 -i "%{file.basename}.json"  > "../../_artifacts/json/%{file.basename}_json.h"',
			'popd'
		}
		-- One or more outputs resulting from the build (required)
		buildoutputs { ROOT .. "_artifacts/json/%{file.basename}.json"}	  
		-- One or more additional dependencies for this build command (optional)
		-- buildinputs { 'path/to/file1.ext', 'path/to/file2.ext' }

	filter {}

	filter {} -- clear filter when you know you no longer need it!
	
	vpaths { ["shaders"] = {"**.hlsl" } }

group ""

group "core"

project "sl.interposer"
	kind "SharedLib"		
	targetdir (ROOT .. "_artifacts/%{prj.name}/%{cfg.buildcfg}_%{cfg.platform}")
	objdir (ROOT .. "_artifacts/%{prj.name}/%{cfg.buildcfg}_%{cfg.platform}") 
	characterset ("MBCS")
	staticruntime "off"
	
	if os.host() == "windows" then
		prebuildcommands { 'pushd '..path.translate("../../_artifacts"), path.translate("../tools/").."gitVersion.bat", 'popd' }
	else
		prebuildcommands { 'pushd '..path.translate("../../_artifacts"), path.translate("../tools/").."gitVersion.sh", 'popd' }
	end

	
	filter "platforms:x64"
		includedirs { "./external/nvapi" }
	filter{}

	defines {"SL_INTERPOSER"}

	files {
		"./source/core/sl.interposer/**.h",
		"./source/core/sl.interposer/**.cpp",
		"./source/core/sl.interposer/**.rc",
	}

	if os.host() == "windows" then
		vpaths { ["proxies/d3d12"] = {"./source/core/sl.interposer/d3d12/**.h", "./source/core/sl.interposer/d3d12/**.cpp" }}
		vpaths { ["proxies/d3d11"] = {"./source/core/sl.interposer/d3d11/**.h", "./source/core/sl.interposer/d3d11/**.cpp" }}
		vpaths { ["proxies/dxgi"] = {"./source/core/sl.interposer/dxgi/**.h", "./source/core/sl.interposer/dxgi/**.cpp" }}

		linkoptions { "/DEF:../../source/core/sl.interposer/exports.def" }
	else
		-- remove on Linux all DX related stuff
		removefiles
		{
			"./source/core/sl.interposer/d3d**",
			"./source/core/sl.interposer/dxgi**",
			"./source/core/sl.interposer/resource.h",
			"./source/core/sl.interposer/**.rc"
		}
	end

	vpaths { ["proxies/vulkan"] = {"./source/core/sl.interposer/vulkan/**.h", "./source/core/sl.interposer/vulkan/**.cpp" }}
	vpaths { ["hook"] = {"./source/core/sl.interposer/hook**"}}


	files {
		"./include/**.h",
		"./source/core/sl.api/**.h",
		"./source/core/sl.api/**.cpp",
		"./source/core/sl.param/**.h",
		"./source/core/sl.param/**.cpp",
		"./source/core/sl.log/**.h",
		"./source/core/sl.log/**.cpp",
		"./source/core/sl.exception/**.h",
		"./source/core/sl.exception/**.cpp",
		"./source/core/sl.security/**.h",
		"./source/core/sl.security/**.cpp",
		"./source/core/sl.plugin-manager/**.h",
		"./source/core/sl.plugin-manager/**.cpp",		
		"./source/core/sl.plugin/inter_plugin_communication.h",
	}

	if os.host() == "windows" then
		defines {"VK_USE_PLATFORM_WIN32_KHR", "SL_ENABLE_EXCEPTION_HANDLING"}
		vpaths { ["security"] = {"./source/security/**.h","./source/security/**.cpp"}}

		links {"dbghelp.lib"}
		filter "platforms:x64"
			links {"external/nvapi/amd64/nvapi64.lib"}
		filter{}
	end
	
	vpaths { ["manager"] = {"./source/core/sl.plugin-manager/**.h", "./source/core/sl.plugin-manager/**.cpp" }}
	vpaths { ["api"] = {"./source/core/sl.api/**.h","./source/core/sl.api/**.cpp"}}
	vpaths { ["include"] = {"./include/**.h"}}
	vpaths { ["log"] = {"./source/core/sl.log/**.h","./source/core/sl.log/**.cpp"}}
	vpaths { ["exception"] = {"./source/core/sl.exception/**.h","./source/core/sl.exception/**.cpp"}}	
	vpaths { ["params"] = {"./source/core/sl.param/**.h","./source/core/sl.param/**.cpp"}}
	vpaths { ["security"] = {"./source/core/sl.security/**.h","./source/core/sl.security/**.cpp"}}
	vpaths { ["version"] = {"./source/core/sl.interposer/versions.h","./source/core/sl.interposer/resource.h","./source/core/sl.interposer/**.rc"}}
	vpaths { ["plugin"] = {"./source/core/sl.plugin/inter_plugin_communication.h",}}

	removefiles 
	{ 	
		"./source/core/sl.plugin-manager/pluginManagerEntry.cpp","./source/core/sl.api/plugin-manager.h"
	}
   
	

group ""

group "platforms"

project "sl.compute"
	kind "StaticLib"	
	targetdir (ROOT .. "_artifacts/%{prj.name}/%{cfg.buildcfg}_%{cfg.platform}")
	objdir (ROOT .. "_artifacts/%{prj.name}/%{cfg.buildcfg}_%{cfg.platform}") 
	characterset ("MBCS")
	staticruntime "off"
	dependson { "sl.interposer"}


	filter "platforms:x64"
		includedirs { "./external/nvapi" }
	filter{}

	if os.host() == "windows" then
		if (os.isfile("./external/slang/bin/windows-x64/release/slangc.exe")) then
		files {
			"./shaders/**.hlsl"
		}
		end
		files {
			"./source/platforms/sl.chi/capture.h",
			"./source/platforms/sl.chi/capture.cpp",
			"./source/platforms/sl.chi/compute.h",
			"./source/platforms/sl.chi/generic.h",		
			"./source/platforms/sl.chi/d3d12.cpp",
			"./source/platforms/sl.chi/d3d12.h",
			"./source/platforms/sl.chi/d3d11.cpp",
			"./source/platforms/sl.chi/d3d11.h",
			"./source/platforms/sl.chi/vulkan.cpp",
			"./source/platforms/sl.chi/vulkan.h",
			"./source/platforms/sl.chi/generic.cpp",
			"./source/core/sl.security/**.h",
			"./source/core/sl.security/**.cpp"
		}
		filter { "options:with-nvllvk=yes" }
			defines { "SL_WITH_NVLLVK" }
			files { "./source/platforms/sl.chi/nvllvk.cpp" }
		filter {}
	else
		files {
			"./shaders/**.hlsl",
			"./source/platforms/sl.chi/capture.h",
			"./source/platforms/sl.chi/capture.cpp",
			"./source/platforms/sl.chi/compute.h",
			"./source/platforms/sl.chi/generic.h",		
			"./source/platforms/sl.chi/vulkan.cpp",
			"./source/platforms/sl.chi/vulkan.h",
			"./source/platforms/sl.chi/generic.cpp"	
		}
	end

	vpaths { ["chi"] = {"./source/platforms/sl.chi/**.h","./source/platforms/sl.chi/**.cpp"}}
	vpaths { ["security"] = {"./source/core/sl.security/**.h","./source/core/sl.security/**.cpp"}}

group ""

group "plugins"

function pluginBasicSetup(name)

	filter "platforms:x64"
		includedirs { "./external/nvapi" }
	filter{}

	files { 
		"./source/core/sl.api/**.h",
		"./source/core/sl.log/**.h",		
		"./source/core/sl.ota/**.h",		
		"./source/core/sl.security/**.h",
		"./source/core/sl.security/**.cpp",
		"./source/core/sl.file/**.h",
		"./source/core/sl.file/**.cpp",
		"./source/core/sl.extra/**.h",		
		"./source/core/sl.plugin/**.h",
		"./source/core/sl.plugin/**.cpp",
		"./source/plugins/sl."..name.."/versions.h",
		"./source/plugins/sl."..name.."/resource.h",
		"./source/plugins/sl."..name.."/**.rc"
	}
	removefiles {"./source/core/sl.api/plugin-manager.h"}
	
	vpaths { ["api"] = {"./source/core/sl.api/**.h"}}
	vpaths { ["log"] = {"./source/core/sl.log/**.h","./source/core/sl.log/**.cpp"}}
	vpaths { ["ota"] = {"./source/core/sl.ota/**.h", "./source/core/sl.ota/**.cpp"}}	
	vpaths { ["file"] = {"./source/core/sl.file/**.h", "./source/core/sl.file/**.cpp"}}	
	vpaths { ["extra"] = {"./source/core/sl.extra/**.h", "./source/core/sl.extra/**.cpp"}}		
	vpaths { ["plugin"] = {"./source/core/sl.plugin/**.h","./source/core/sl.plugin/**.cpp"}}
	vpaths { ["security"] = {"./source/core/sl.security/**.h","./source/core/sl.security/**.cpp"}}
	vpaths { ["version"] = {"./source/plugins/sl."..name.."/resource.h","./source/plugins/sl."..name.."/versions.h","./source/plugins/sl."..name.."/**.rc"}}
end

project "sl.common"
	kind "SharedLib"	
	targetdir (ROOT .. "_artifacts/%{prj.name}/%{cfg.buildcfg}_%{cfg.platform}")
	objdir (ROOT .. "_artifacts/%{prj.name}/%{cfg.buildcfg}_%{cfg.platform}") 
	characterset ("MBCS")
	dependson { "sl.compute"}

	pluginBasicSetup("common")

	defines {"SL_COMMON_PLUGIN"}

	files { 
		"./source/core/sl.extra/**.cpp",		
		"./source/plugins/sl.common/**.json",
		"./source/plugins/sl.common/**.h", 
		"./source/plugins/sl.common/**.cpp",
		"./source/core/ngx/**.h",
		"./source/core/ngx/**.cpp",		
		"./source/core/sl.ota/**.cpp",
	}

	vpaths { ["imgui"] = {"./external/imgui/**.cpp" }}
	vpaths { ["impl"] = {"./source/plugins/sl.common/**.h", "./source/plugins/sl.common/**.cpp" }}
	--vpaths { ["ngx"] = {"./source/core/ngx/**.h", "./source/core/ngx/**.cpp"}}
	
	filter "platforms:x64"
		libdirs {externaldir .."nvapi/amd64",externaldir .."ngx-sdk/Lib/Windows_x86_64", externaldir .."pix/bin", externaldir .."reflex-sdk-vk/lib"}
		links { "nvapi64.lib" }
	filter{}
    links
    {     
		"delayimp.lib", "d3d12.lib", "dxgi.lib", "dxguid.lib", (ROOT .. "_artifacts/sl.compute/%{cfg.buildcfg}_%{cfg.platform}/sl.compute.lib"),
		"Version.lib"
	}

    filter "configurations:Debug"
	 	links { "nvsdk_ngx_d_dbg.lib" }
	filter "configurations:Develop or Production"
		links { "nvsdk_ngx_d.lib"}
	filter {}

	filter { "options:with-nvllvk=yes" }
		defines { "SL_WITH_NVLLVK" }
		links { "NvLowLatencyVk.lib" }
		linkoptions { "/DELAYLOAD:NvLowLatencyVk.dll" }
	filter {}

if (os.isdir("./source/plugins/sl.dlss_g")) then
	project "sl.dlss_g"
		kind "SharedLib"	
		targetdir (ROOT .. "_artifacts/%{prj.name}/%{cfg.buildcfg}_%{cfg.platform}")
		objdir (ROOT .. "_artifacts/%{prj.name}/%{cfg.buildcfg}_%{cfg.platform}") 
		characterset ("MBCS")
		dependson { "sl.common"}
		pluginBasicSetup("dlss_g")

		files { 
			"./source/plugins/sl.dlss_g/**.json",
			"./source/plugins/sl.dlss_g/**.h", 
			"./source/plugins/sl.dlss_g/**.cpp",
		}

		links {"external/nvapi/amd64/nvapi64.lib", "Winmm.lib", "Version.lib" }

		vpaths {["impl"] = { "./source/plugins/sl.dlss_g/**.h", "./source/plugins/sl.dlss_g/**.cpp" }}
		
		removefiles {"./source/core/sl.extra/extra.cpp"}
end

project "sl.dlss"
	kind "SharedLib"	
	targetdir (ROOT .. "_artifacts/%{prj.name}/%{cfg.buildcfg}_%{cfg.platform}")
	objdir (ROOT .. "_artifacts/%{prj.name}/%{cfg.buildcfg}_%{cfg.platform}") 
	characterset ("MBCS")
	dependson { "sl.common"}
	pluginBasicSetup("dlss")
	
	files { 
		"./source/core/ngx/**.h",
		"./source/core/ngx/**.cpp",		
		"./source/plugins/sl.dlss/**.json",
		"./source/plugins/sl.dlss/**.h",
		"./source/plugins/sl.dlss/**.cpp"
	}

	vpaths { ["impl"] = {"./source/plugins/sl.dlss/**.h", "./source/plugins/sl.dlss/**.cpp" }}
	vpaths { ["ngx"] = {"./source/core/ngx/**.h", "./source/core/ngx/**.cpp"}}
		
	removefiles {"./source/core/sl.extra/extra.cpp"}
  	

project "sl.reflex"
	kind "SharedLib"	
	targetdir (ROOT .. "_artifacts/%{prj.name}/%{cfg.buildcfg}_%{cfg.platform}")
	objdir (ROOT .. "_artifacts/%{prj.name}/%{cfg.buildcfg}_%{cfg.platform}") 
	characterset ("MBCS")
	pluginBasicSetup("reflex")
	
	files { 
		"./source/plugins/sl.reflex/**.json", 
		"./source/plugins/sl.reflex/**.h",
		"./source/plugins/sl.reflex/**.cpp"
	}

	vpaths { ["impl"] = {"./source/plugins/sl.reflex/**.h", "./source/plugins/sl.reflex/**.cpp" }}
			
	removefiles {"./source/core/sl.extra/extra.cpp"}

project "sl.pcl"
	kind "SharedLib"
	targetdir (ROOT .. "_artifacts/%{prj.name}/%{cfg.buildcfg}_%{cfg.platform}")
	objdir (ROOT .. "_artifacts/%{prj.name}/%{cfg.buildcfg}_%{cfg.platform}") 
	characterset ("MBCS")
	pluginBasicSetup("pcl")
	
	files { 
		"./source/plugins/sl.pcl/**.json",
		"./source/plugins/sl.pcl/**.h",
		"./source/plugins/sl.pcl/**.cpp"

	}

	vpaths { ["impl"] = {"./source/plugins/sl.pcl/**.h", "./source/plugins/sl.pcl/**.cpp" }}
			
	removefiles {"./source/core/sl.extra/extra.cpp"}
   	
project "sl.template"
	kind "SharedLib"	
	targetdir (ROOT .. "_artifacts/%{prj.name}/%{cfg.buildcfg}_%{cfg.platform}")
	objdir (ROOT .. "_artifacts/%{prj.name}/%{cfg.buildcfg}_%{cfg.platform}") 
	characterset ("MBCS")
	pluginBasicSetup("template")
	
	files { 
		"./source/plugins/sl.template/**.json",
		"./source/plugins/sl.template/**.h",
		"./source/plugins/sl.template/**.cpp"
	}

	vpaths { ["impl"] = {"./source/plugins/sl.template/**.h", "./source/plugins/sl.template/**.cpp" }}
			
	removefiles {"./source/core/sl.extra/extra.cpp"}
	
project "sl.nis"
	kind "SharedLib"
	targetdir (ROOT .. "_artifacts/%{prj.name}/%{cfg.buildcfg}_%{cfg.platform}")
	objdir (ROOT .. "_artifacts/%{prj.name}/%{cfg.buildcfg}_%{cfg.platform}")
	characterset ("MBCS")
	dependson { "sl.compute"}
	dependson { "sl.common"}
	pluginBasicSetup("nis")

	files {
		"./source/plugins/sl.nis/**.json",
		"./source/plugins/sl.nis/**.h",
		"./source/plugins/sl.nis/**.cpp"
	}

	vpaths { ["impl"] = {"./source/plugins/sl.nis/**.h", "./source/plugins/sl.nis/**.cpp" }}

	removefiles {"./source/core/sl.extra/extra.cpp"}

if nvcfg.SL_BUILD_DEEPDVC then
project "sl.deepdvc"
	kind "SharedLib"
	targetdir (ROOT .. "_artifacts/%{prj.name}/%{cfg.buildcfg}_%{cfg.platform}")
	objdir (ROOT .. "_artifacts/%{prj.name}/%{cfg.buildcfg}_%{cfg.platform}")
	characterset ("MBCS")
	dependson { "sl.compute"}
	dependson { "sl.common"}
	pluginBasicSetup("deepdvc")

	files {
		"./source/core/ngx/**.h",
		"./source/core/ngx/**.cpp",
		"./source/plugins/sl.deepdvc/**.json",
		"./source/plugins/sl.deepdvc/**.h",
		"./source/plugins/sl.deepdvc/**.cpp"
	}

	vpaths { ["impl"] = {"./source/plugins/sl.deepdvc/**.h", "./source/plugins/sl.deepdvc/**.cpp" }}
	vpaths { ["ngx"] = {"./source/core/ngx/**.h", "./source/core/ngx/**.cpp"}}

	removefiles {"./source/core/sl.extra/extra.cpp"}
end

project "sl.imgui"
	kind "SharedLib"
	targetdir (ROOT .. "_artifacts/%{prj.name}/%{cfg.buildcfg}_%{cfg.platform}")
	objdir (ROOT .. "_artifacts/%{prj.name}/%{cfg.buildcfg}_%{cfg.platform}")
	characterset ("MBCS")
	dependson { "sl.compute"}	
	pluginBasicSetup("imgui")

	files {
		"./source/plugins/sl.imgui/**.json",
		"./source/plugins/sl.imgui/**.h",
		"./source/plugins/sl.imgui/**.cpp",
		"./external/imgui/imgui*.cpp",
		"./external/implot/*.h",
		"./external/implot/*.cpp"
	}

	vpaths { ["helpers"] = {"./source/plugins/sl.imgui/imgui_impl**"}}
	vpaths { ["impl"] = {"./source/plugins/sl.imgui/**.h", "./source/plugins/sl.imgui/**.cpp" }}
	vpaths { ["implot"] = {"./external/implot/*.h", "./external/implot/*.cpp"}}
	vpaths { ["imgui"] = {"./external/imgui/**.cpp" }}
	
	defines {"ImDrawIdx=unsigned int"}

	-- For warning in ryml_all.hpp:
	-- warning C4996: 'std::aligned_storage': warning STL4034: std::aligned_storage and std::aligned_storage_t are deprecated in C++23. Prefer alignas(T) std::byte t_buff[sizeof(T)].
	defines {"_SILENCE_CXX23_ALIGNED_STORAGE_DEPRECATION_WARNING"}
	
	removefiles {"./source/core/sl.extra/extra.cpp"}

	libdirs {externaldir .."vulkan/Lib"}

	links { "d3d12.lib", "vulkan-1.lib"}
	
if nvcfg.SL_BUILD_DLSS_DN then
    project "sl.dlss_d"
	    kind "SharedLib"
	    targetdir (ROOT .. "_artifacts/%{prj.name}/%{cfg.buildcfg}_%{cfg.platform}")
	    objdir (ROOT .. "_artifacts/%{prj.name}/%{cfg.buildcfg}_%{cfg.platform}") 
	    characterset ("MBCS")
	    dependson { "sl.common"}
	    pluginBasicSetup("dlss_d")

	    files { 
			"./source/core/ngx/**.h",
			"./source/core/ngx/**.cpp",
			"./source/plugins/sl.dlss_d/**.json",
			"./source/plugins/sl.dlss_d/**.h",
			"./source/plugins/sl.dlss_d/**.cpp"
	    }

	    vpaths { ["impl"] = {"./source/plugins/sl.dlss_d/**.h", "./source/plugins/sl.dlss_d/**.cpp" }}
	    vpaths { ["ngx"] = {"./source/core/ngx/**.h", "./source/core/ngx/**.cpp"}}

	    removefiles {"./source/core/sl.extra/extra.cpp"}
end

if nvcfg.SL_BUILD_DIRECTSR then
    project "sl.directsr"
	    kind "SharedLib"
	    targetdir (ROOT .. "_artifacts/%{prj.name}/%{cfg.buildcfg}_%{cfg.platform}")
	    objdir (ROOT .. "_artifacts/%{prj.name}/%{cfg.buildcfg}_%{cfg.platform}")
	    characterset ("MBCS")
	    dependson { "sl.common" }
	    pluginBasicSetup("directsr")

	    files {
			"./source/core/ngx/**.h",
			"./source/core/ngx/**.cpp",
			"./source/plugins/sl.directsr/**.json",
			"./source/plugins/sl.directsr/**.h",
			"./source/plugins/sl.directsr/**.cpp"
	    }

	    removefiles {"./source/core/sl.extra/extra.cpp"}
end

if (os.isdir("./source/plugins/sl.nvperf")) then
	project "sl.nvperf"
		kind "SharedLib"	
		targetdir (ROOT .. "_artifacts/%{prj.name}/%{cfg.buildcfg}_%{cfg.platform}")
		objdir (ROOT .. "_artifacts/%{prj.name}/%{cfg.buildcfg}_%{cfg.platform}") 
		characterset ("MBCS")
		dependson { "sl.imgui"}
		pluginBasicSetup("nvperf")
		
		files { 
			"./source/plugins/sl.nvperf/**.json",
			"./source/plugins/sl.nvperf/**.h",
			"./source/plugins/sl.nvperf/**.cpp"
		}

		vpaths { ["impl"] = {"./source/plugins/sl.nvperf/**.h", "./source/plugins/sl.nvperf/**.cpp" }}

		-- For warning in ryml_all.hpp:
		-- warning C4996: 'std::aligned_storage': warning STL4034: std::aligned_storage and std::aligned_storage_t are deprecated in C++23. Prefer alignas(T) std::byte t_buff[sizeof(T)].
		defines {"_SILENCE_CXX23_ALIGNED_STORAGE_DEPRECATION_WARNING"}
				
		removefiles {"./source/core/sl.extra/extra.cpp"}
		
		libdirs {externaldir .."vulkan/Lib"}

		links { "d3d12.lib", "vulkan-1.lib"}
end

group ""
