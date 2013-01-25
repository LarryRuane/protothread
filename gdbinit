#**************************************************************
#* gdbinit
#* Larry Ruane
#*
#* Protothread stack trace macros
#* Copy or append this file to your ~/.gdbinit file
#*
#* PT_DEBUG must be defined as 1 in protothread.h
#*
#**************************************************************

define ptbt
    printf "func: "
    p $arg0->func
    printf "env:  "
    p $arg0->env
    printf "chan: "
    p $arg0->channel
    set $pte = &$arg0->pt_func
    set $j = 0
    while ($pte && $pte->label)
        printf "#%d %s (%p) at %s:%d\n", $j, $pte->function, $pte, $pte->file, $pte->line
        set $pte = $pte->next
        set $j++
    end
end

document ptbt
    pt_thread_t -- print stack backtrace of given protothread
end

define ptbtall
    set $pt = $arg0->ready
    while ($pt)
        set $pt = $pt->next
        printf "\nstate: ready_to_run p *(struct pt_thread_s *)%p\n", $pt
        ptbt $pt
        if ($pt == $arg0->ready)
            set $pt = 0
        end
    end

    set $i = 0
    set $nwait = sizeof($arg0->wait) / sizeof($arg0->wait[0])
    while ($i < $nwait)
        set $pt = $arg0->wait[$i]
        while ($pt)
            set $pt = $pt->next
            printf "\nstate: wait p *(struct pt_thread_s *)%p\n", $pt
            ptbt $pt
            if ($pt == $arg0->wait[$i])
                set $pt = 0
            end
        end
        set $i++
    end
end

document ptbtall
    protothread_t -- print stack backtraces of all protothreads
end
