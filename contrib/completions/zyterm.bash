# bash completion for zyterm
# Install:  cp zyterm.bash /etc/bash_completion.d/zyterm
#       or: source contrib/completions/zyterm.bash from ~/.bashrc

_zyterm() {
    local cur prev words cword
    _init_completion || return

    local opts="
        -b --baud --data --parity --stop --flow
        --reconnect --no-reconnect
        --port-glob --match-vid-pid
        --map-out --map-in
        -l --log --log-max-kb --replay
        --profile --profile-save --profile-list
        --watch --watch-color --watch-beep
        --macro
        --hud --no-hud
        --http --http-bind
        --jsonl
        -h --help -V --version
    "

    case "$prev" in
        -b|--baud)
            COMPREPLY=( $(compgen -W "9600 19200 38400 57600 115200 230400 460800 921600 1500000 3000000" -- "$cur") )
            return 0 ;;
        --data)
            COMPREPLY=( $(compgen -W "5 6 7 8" -- "$cur") ); return 0 ;;
        --parity)
            COMPREPLY=( $(compgen -W "n e o" -- "$cur") ); return 0 ;;
        --stop)
            COMPREPLY=( $(compgen -W "1 2" -- "$cur") ); return 0 ;;
        --flow)
            COMPREPLY=( $(compgen -W "n r x" -- "$cur") ); return 0 ;;
        --map-out|--map-in)
            COMPREPLY=( $(compgen -W "none cr lf crlf cr-crlf lf-crlf" -- "$cur") )
            return 0 ;;
        -l|--log|--replay|--jsonl|--profile|--profile-save)
            _filedir; return 0 ;;
        --port-glob)
            COMPREPLY=( $(compgen -W "/dev/ttyUSB* /dev/ttyACM* /dev/serial/by-id/*" -- "$cur") )
            return 0 ;;
        --match-vid-pid)
            COMPREPLY=( $(compgen -W "0403:6001 0403:6014 067b:2303 10c4:ea60 1a86:7523 2341:0043" -- "$cur") )
            return 0 ;;
    esac

    if [[ "$cur" == -* ]]; then
        COMPREPLY=( $(compgen -W "$opts" -- "$cur") )
        return 0
    fi

    # Positional <device>: complete tty nodes and tcp:// URLs
    local devs
    devs=$(compgen -G '/dev/ttyUSB*' 2>/dev/null; compgen -G '/dev/ttyACM*' 2>/dev/null; compgen -G '/dev/serial/by-id/*' 2>/dev/null)
    COMPREPLY=( $(compgen -W "$devs tcp:// telnet:// rfc2217://" -- "$cur") )
}
complete -F _zyterm zyterm
