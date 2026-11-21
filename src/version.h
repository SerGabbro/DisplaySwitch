// Single source of truth for the version: used by the code ("version" command)
// and by the resource file (Windows file properties). Bump it here only.
#pragma once

#define DS_VERSION_MAJOR 1
#define DS_VERSION_MINOR 0
#define DS_VERSION_PATCH 0

#define DS_STR2(x) #x
#define DS_STR(x) DS_STR2(x)
#define DS_VERSION_STRING DS_STR(DS_VERSION_MAJOR) "." DS_STR(DS_VERSION_MINOR) "." DS_STR(DS_VERSION_PATCH)

#define DS_PRODUCT_NAME "DisplaySwitch"
#define DS_DESCRIPTION  "Switch Windows display layouts between profiles"
#define DS_COPYRIGHT    "Copyright (c) 2026 DisplaySwitch contributors. MIT License."
