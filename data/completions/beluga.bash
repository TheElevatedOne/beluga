# bash completion for beluga

_beluga()
{
    local cur prev
    _init_completion || return

    if [[ $prev == --screenshots ]]; then
        _filedir -d
        return
    fi
    if [[ $cur == -* ]]; then
        COMPREPLY=($(compgen -W '-s --select --split --new-window --daemon --detach --portal -v --version
            -h --help --help-all --help-gapplication --gapplication-service --selftest --screenshots' -- "$cur"))
        return
    fi
    # Files and folders to open (folders) or select (--select).
    _filedir
} &&
    complete -F _beluga beluga
