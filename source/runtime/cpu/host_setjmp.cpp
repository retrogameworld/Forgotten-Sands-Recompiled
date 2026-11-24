#include <stdafx.h>

// Host side of the guest setjmp/longjmp (PPC_SETJMP / PPC_LONGJMP in ppc_context.h).
//
// Why not the CRT or the compiler builtins:
//  - The Windows CRT longjmp unwinds the host stack with RtlUnwindEx (crashed).
//  - clang's __builtin_setjmp on Win64 saves llvm.frameaddress, which is not the value of rbp
//    when the frame pointer sits at an offset (lea rbp,[rsp+0x80]); after __builtin_longjmp
//    the epilogue used a wrong rbp and returned to garbage.
// So on Windows x64 this is a minimal setjmp/longjmp in assembly: saves the Win64 callee-saved
// registers (rbx rbp rdi rsi r12-r15, xmm6-xmm15), rsp, the return address and MXCSR / x87
// control word, and restores them without unwinding. Recompiled code has no destructors, so
// skipping host frames is fine. The buffer (256 bytes) lives at the start of the guest jmp_buf
// (> 1 KB on the Xbox 360). Other platforms use _setjmp/_longjmp from libc (no unwinding).

#if defined(_WIN32) && defined(__x86_64__)

extern "C" __attribute__((naked, returns_twice)) int PPCHostSetJmp(void* buf)
{
    __asm__ volatile(
        "movq %rbx, 0x00(%rcx)\n"
        "movq %rbp, 0x08(%rcx)\n"
        "movq %rdi, 0x10(%rcx)\n"
        "movq %rsi, 0x18(%rcx)\n"
        "movq %r12, 0x20(%rcx)\n"
        "movq %r13, 0x28(%rcx)\n"
        "movq %r14, 0x30(%rcx)\n"
        "movq %r15, 0x38(%rcx)\n"
        "leaq 8(%rsp), %rdx\n"      // rsp after our return
        "movq %rdx, 0x40(%rcx)\n"
        "movq (%rsp), %rdx\n"       // return address
        "movq %rdx, 0x48(%rcx)\n"
        "stmxcsr 0x50(%rcx)\n"
        "fnstcw 0x54(%rcx)\n"
        "movups %xmm6, 0x60(%rcx)\n"
        "movups %xmm7, 0x70(%rcx)\n"
        "movups %xmm8, 0x80(%rcx)\n"
        "movups %xmm9, 0x90(%rcx)\n"
        "movups %xmm10, 0xA0(%rcx)\n"
        "movups %xmm11, 0xB0(%rcx)\n"
        "movups %xmm12, 0xC0(%rcx)\n"
        "movups %xmm13, 0xD0(%rcx)\n"
        "movups %xmm14, 0xE0(%rcx)\n"
        "movups %xmm15, 0xF0(%rcx)\n"
        "xorl %eax, %eax\n"
        "ret\n");
}

extern "C" __attribute__((naked, noreturn)) void PPCHostLongJmp(void* buf, int value)
{
    __asm__ volatile(
        "movl %edx, %eax\n"
        "testl %eax, %eax\n"
        "jnz 1f\n"
        "movl $1, %eax\n"
        "1:\n"
        "movq 0x00(%rcx), %rbx\n"
        "movq 0x08(%rcx), %rbp\n"
        "movq 0x10(%rcx), %rdi\n"
        "movq 0x18(%rcx), %rsi\n"
        "movq 0x20(%rcx), %r12\n"
        "movq 0x28(%rcx), %r13\n"
        "movq 0x30(%rcx), %r14\n"
        "movq 0x38(%rcx), %r15\n"
        "ldmxcsr 0x50(%rcx)\n"
        "fnclex\n"
        "fldcw 0x54(%rcx)\n"
        "movups 0x60(%rcx), %xmm6\n"
        "movups 0x70(%rcx), %xmm7\n"
        "movups 0x80(%rcx), %xmm8\n"
        "movups 0x90(%rcx), %xmm9\n"
        "movups 0xA0(%rcx), %xmm10\n"
        "movups 0xB0(%rcx), %xmm11\n"
        "movups 0xC0(%rcx), %xmm12\n"
        "movups 0xD0(%rcx), %xmm13\n"
        "movups 0xE0(%rcx), %xmm14\n"
        "movups 0xF0(%rcx), %xmm15\n"
        "movq 0x40(%rcx), %rsp\n"
        "jmpq *0x48(%rcx)\n");
}

#else

#include <setjmp.h>

static_assert(sizeof(jmp_buf) <= 1024, "host jmp_buf must fit in the guest jmp_buf");

// Note: setjmp must be called in the frame that later continues, so on these platforms
// PPC_SETJMP in ppc_context.h calls _setjmp directly; only longjmp lives here.
extern "C" [[noreturn]] void PPCHostLongJmp(void* buf, int value)
{
    _longjmp(*(jmp_buf*)buf, value ? value : 1);
}

#endif
