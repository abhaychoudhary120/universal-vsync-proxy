#pragma once

// Generates a naked, argument-transparent jump stub for a forwarded export.
// The matching .def entry maps the public export name to vs_fwd_stub_<name>.
// The stub consumes no stack and touches no argument register.
#if defined(_WIN64)
#define VS_FORWARD_JUMP(name) "jmp *g_fwd_" #name "(%rip)"
#else
#define VS_FORWARD_JUMP(name) "jmp *_g_fwd_" #name
#endif

#define VS_DEFINE_FORWARD_STUB(name)                                      \
    extern "C" __attribute__((naked, used)) void vs_fwd_stub_##name() {   \
        __asm__ __volatile__(VS_FORWARD_JUMP(name));                      \
    }
