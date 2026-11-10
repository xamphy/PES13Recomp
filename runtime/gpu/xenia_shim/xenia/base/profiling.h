// Shim for Xenia's profiling.h: profiling scopes compile to nothing (no microprofile).
#pragma once
#define DEFINE_profile_cpu(name, group_name, scope_name)
#define DECLARE_profile_cpu(name)
#define DEFINE_profile_gpu(name, group_name, scope_name)
#define DECLARE_profile_gpu(name)
#define SCOPE_profile_cpu(name) do {} while (false)
#define SCOPE_profile_cpu_f(name) do {} while (false)
#define SCOPE_profile_cpu_i(group_name, scope_name) do {} while (false)
#define COUNT_profile_gpu(name)
#define COUNT_profile_set(name, value) do {} while (false)
