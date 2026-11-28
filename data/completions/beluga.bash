# bash completion for beluga

_beluga()
{
    local cur prev
    _init_completion || return

    case $prev in
        --screenshots)
            _filedir -d
            return
            ;;
        --log-file)
            _filedir
            return
            ;;
    esac
    if [[ $cur == --log=* ]]; then
        COMPREPLY=($(compgen -W '1 2 3' -- "${cur#--log=}"))
        return
    fi
    if [[ $cur == -* ]]; then
        COMPREPLY=($(compgen -W '-s --select --split --new-window --daemon --detach --portal -v --version
            --log --log=1 --log=2 --log=3 --log-file -h --help --help-all --help-gapplication --gapplication-service --selftest --screenshots' -- "$cur"))
        return
    fi
    # Files and folders to open (folders) or select (--select).
    _filedir
} &&
    complete -F _beluga beluga
