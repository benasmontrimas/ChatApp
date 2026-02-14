workspace "ChatApp"
   configurations { "Debug", "Development", "Release" }
   platforms { "Windows", "Linux" }
   architecture "x64"

project "ChatApp"
   kind "ConsoleApp"
   language "C++"
   cppdialect "C++23"
   location "Build/"

   targetdir "Bin/%{cfg.buildcfg}"

   externalincludedirs { "External/imgui", "External/imgui/backends", "External/FMOD/inc" }
   includedirs { "Source/" }

   -- enableunitybuild "On"
   externalwarnings "Off"
   defines { "IMGUI_DEFINE_MATH_OPERATORS" }

   links { "d3d12.lib", "d3dcompiler.lib", "dxgi.lib", "External/FMOD/lib/x64/fmod_vc.lib" }

   files { "Source/**.h", "Source/**.cpp", "External/imgui/backends/imgui_impl_dx12.cpp", "External/imgui/backends/imgui_impl_win32.cpp", "External/imgui/imgui*.cpp" }

   filter "platforms:Windows"
      defines { "WINDOWS" }
      system ("windows")

   filter "platforms:Linux"
      defines { "LINUX" }
      system ("linux")

   filter "configurations:Debug"
      defines { "DEBUG" }
      symbols "On"
      links { "dxguid" }

   filter "configurations:Development"
      defines { "DEBUG" }
      symbols "On"
      optimize "Debug"
      links { "dxguid" }

   filter "configurations:Release"
      -- kind "WindowedApp"
      defines { "RELEASE" }
      optimize "On"