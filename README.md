# Streamline (SL) - Version 2.14.16

Streamline is an open-sourced cross-IHV solution that simplifies integration of the latest NVIDIA and other independent hardware vendors' super resolution technologies into applications and games. This framework allows developers to easily implement one single integration and enable multiple super-resolution technologies and other graphics effects supported by the hardware vendor.

This repo contains the SDK for integrating Streamline into your application.

For a high level overview, see the [NVIDIA Developer Streamline page](https://developer.nvidia.com/rtx/streamline)

As of SL 2.0.0, it is now possible to recompile all of SL from source, with the exception of the DLSS-G plugin.  The DLSS-G plugin is provided as prebuilt DLLs only.  We also provide prebuilt DLLs (signed) for all other plugins that have source.  Most application developers will never need to rebuild SL themselves, especially for shipping; all of the pieces needed to develop, debug and ship an application that integrates SL and its features are provided in the pre-existing directories `bin/`, `include/`, and `lib/`.  Compiling from source is purely optional and likely of interest to a subset of SL application developers. For developers wishing to build SL from source, see the following sections.

As of SL 2.7.32, binary artifacts (DLSS feature DLLs and Streamline DLLs) are not in the GitHub repository. In order to obtain these artifacts, please download the appropriate release zip from the [GitHub releases page](https://github.com/NVIDIA-RTX/Streamline/releases).

------

## Prerequisites

#### Hardware

- GPU supporting DirectX 11 and Vulkan 1.2 or higher

#### Windows

- Win10 20H1 (version 2004 - 10.0.19041) or newer
- Install latest graphics driver (**if using NVIDIA GPU it MUST be 512.15 or newer**)
- Install VS Code or VS2019+ with [SDK 10.0.19041+](https://go.microsoft.com/fwlink/?linkid=2120843)
- Install "git".
- Clone your fork to a local hard drive, make sure to use a NTFS drive on Windows (SL uses symbolic links)

## BUILDING SL FROM SOURCE
------------------------------------------------

As mentioned in the lead section of this document, SL now ships with most of its source code available, which allows developers who wish to build most of SL from source to do so locally.  The sole exception is the DLSS-G plugin, which is only available procompiled.

> **IMPORTANT:**
> Only use `production` builds when releasing your software.  Also, use either the original NVIDIA-signed SL DLLs or implement your own signing system (and check for that signature in SL), otherwise SL plugins could be replaced with potentially malicious modules.

#### Configuring and Building a Tree

All of SL's projects and build information (and those of all SL based apps) are controlled through a single platform independent build script called `premake5.lua`.  This is located in the root of the SL tree and uses the `premake` project creation toolchain.  Any new projects or changes to existing projects must be listed in this file.  For most projects, all source and header files found within a project's directory will be automatically added to that project.

To configure a new SL tree to build, there is a script called `setup.bat`.  Note that on Windows this must be run from either a Windows command prompt window or a PowerShell window.  

Running the `setup.bat` script will cause two things to be done:

1. Use the NVIDIA tool `packman` to pull all build dependencies to the local machine and cache them in a shared directory.  Links are created from `external` in the SL tree to this shared cache for external build dependencies.
2. Run `premake` to generate the project build files in `_project\vs2019` (for Windows)

To build the project, simply open `_project\vs2019\streamline.sln` in Visual Studio, select the desired build configuration and build, or else use the provided build script:

`./build.bat` with `-{debug|develop|production}` (`debug` is default) and optional architecture `{-amd64|-arm64|-arm64ec}` (`amd64` is default), or use VS IDE and load solution from the `_project` directory

The default setting is to target x86_64 CPU architecture (`amd64`).

> NOTE: To build the project minimal configuration is needed. Any version of Windows 10 will do. Then
run the setup and build scripts as described here above. That's it. The specific version of Windows, NVIDIA driver,
or Vulkan are all runtime dependencies, not compile/link time dependencies. This allows SL to build on stock
virtual machines that require zero configuration. This is a beautiful thing, help us keep it that way.

#### Changing an Existing Project

Do not edit the MSVC project files (or Makefiles on other platforms) directly!  Always modify the `premake5.lua` described above.

When changing an existing project's settings or contents (ie: adding a new source file, changing a compiler setting, linking to a new library, etc), it is necessary to run `setup.bat` again for those changes to take effect and MSVC project files and or solution will need to be reloaded in the IDE.

NVIDIA does not recommend making changes to the headers in `include`, as these can affect the API itself and can make developer-built components incompatible with NVIDIA-supplied components.

#### Using the results of local builds

Once the project is built for a configuration, the built, unsigned DLLs may be found in `_artifacts\sl.*\<Config>\`.  These DLLs can be copied as desired into the `bin\x64` directory, or packaged for use in the application itself.

Obviously, `sl.dlss_g.dll` cannot be built from source and thus the prebuilt copy must be used.

#### (Optional) Compiling Shaders

If you would like to recompile the shaders for the NIS plugin, you will need to have Python 3 installed and in the path.

## SDK Packaging

- Execute `./package.bat` with `-{debug|develop|production}` (`production` is default)

The packaged SDK can be found in the generated `_sdk` folder.

## Debugging

Streamline offers several ways to debug and troubleshoot issues. Please see the following pages for more information.
* Using SL ImGui: [Debugging - SL ImGUI (Realtime Data Inspection).md](<docs/Debugging - SL ImGUI (Realtime Data Inspection).md>)
* Using JSON configuration files: [Debugging - JSON Configs (Plugin Configs).md](<docs/Debugging - JSON Configs (Plugin Configs).md>)

### Logging

Starting with Streamline 2.7.x, it is possible to forcibly override logging settings such as log level, log file output path/name and whether to log to a separate console window.

There are two ways to override the log settings: via the Windows registry, or using environment variables. Environment variables take precedence over the Windows registry (i.e. if you have both a registry override and an environment variable override in effect, the environment variable's override value will be used).

The log path setting refers to a writeable directory on the filesystem where Streamline can place its log file (called `sl.log` by default). Change the log name setting to change the name of this log file.

The logging override keys live in the `HKEY_LOCAL_MACHINE\SOFTWARE\NVIDIA Corporation\Global\Streamline` subkey.

| Description                                       | Registry Value         | Registry Value Type | Environment Variable        |  Valid Data |
| ------------------------------------------------- | ---------------------- | ------------------- | --------------------------- |  ---------- |
| Enable console logging to separate console window | `EnableConsoleLogging` | DWORD               | `SL_ENABLE_CONSOLE_LOGGING` |  [0-1]      |
| Log level                                         | `LogLevel`             | DWORD               | `SL_LOG_LEVEL`              |  [0-2]      |
| Output directory for log file                     | `LogPath`              | String              | `SL_LOG_PATH`               |  Any string |
| Filename to log to                                | `LogName`              | String              | `SL_LOG_NAME`               |  Any string |

For convenience, there are also two registry files in the Streamline repo that can quickly enable or disable logging, called `streamline_logging_disable.reg` and `streamline_logging_enable.reg`.

> **NOTE:**
> The Streamline interposer configuration (JSON) takes precedence over these logging overrides.

## General Programming Guide

Please read [ProgrammingGuide.md](docs/ProgrammingGuide.md) to learn about the integration in games.

## Advanced Programming Guide - Manual Hooking With Lowest Overhead

Please read [ProgrammingGuideManualHooking.md](docs/ProgrammingGuideManualHooking.md) to learn about advanced SL integration in games.

## Programming Guides Per Feature:

- [DLSS Super Resolution](docs/ProgrammingGuideDLSS.md)
- [DLSS Frame Generation](docs/ProgrammingGuideDLSS_G.md)
- [Reflex](docs/ProgrammingGuideReflex.md)
- [NIS](docs/ProgrammingGuideNIS.md)

## Sample Plugin Source Code

A sample Streamline plugin source code is located [here](source/plugins/sl.template/templateEntry.cpp)

## Sample App and Source

A sample application using Streamline may be found in [this git repo](https://github.com/NVIDIA-RTX/Streamline_Sample)

## Contributing

For information on reporting issues and contributing changes, see [CONTRIBUTING.md](CONTRIBUTING.md).
