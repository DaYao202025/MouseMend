#pragma once

// Version information (single source of truth: included by MouseMend.rc as well)

#define APP_VERSION_MAJOR 2
#define APP_VERSION_MINOR 1
#define APP_VERSION_PATCH 2

#define APP_VERSION_STRING "2.1.2"

// Wide variant, kept in sync by hand because C cannot widen a string literal.
// tests/test_core.c asserts that both spell the same version.
#define APP_VERSION_STRING_W L"2.1.2"

#define APP_NAME "MouseMend"
#define APP_NAME_W L"MouseMend"

// Shown in the properties of the executable
#define APP_COMPANY "梦创"
#define APP_COPYRIGHT "By_yao (C) 2026"

// The project this one was started from. Everything else about the original
// author was removed on purpose: the suppression engine, the check-up window
// and the tray behaviour were rewritten here, and the remaining shared code is
// the low level hook plumbing. The link stays because "where did this come
// from" is a fair question with a short answer.
#define APP_ORIGIN_URL "https://gitee.com/Matreshka15/MouseFix"
#define APP_ORIGIN_URL_W L"https://gitee.com/Matreshka15/MouseFix"
