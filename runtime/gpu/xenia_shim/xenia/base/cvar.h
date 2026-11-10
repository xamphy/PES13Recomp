// Shim for Xenia's cvar system (which needs cpptoml/cxxopts): every cvar is a plain global
// in namespace cvars holding its default value. The runtime may assign them directly.
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>

#define XE_SHIM_CVAR_DECLARE(type, name) namespace cvars { extern type name; }
#define XE_SHIM_CVAR_DEFINE(type, name, default_value) namespace cvars { type name = default_value; }

#define DEFINE_CVar(name, default_value, description, category, is_transient, type) \
    XE_SHIM_CVAR_DEFINE(type, name, default_value)
#define DEFINE_bool(name, default_value, description, category) XE_SHIM_CVAR_DEFINE(bool, name, default_value)
#define DEFINE_int32(name, default_value, description, category) XE_SHIM_CVAR_DEFINE(int32_t, name, default_value)
#define DEFINE_uint32(name, default_value, description, category) XE_SHIM_CVAR_DEFINE(uint32_t, name, default_value)
#define DEFINE_uint64(name, default_value, description, category) XE_SHIM_CVAR_DEFINE(uint64_t, name, default_value)
#define DEFINE_double(name, default_value, description, category) XE_SHIM_CVAR_DEFINE(double, name, default_value)
#define DEFINE_string(name, default_value, description, category) XE_SHIM_CVAR_DEFINE(std::string, name, default_value)
#define DEFINE_path(name, default_value, description, category) XE_SHIM_CVAR_DEFINE(std::filesystem::path, name, default_value)
#define DEFINE_transient_bool(name, default_value, description, category) XE_SHIM_CVAR_DEFINE(bool, name, default_value)
#define DEFINE_transient_string(name, default_value, description, category) XE_SHIM_CVAR_DEFINE(std::string, name, default_value)
#define DEFINE_transient_path(name, default_value, description, category) XE_SHIM_CVAR_DEFINE(std::filesystem::path, name, default_value)

#define DECLARE_bool(name) XE_SHIM_CVAR_DECLARE(bool, name)
#define DECLARE_int32(name) XE_SHIM_CVAR_DECLARE(int32_t, name)
#define DECLARE_uint32(name) XE_SHIM_CVAR_DECLARE(uint32_t, name)
#define DECLARE_uint64(name) XE_SHIM_CVAR_DECLARE(uint64_t, name)
#define DECLARE_double(name) XE_SHIM_CVAR_DECLARE(double, name)
#define DECLARE_string(name) XE_SHIM_CVAR_DECLARE(std::string, name)
#define DECLARE_path(name) XE_SHIM_CVAR_DECLARE(std::filesystem::path, name)

#define UPDATE_CVar(name, value, type) (void)0
#define UPDATE_from_bool(name, year, month, day, hour, value) (void)0
#define UPDATE_from_int32(name, year, month, day, hour, value) (void)0
#define UPDATE_from_uint32(name, year, month, day, hour, value) (void)0
#define UPDATE_from_string(name, year, month, day, hour, value) (void)0
