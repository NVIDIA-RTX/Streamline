:: package.bat
:: Collages Streamline components into a package for distribution and integration.
:: Usage: package.bat [args...]
::   Args:
::      -debug                Package using Debug binaries
::      -develop              Package using Develop binaries
::      -production           Package using Production binaries (also includes Develop binaries in the development/ folder)
::      -source               Include source in the final package
::      -sourceonly           Package only sources (no binaries)
::      -dir [output_path]    Creates the package in output_path. Defaults to .\_sdk
::      -arm64                Package arm64/aarch64 (default is x64)

@SETLOCAL EnableDelayedExpansion

@ECHO off

set cfg=UNKNOWN
set cfg_alt=None
set src=%~dp0
set artifacts_src=%src%\_artifacts
set dest=%~dp0\_sdk
set include_source=False
set include_binaries=True
:: When packaging with source, the features dir is redundant with bin, but better reflects the original repo
set create_features_dir=False

set arch_vs=x64
set arch_vs_ex=x86_64
:: Currently only used for features/ subdir and x64/amd64 binaries aren't in a subdir...
set arch_nvmake=

:argloop
IF NOT "%1"=="" (
    IF "%1"=="-debug" (
        set cfg=Debug
    )
    IF "%1"=="-develop" (
        set cfg=Develop
    )
    IF "%1"=="-production" (
        set cfg=Production
        set cfg_alt=Develop
    )
    :: Deprecated build configuration names
    IF "%1"=="-release" (
        set cfg=Debug
    )
    IF "%1"=="-profiling" (
        set cfg=Develop
    )
    IF "%1"=="-relextdev" (
        set cfg=Develop
    )

    IF "%1"=="-source" (
        set include_source=True
    )
    if "%1"=="-sourceonly" (
        set include_source=True
        set include_binaries=False
    )
    IF "%1"=="-dir" (
        set dest=%~f2
        shift
    )
    IF "%1"=="-root" (
        set src=%~f2
        set artifacts_src=%~f2\_artifacts
        shift
    )
    IF "%1"=="-artifacts-src" (
        set artifacts_src=%~f2
        shift
    )
    IF "%1"=="-arm64" (
        set arch_vs=arm64
        set arch_vs_ex=aarch64
        set arch_nvmake=aarch64
    )
    shift
    goto :argloop
)

IF "%cfg%"=="UNKNOWN" (
    echo "Could not determine build type"
    echo "Use one of -debug, -develop, or -production"
    exit /b 1
)

IF EXIST %dest%\ rmdir %dest% /S /Q
mkdir %dest%

:: BINARIES AND SYMBOLS
:: copybin [copy_cfg] [copy_dest] [sym_dest]
:: Copies binaries for copy_cfg to copy_dest, and symbols to symbol_dest
:: Includes json and licenses where relevant
goto endcopybin
:copybin

set copy_cfg=%~1
set copy_dest=%~2
set sym_dest=%~3

IF EXIST %src%/features (
    set features_src=%src%\features\%arch_nvmake%
    :: Licenses are shared by all architectures in the features root.
    set licenses_src=%src%\features
) ELSE (
    set features_src=%src%\bin\%arch_vs%
    set licenses_src=%src%\bin\%arch_vs%
)

:: Common Plugins
copy %artifacts_src%\sl.common\%copy_cfg%_%arch_vs%\sl.common.dll %copy_dest% /Y
copy %artifacts_src%\sl.common\%copy_cfg%_%arch_vs%\sl.common.pdb %sym_dest% /Y
copy %artifacts_src%\sl.interposer\%copy_cfg%_%arch_vs%\sl.interposer.dll %copy_dest% /Y
copy %artifacts_src%\sl.interposer\%copy_cfg%_%arch_vs%\sl.interposer.pdb %sym_dest% /Y
IF NOT "%copy_cfg%"=="Production" (
    copy %artifacts_src%\sl.imgui\%copy_cfg%_%arch_vs%\sl.imgui.dll %copy_dest% /Y
    copy %artifacts_src%\sl.imgui\%copy_cfg%_%arch_vs%\sl.imgui.pdb %sym_dest% /Y
)

:: DLSS Super Resolution
copy %artifacts_src%\sl.dlss\%copy_cfg%_%arch_vs%\sl.dlss.dll %copy_dest% /Y
copy %artifacts_src%\sl.dlss\%copy_cfg%_%arch_vs%\sl.dlss.pdb %sym_dest% /Y

copy %licenses_src%\nvngx_dlss.license.txt %copy_dest% /Y

IF "%copy_cfg%"=="Production" (
    copy %features_src%\nvngx_dlss.dll %copy_dest% /Y
) ELSE (
    copy %features_src%\development\nvngx_dlss.dll %copy_dest% /Y
)

:: DLSS Frame Generation
IF "%copy_cfg%"=="Production" (
    copy %features_src%\sl.dlss_g.dll %copy_dest% /Y
    copy %artifacts_src%\sl.dlss_g\%copy_cfg%_%arch_vs%\sl.dlss_g.dll %copy_dest% /Y
) ELSE (
    copy %features_src%\development\sl.dlss_g.dll %copy_dest% /Y
    copy %artifacts_src%\sl.dlss_g\Develop_%arch_vs%\sl.dlss_g.dll %copy_dest% /Y
)

IF "%copy_cfg%"=="Production" (
    copy %features_src%\nvngx_dlssg.dll %copy_dest% /Y
) ELSE (
    copy %features_src%\development\nvngx_dlssg.dll %copy_dest% /Y
)

:: DLSS Ray Reconstruction
copy %artifacts_src%\sl.dlss_d\%copy_cfg%_%arch_vs%\sl.dlss_d.dll %copy_dest% /Y
copy %artifacts_src%\sl.dlss_d\%copy_cfg%_%arch_vs%\sl.dlss_d.pdb %sym_dest% /Y

IF "%copy_cfg%"=="Production" (
    copy %features_src%\nvngx_dlssd.dll %copy_dest% /Y
) ELSE (
    copy %features_src%\development\nvngx_dlssd.dll %copy_dest% /Y
)

:: DeepDVC
copy %artifacts_src%\sl.deepdvc\%copy_cfg%_%arch_vs%\sl.deepdvc.dll %copy_dest% /Y
copy %artifacts_src%\sl.deepdvc\%copy_cfg%_%arch_vs%\sl.deepdvc.pdb %sym_dest% /Y

IF "%copy_cfg%"=="Production" (
    copy %features_src%\nvngx_deepdvc.dll %copy_dest% /Y
) ELSE (
    copy %features_src%\development\nvngx_deepdvc.dll %copy_dest% /Y
)



:: NIS
copy %artifacts_src%\sl.nis\%copy_cfg%_%arch_vs%\sl.nis.dll %copy_dest% /Y
copy %artifacts_src%\sl.nis\%copy_cfg%_%arch_vs%\sl.nis.pdb %sym_dest% /Y

copy %licenses_src%\nis.license.txt %copy_dest% /Y

:: NvPerf
copy %artifacts_src%\sl.nvperf\%copy_cfg%_%arch_vs%\sl.nvperf.dll %copy_dest% /Y
copy %artifacts_src%\sl.nvperf\%copy_cfg%_%arch_vs%\sl.nvperf.pdb %sym_dest% /Y

:: PCL
copy %artifacts_src%\sl.pcl\%copy_cfg%_%arch_vs%\sl.pcl.dll %copy_dest% /Y
copy %artifacts_src%\sl.pcl\%copy_cfg%_%arch_vs%\sl.pcl.pdb %sym_dest% /Y

:: DirectSR
copy %artifacts_src%\sl.directsr\%copy_cfg%_%arch_vs%\sl.directsr.dll %copy_dest% /Y
copy %artifacts_src%\sl.directsr\%copy_cfg%_%arch_vs%\sl.directsr.pdb %sym_dest% /Y

:: Reflex
copy %artifacts_src%\sl.reflex\%copy_cfg%_%arch_vs%\sl.reflex.dll %copy_dest% /Y
copy %artifacts_src%\sl.reflex\%copy_cfg%_%arch_vs%\sl.reflex.pdb %sym_dest% /Y

copy %licenses_src%\reflex.license.txt %copy_dest% /Y

IF "%arch_vs%"=="x64" (
copy %src%\external\reflex-sdk-vk\lib\NvLowLatencyVk.dll %copy_dest% /Y

) ELSE (
    :: Don't want to have to build/support NvLowLatencyVk futher, so there's no arm64ec build.  Use x64.
    IF "%arch_nvmake%"=="arm64ec" (
        copy %src%\external\reflex-sdk-vk\lib\NvLowLatencyVk.dll %copy_dest% /Y
    )
)


:: Profiling Binary
IF NOT "%copy_cfg%"=="Production" (
    copy %src%\external\pix\bin\WinPixEventRuntime.dll %copy_dest% /Y
    copy %src%\external\pix\PACKAGE-LICENSES\winpixeventruntime-LICENSE.md %copy_dest% /Y
)

exit /b 0
:endcopybin


IF "%include_binaries%"=="True" (
    mkdir %dest%\lib\%arch_vs%
    mkdir %dest%\bin\%arch_vs%
    mkdir %dest%\symbols

    IF "%cfg_alt%"=="None" (
        call:copybin %cfg% %dest%\bin\%arch_vs% %dest%\symbols
    ) ELSE (
        mkdir %dest%\bin\%arch_vs%\development

        call:copybin %cfg% %dest%\bin\%arch_vs% NUL
        call:copybin %cfg_alt% %dest%\bin\%arch_vs%\development %dest%\symbols
    )

    :: Interposer lib
    copy %artifacts_src%\sl.interposer\%cfg%_%arch_vs%\sl.interposer.lib %dest%\lib\%arch_vs%\ /Y
)

:: INCLUDES
mkdir %dest%\include

copy %src%\include\sl.h                 %dest%\include
copy %src%\include\sl_appidentity.h     %dest%\include
copy %src%\include\sl_consts.h          %dest%\include
copy %src%\include\sl_core_api.h        %dest%\include
copy %src%\include\sl_core_types.h      %dest%\include
copy %src%\include\sl_device_wrappers.h %dest%\include
copy %src%\include\sl_helpers.h         %dest%\include
copy %src%\include\sl_helpers_vk.h      %dest%\include
copy %src%\include\sl_hooks.h           %dest%\include
copy %src%\include\sl_matrix_helpers.h  %dest%\include
copy %src%\include\sl_result.h          %dest%\include
copy %src%\include\sl_security.h        %dest%\include
copy %src%\include\sl_struct.h          %dest%\include
copy %src%\include\sl_version.h         %dest%\include


copy %src%\include\sl_deepdvc.h         %dest%\include
copy %src%\include\sl_dlss.h            %dest%\include
copy %src%\include\sl_dlss_d.h          %dest%\include
copy %src%\include\sl_dlss_g.h          %dest%\include
copy %src%\include\sl_nis.h             %dest%\include
copy %src%\include\sl_nvperf.h          %dest%\include
copy %src%\include\sl_pcl.h             %dest%\include
copy %src%\include\sl_directsr.h             %dest%\include
copy %src%\include\sl_reflex.h          %dest%\include
copy %src%\include\sl_template.h        %dest%\include


:: SCRIPTS
mkdir %dest%\scripts

copy %src%\scripts\sl.common.json                       %dest%\scripts
copy %src%\scripts\sl.interposer.json                   %dest%\scripts
copy %src%\scripts\sl.reflex.json                       %dest%\scripts
copy %src%\scripts\sl.imgui.json                        %dest%\scripts
copy %src%\scripts\streamline_logging_disable.reg       %dest%\scripts
copy %src%\scripts\streamline_logging_enable.reg        %dest%\scripts
copy %src%\scripts\ngx_driver_onscreenindicator.reg     %dest%\scripts
copy %src%\scripts\ngx_driver_onscreenindicator_off.reg %dest%\scripts

copy %src%\scripts\sl.cmake %dest%\CMakeLists.txt


:: UTILITIES
IF "%include_binaries%"=="True" (
    mkdir %dest%\utils
    mkdir %dest%\utils\reflex

    xcopy %src%\utils\reflex %dest%\utils\reflex /S
)

:: DOCUMENTATION
mkdir %dest%\docs
mkdir %dest%\docs\media

copy %src%\docs\ProgrammingGuide.md              %dest%\docs
copy %src%\docs\ProgrammingGuideDeepDVC.md       %dest%\docs
copy %src%\docs\ProgrammingGuideDirectSR.md      %dest%\docs
copy %src%\docs\ProgrammingGuideDLSS.md          %dest%\docs
copy %src%\docs\ProgrammingGuideDLSS_G.md        %dest%\docs
copy %src%\docs\media\dlssg*.png %dest%\docs\media
copy %src%\docs\ProgrammingGuideDLSS_RR.md       %dest%\docs
copy %src%\docs\ProgrammingGuideManualHooking.md %dest%\docs
copy %src%\docs\ProgrammingGuideNIS.md           %dest%\docs
copy %src%\docs\ProgrammingGuidePCL.md           %dest%\docs
copy %src%\docs\ProgrammingGuideReflex.md        %dest%\docs

copy %src%\docs\APIChangesAndImprovements.md     %dest%\docs

copy "%src%\docs\RTX Developer Localization Strings.zip"             %dest%\docs
copy "%src%\docs\RTX UI Developer Guidelines Chinese Version.pdf"    %dest%\docs
copy "%src%\docs\RTX UI Developer Guidelines.pdf"                    %dest%\docs
copy "%src%\docs\Debugging - JSON Configs (Plugin Configs).md"       %dest%\docs
copy "%src%\docs\Debugging - NvPerf GUI.md"                          %dest%\docs
copy "%src%\docs\Debugging - SL ImGUI (Realtime Data Inspection).md" %dest%\docs
copy %src%\docs\media\nvperf*.png %dest%\docs\media
copy %src%\docs\media\sl_imgui*.png %dest%\docs\media
copy %src%\docs\media\Validation.png %dest%\docs\media

copy %src%\docs\Streamline*.pdf %dest%\docs

copy "%src%\docs\DLSS-FG Programming Guide.pdf" %dest%\docs
copy "%src%\docs\DLSS-RR Integration Guide.pdf" %dest%\docs
copy "%src%\docs\DLSS Programming Guide.pdf" %dest%\docs

:: Changelog
copy %src%\docs\changelog.txt %dest% /Y

:: README AND LICENSES
copy %src%\README.md             %dest% /Y
:: Contribution docs are for the public GitHub repo only.
copy %src%\CONTRIBUTING.md       %dest% /Y
copy %src%\CLA.md                %dest% /Y
copy %src%\license.txt           %dest% /Y
copy %src%\"NVIDIA Nsight Perf SDK License (28Sept2022).pdf" %dest% /Y
copy %src%\"NVIDIA Nsight Graphics SDK License (Apache 2.0).txt" %dest% /Y
copy %src%\3rd-party-licenses.md %dest% /Y

:: SOURCE
IF "%include_source%"=="True" (
    mkdir %dest%\shaders
    mkdir %dest%\source
    mkdir %dest%\source\core
    mkdir %dest%\source\platforms
    mkdir %dest%\source\shared
    mkdir %dest%\source\plugins
    mkdir %dest%\tools
    mkdir %dest%\tools\packman
    mkdir %dest%\_artifacts
    mkdir %dest%\_artifacts\shaders

    :: Tools
    copy %src%\tools\build_shader.bat %dest%\tools
    copy %src%\tools\gitVersion.bat   %dest%\tools
    copy %src%\tools\vswhere.exe      %dest%\tools
    copy %src%\tools\bin2cheader.ps1  %dest%\tools
    xcopy %src%\tools\packman         %dest%\tools\packman /S

    :: Additional scripts
    copy %src%\scripts\_manifest.lua %dest%\scripts
    copy %src%\scripts\_preload.lua  %dest%\scripts
    copy %src%\scripts\sl.cmake      %dest%\scripts

    :: Build scripts
    copy %src%\premake.lua   %dest%
    copy %src%\build.bat     %dest%
    copy %src%\build_all.bat %dest%
    copy %src%\setup.bat     %dest%
    copy %src%\project.xml   %dest%
    copy %src%\package.bat   %dest%

    :: Agent skills
    mkdir "%dest%\.agents"
    mkdir "%dest%\.agents\plugins"
    mkdir "%dest%\.claude-plugin"
    mkdir "%dest%\plugins"
    mkdir "%dest%\plugins\streamline-agent-skills"
    mkdir "%dest%\plugins\streamline-agent-skills\skills"

    copy "%src%\.agents\plugins\marketplace.json" "%dest%\.agents\plugins"
    copy "%src%\.claude-plugin\marketplace.json" "%dest%\.claude-plugin"
    copy "%src%\plugins\streamline-agent-skills\AGENTS.md" "%dest%\plugins\streamline-agent-skills"
    copy "%src%\plugins\streamline-agent-skills\CLAUDE.md" "%dest%\plugins\streamline-agent-skills"
    copy "%src%\plugins\streamline-agent-skills\plugin-manifest-source.json" "%dest%\plugins\streamline-agent-skills"
    xcopy "%src%\plugins\streamline-agent-skills\skills\authoring-dlss-agent-skills" "%dest%\plugins\streamline-agent-skills\skills\authoring-dlss-agent-skills" /S /I /Y
    xcopy "%src%\plugins\streamline-agent-skills\skills\integrate-dlss-fg" "%dest%\plugins\streamline-agent-skills\skills\integrate-dlss-fg" /S /I /Y
    xcopy "%src%\plugins\streamline-agent-skills\skills\integrate-dlss-rr" "%dest%\plugins\streamline-agent-skills\skills\integrate-dlss-rr" /S /I /Y
    xcopy "%src%\plugins\streamline-agent-skills\skills\integrate-dlss-sr" "%dest%\plugins\streamline-agent-skills\skills\integrate-dlss-sr" /S /I /Y
    xcopy "%src%\plugins\streamline-agent-skills\skills\integrate-reflex-pcl" "%dest%\plugins\streamline-agent-skills\skills\integrate-reflex-pcl" /S /I /Y

    :: Common Source
    xcopy %src%\source\core      %dest%\source\core /S
    mkdir %dest%\source\platforms\sl.chi
    xcopy %src%\source\platforms\sl.chi %dest%\source\platforms\sl.chi /S
    xcopy %src%\source\shared    %dest%\source\shared /S

    :: Plugins
    mkdir %dest%\source\plugins\sl.common
    xcopy %src%\source\plugins\sl.common   %dest%\source\plugins\sl.common   /S
    mkdir %dest%\source\plugins\sl.deepdvc
    xcopy %src%\source\plugins\sl.deepdvc  %dest%\source\plugins\sl.deepdvc  /S
    mkdir %dest%\source\plugins\sl.directsr
    xcopy %src%\source\plugins\sl.directsr %dest%\source\plugins\sl.directsr /S

    mkdir %dest%\external\dx-agility-sdk-headers-1.714.0-preview\
    xcopy %src%\external\dx-agility-sdk-headers-1.714.0-preview\ %dest%\external\dx-agility-sdk-headers-1.714.0-preview /S
    mkdir %dest%\source\plugins\sl.dlss
    xcopy %src%\source\plugins\sl.dlss     %dest%\source\plugins\sl.dlss     /S
    mkdir %dest%\source\plugins\sl.dlss_d
    xcopy %src%\source\plugins\sl.dlss_d   %dest%\source\plugins\sl.dlss_d   /S
    mkdir %dest%\source\plugins\sl.imgui
    xcopy %src%\source\plugins\sl.imgui    %dest%\source\plugins\sl.imgui    /S
    mkdir %dest%\source\plugins\sl.nis
    xcopy %src%\source\plugins\sl.nis      %dest%\source\plugins\sl.nis      /S
    mkdir %dest%\source\plugins\sl.pcl
    xcopy %src%\source\plugins\sl.pcl      %dest%\source\plugins\sl.pcl      /S
    mkdir %dest%\source\plugins\sl.reflex
    xcopy %src%\source\plugins\sl.reflex   %dest%\source\plugins\sl.reflex   /S
    mkdir %dest%\source\plugins\sl.template
    xcopy %src%\source\plugins\sl.template %dest%\source\plugins\sl.template /S

    :: Git metadata files
    copy %src%\.gitignore %dest%\.gitignore
    copy %src%\.gitattributes %dest%\

    :: External Dependencies
    mkdir %dest%\external
    mkdir %dest%\external\json
    mkdir %dest%\external\json\include
    mkdir %dest%\external\ngx-sdk
    mkdir %dest%\external\ngx-sdk\include
    mkdir %dest%\external\ngx-sdk\lib
    mkdir %dest%\external\ngx-sdk\lib\Windows_%arch_vs_ex%
    mkdir %dest%\external\nvapi

    xcopy %src%\external\json\include                          %dest%\external\json\include /S
    copy %src%\external\json\LICENSE.MIT                       %dest%\external\json
    copy %src%\external\json\nlohmann_json.natvis              %dest%\external\json
    copy %src%\external\ngx-sdk\include\nvsdk_ngx_defs.h       %dest%\external\ngx-sdk\include
    copy %src%\external\ngx-sdk\include\nvsdk_ngx_defs_vk.h   %dest%\external\ngx-sdk\include
    copy %src%\external\ngx-sdk\include\nvsdk_ngx_helpers.h    %dest%\external\ngx-sdk\include
    copy %src%\external\ngx-sdk\include\nvsdk_ngx_helpers_d3d.h %dest%\external\ngx-sdk\include
    copy %src%\external\ngx-sdk\include\nvsdk_ngx_helpers_cuda.h %dest%\external\ngx-sdk\include
    copy %src%\external\ngx-sdk\include\nvsdk_ngx_helpers_vk.h %dest%\external\ngx-sdk\include
    copy %src%\external\ngx-sdk\include\nvsdk_ngx_params.h     %dest%\external\ngx-sdk\include
    copy %src%\external\ngx-sdk\include\nvsdk_ngx.h            %dest%\external\ngx-sdk\include
    copy %src%\external\ngx-sdk\include\nvsdk_ngx_vk.h         %dest%\external\ngx-sdk\include
    copy %src%\external\ngx-sdk\include\nvsdk_ngx_defs_dlssd.h       %dest%\external\ngx-sdk\include
    copy %src%\external\ngx-sdk\include\nvsdk_ngx_helpers_dlssd.h     %dest%\external\ngx-sdk\include
    copy %src%\external\ngx-sdk\include\nvsdk_ngx_helpers_dlssd_d3d.h %dest%\external\ngx-sdk\include
    copy %src%\external\ngx-sdk\include\nvsdk_ngx_helpers_dlssd_vk.h  %dest%\external\ngx-sdk\include
    copy %src%\external\ngx-sdk\include\nvsdk_ngx_params_dlssd.h     %dest%\external\ngx-sdk\include
    copy %src%\external\ngx-sdk\include\nvsdk_ngx_defs_deepdvc.h       %dest%\external\ngx-sdk\include
    copy %src%\external\ngx-sdk\include\nvsdk_ngx_helpers_deepdvc.h    %dest%\external\ngx-sdk\include
    copy %src%\external\ngx-sdk\include\nvsdk_ngx_helpers_deepdvc_vk.h %dest%\external\ngx-sdk\include

    xcopy %src%\external\ngx-sdk\lib\Windows_%arch_vs_ex%                    %dest%\external\ngx-sdk\lib\Windows_%arch_vs_ex% /S

    copy %src%\external\nvapi\*.c %dest%\external\nvapi\ /Y
    copy %src%\external\nvapi\*.h %dest%\external\nvapi\ /Y
    mkdir %dest%\external\nvapi\amd64
    copy %src%\external\nvapi\amd64\nvapi64.lib %dest%\external\nvapi\amd64\nvapi64.lib
    mkdir %dest%\external\nvapi\aarch64
    copy %src%\external\nvapi\aarch64\nvapia64.lib %dest%\external\nvapi\aarch64\nvapia64.lib

    :: Copy RTX SDKs EULA for NGX SDK bits
    copy %src%\features\nvngx_dlss.license.txt %dest%\external\ngx-sdk\license.txt

    mkdir %dest%\external\reflex-sdk-vk
    xcopy %src%\external\reflex-sdk-vk\ %dest%\external\reflex-sdk-vk /S
    copy %src%\features\reflex.license.txt %dest%\external\reflex-sdk-vk /Y

    mkdir %dest%\external\nsight-sdk
    xcopy %src%\external\nsight-sdk\ %dest%\external\nsight-sdk /S

    :: Shader Source
    copy %src%\shaders\copy.hlsl                       %dest%\shaders
    copy %src%\shaders\copy_to_buffer.hlsl             %dest%\shaders
    copy %src%\shaders\copy_to_buffer_cs.h             %dest%\shaders
    copy %src%\shaders\copy_to_buffer_spv.h            %dest%\shaders
    copy %src%\shaders\mvec.hlsl                       %dest%\shaders
    copy %src%\shaders\vulkan_clear_image_view.comp    %dest%\shaders
    copy %src%\shaders\vulkan_clear_image_view_spirv.h %dest%\shaders
    :: Compiled Shaders
    copy %artifacts_src%\shaders\copy_cs.h            %dest%\_artifacts\shaders
    copy %artifacts_src%\shaders\copy_spv.h           %dest%\_artifacts\shaders
    copy %artifacts_src%\shaders\copy_to_buffer_cs.h  %dest%\_artifacts\shaders
    copy %artifacts_src%\shaders\copy_to_buffer_spv.h %dest%\_artifacts\shaders
    copy %artifacts_src%\shaders\mvec_cs.h            %dest%\_artifacts\shaders
    copy %artifacts_src%\shaders\mvec_spv.h           %dest%\_artifacts\shaders
    :: Feature DLLs
    IF "%create_features_dir%"=="True" (
        mkdir %dest%\features
        mkdir %dest%\features\development

        copy %src%\features\nvngx_dlss.dll %dest%\features
        copy %src%\features\development\nvngx_dlss.dll %dest%\features\development

        copy %src%\features\nvngx_dlssg.dll %dest%\features
        copy %src%\features\development\nvngx_dlssg.dll %dest%\features\development

        copy %src%\features\nvngx_dlssd.dll %dest%\features
        copy %src%\features\development\nvngx_dlssd.dll %dest%\features\development

        copy %src%\features\nvngx_deepdvc.dll %dest%\features
        copy %src%\features\development\nvngx_deepdvc.dll %dest%\features\development

        :: Feature Licenses
        copy %src%\features\nvngx_dlss.license.txt %dest%\features\nvngx_dlss.license.txt
        copy %src%\features\nis.license.txt %dest%\features\nis.license.txt
        copy %src%\features\reflex.license.txt %dest%\features\reflex.license.txt
    )

    :: DLSS-G Plugin
    IF "%include_binaries%"=="True" (
        mkdir %dest%\_artifacts\sl.dlss_g
        mkdir %dest%\_artifacts\sl.dlss_g\Production_%arch_vs%
        mkdir %dest%\_artifacts\sl.dlss_g\Develop_%arch_vs%
        copy %artifacts_src%\sl.dlss_g\Production_%arch_vs%\sl.dlss_g.dll %dest%\_artifacts\sl.dlss_g\Production_%arch_vs%
        copy %artifacts_src%\sl.dlss_g\Develop_%arch_vs%\sl.dlss_g.dll %dest%\_artifacts\sl.dlss_g\Develop_%arch_vs%
    )


)
