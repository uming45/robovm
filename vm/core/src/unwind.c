#include <nullvm.h>
#include <unwind.h>
#include "uthash.h"
#include "private.h"

#define EXCEPTION_CLASS 0x4A4A4A4A4A4A4A4A // "JJJJJJJJ"
#if defined(NVM_X86_64)
    // Assume that unwindBacktrace() and _call0() save the CFA in %rbp
    #define CFA_REG 6
#elif defined(NVM_I386) && defined(LINUX)
    // Assume that unwindBacktrace() and _call0() save the CFA in %ebp
    #define CFA_REG 5
#elif defined(NVM_I386) && defined(DARWIN)
    // Assume that unwindBacktrace() and _call0() save the CFA in %ebp
    #define CFA_REG 4
#endif

typedef struct UnwindInfo {
    struct _Unwind_Exception exception_info;
    void* throwable;
    _Unwind_Ptr landing_pad;
} UnwindInfo;

typedef struct CallbackEntry {
    void* key;
    void* value;
    UT_hash_handle hh;
} CallbackEntry;
static CallbackEntry* callbacks = NULL;

extern _Unwind_Reason_Code unwindBacktrace(_Unwind_Trace_Fn fn, void* data);
extern _Unwind_Reason_Code __gcc_personality_v0(int version, _Unwind_Action actions, _Unwind_Exception_Class exception_class, struct _Unwind_Exception* exception_info, struct _Unwind_Context* context);

_Unwind_Reason_Code _nvmPersonality(int version, _Unwind_Action actions, _Unwind_Exception_Class exception_class, struct _Unwind_Exception* exception_info, struct _Unwind_Context* context) {

    UnwindInfo* info = (UnwindInfo*) exception_info;
    if (actions & _UA_SEARCH_PHASE) {
        _Unwind_Ptr saved_ip = _Unwind_GetIP(context);
        _Unwind_Reason_Code urc = __gcc_personality_v0(version, _UA_CLEANUP_PHASE, exception_class, exception_info, context);
        if (urc == _URC_INSTALL_CONTEXT) {
            info->landing_pad = _Unwind_GetIP(context);
            _Unwind_SetIP(context, saved_ip);
            return _URC_HANDLER_FOUND;
        }
        return urc;
    } else if (actions & _UA_HANDLER_FRAME) {
        _Unwind_SetGR(context, __builtin_eh_return_data_regno (0), (_Unwind_Ptr) exception_info);
        _Unwind_SetGR(context, __builtin_eh_return_data_regno (1), 0);
        _Unwind_SetIP(context, info->landing_pad); 
        return _URC_INSTALL_CONTEXT;
    }

    return _URC_CONTINUE_UNWIND;
}

jint unwindRaiseException(Env* env) {
    UnwindInfo* u = nvmAllocateMemory(env, sizeof(UnwindInfo));
    u->exception_info.exception_class = EXCEPTION_CLASS;
    u->throwable = env->throwable;
    _Unwind_Reason_Code urc = _Unwind_RaiseException(&u->exception_info);
    return urc == _URC_END_OF_STACK ? UNWIND_UNHANDLED_EXCEPTION : UNWIND_FATAL_ERROR;
}

typedef struct UnwindCallStackData {
    jboolean (*iterator)(Env*, void*, jint, void*);
    Env* env;
    void** nativeFramesTop;
    void* data;
    void** restoreFrameAddressPtr;
    void* restoreFrameAddress;
} UnwindCallStackData;

static jboolean isCallback(void* function) {
    // TODO: Lock?
    CallbackEntry* entry;
    HASH_FIND_PTR(callbacks, &function, entry);
    return entry != NULL ? TRUE : FALSE;
}

static _Unwind_Reason_Code unwindCallStack(struct _Unwind_Context* ctx, UnwindCallStackData* d) {
    if (d->restoreFrameAddressPtr) {
        // Restore the frame we altered in the previous call to this callback.
        *d->restoreFrameAddressPtr = d->restoreFrameAddress;
        d->restoreFrameAddressPtr = NULL;
        d->restoreFrameAddress = NULL;
    }
    void* address = (void*) _Unwind_GetIP(ctx);
    void* function = _Unwind_FindEnclosingFunction(address);
    if (d->nativeFramesTop > d->env->nativeFrames.base) {
        if (function == unwindBacktrace || function == _call0 || isCallback(function)) {
            // Temporarily alter the frame to skip all frames until the last native trampoline frame.
            // We need to do this since we cannot assume native code to have proper unwind info.
            // Note that unwindBacktrace and _call0 must save the CFA in a well defined register 
            // (%rbp on x86_64, %ebp on i386) for this to work. We also make some assumptions
            // on the layout of _Unwind_Context (DWARF register 0 is saved at ctx+0, register 1 at
            // ctx+1, etc).
            d->nativeFramesTop--;
            void* cfa = *d->nativeFramesTop;
            d->restoreFrameAddressPtr = *(((void***) ctx) + CFA_REG);
            d->restoreFrameAddress = (void*) _Unwind_GetGR(ctx, CFA_REG);
            _Unwind_SetGR(ctx, CFA_REG, (_Unwind_Word) cfa);
            return _URC_NO_REASON;
        }
    }
    jint offset = address - function;
    return d->iterator(d->env, function, offset, d->data) ? _URC_NO_REASON : _URC_NORMAL_STOP;
}

void unwindIterateCallStack(Env* env, jboolean (*iterator)(Env*, void*, jint, void*), void* data) {
    UnwindCallStackData d = {iterator, env, env->nativeFrames.top, data, NULL, NULL};
    unwindBacktrace((_Unwind_Trace_Fn) unwindCallStack, &d);
}

void unwindRegisterCallback(Env* env, void* callbackImpl) {
    CallbackEntry* entry = nvmAllocateMemory(env, sizeof(CallbackEntry));
    if (!entry) return;
    entry->key = callbackImpl;
    entry->value = callbackImpl;
    HASH_ADD_PTR(callbacks, key, entry);
}
