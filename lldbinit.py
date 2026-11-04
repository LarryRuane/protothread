#**************************************************************
#* lldbinit.py
#* https://github.com/LarryRuane/protothread
#* Copyright (c) 2008-present Larry Ruane
#* Distributed under the MIT software license, see the accompanying
#* file LICENSE or https://opensource.org/licenses/MIT.
#* SPDX-License-Identifier: MIT
#*
#* Protothread stack trace commands for lldb (port of gdbinit)
#* Load with: command script import /path/to/lldbinit.py
#* (or add that line to your ~/.lldbinit file)
#*
#* PT_DEBUG must be defined as 1 in protothread.h
#*
#**************************************************************

import lldb


def _eval(exe_ctx, expr, result):
    # try the current frame, then outer frames (so "pt" works inside callees)
    frame = exe_ctx.GetFrame()
    thread = exe_ctx.GetThread()
    first = None
    for i in range(frame.GetFrameID(), thread.GetNumFrames()):
        v = thread.GetFrameAtIndex(i).EvaluateExpression(expr)
        if v.IsValid() and v.GetError().Success():
            return v
        first = first or v
    result.SetError("can't evaluate '%s': %s" % (expr, first.GetError()))
    return None


def _str(v):
    addr = v.GetValueAsUnsigned()
    if not addr:
        return "?"
    err = lldb.SBError()
    s = v.GetProcess().ReadCStringFromMemory(addr, 4096, err)
    return s if err.Success() else "?"


def _ptbt(t, out):
    for name in ("func", "env", "channel"):
        out.write("%-6s%s\n" % (name[:4] + ":", t.GetChildMemberWithName(name)))
    pte = t.GetChildMemberWithName("pt_func")
    j = 0
    while pte.GetValueAsUnsigned() and \
            pte.GetChildMemberWithName("label").GetValueAsUnsigned():
        out.write("#%d %s (0x%x) at %s:%d\n" % (
            j, _str(pte.GetChildMemberWithName("function")),
            pte.GetValueAsUnsigned(),
            _str(pte.GetChildMemberWithName("file")),
            pte.GetChildMemberWithName("line").GetValueAsSigned()))
        pte = pte.GetChildMemberWithName("next")
        j += 1


def _ring(head, state, out):
    # lists are circular, head points to newest, head->next is oldest
    if not head.GetValueAsUnsigned():
        return
    pt = head
    while True:
        pt = pt.GetChildMemberWithName("next")
        out.write("\nstate: %s p *(struct pt_thread_s *)0x%x\n"
                  % (state, pt.GetValueAsUnsigned()))
        _ptbt(pt, out)
        if pt.GetValueAsUnsigned() == head.GetValueAsUnsigned():
            break


def ptbt(debugger, command, exe_ctx, result, internal_dict):
    """pt_thread_t -- print stack backtrace of given protothread"""
    t = _eval(exe_ctx, command, result)
    if t is not None:
        _ptbt(t, result)


def ptbtall(debugger, command, exe_ctx, result, internal_dict):
    """protothread_t -- print stack backtraces of all protothreads"""
    s = _eval(exe_ctx, command, result)
    if s is None:
        return
    running = s.GetChildMemberWithName("running")
    if running.GetValueAsUnsigned():
        result.write("\nstate: running p *(struct pt_thread_s *)0x%x\n"
                     % running.GetValueAsUnsigned())
        _ptbt(running, result)
    _ring(s.GetChildMemberWithName("ready"), "ready_to_run", result)
    wait = s.GetChildMemberWithName("wait")
    for i in range(wait.GetNumChildren()):
        _ring(wait.GetChildAtIndex(i), "wait", result)


def __lldb_init_module(debugger, internal_dict):
    for name in ("ptbt", "ptbtall"):
        debugger.HandleCommand(
            "command script add -o -f %s.%s %s" % (__name__, name, name))
