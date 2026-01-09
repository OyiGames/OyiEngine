#pragma once

#if defined(_WIN32)
  #if defined(OYI_GRAPHIC_BUILD_DLL)
    #define OYI_GRAPHIC_API __declspec(dllexport)
  #else
    #define OYI_GRAPHIC_API __declspec(dllimport)
  #endif
#else
  #define OYI_GRAPHIC_API
#endif