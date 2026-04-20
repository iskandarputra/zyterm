# fish completion for zyterm
# Install:  cp zyterm.fish ~/.config/fish/completions/zyterm.fish

complete -c zyterm -f

# Connection
complete -c zyterm -s b -l baud -d 'baud rate' -x \
    -a '9600 19200 38400 57600 115200 230400 460800 921600 1500000 3000000'
complete -c zyterm      -l data -d 'data bits' -x -a '5 6 7 8'
complete -c zyterm      -l parity -d 'parity'  -x -a 'n e o'
complete -c zyterm      -l stop -d 'stop bits' -x -a '1 2'
complete -c zyterm      -l flow -d 'flow control' -x -a 'n r x'
complete -c zyterm      -l reconnect    -d 'auto-reopen on hang-up (default)'
complete -c zyterm      -l no-reconnect -d 'exit on hang-up'
complete -c zyterm      -l port-glob -d 're-resolve device via glob on reconnect' \
    -x -a '/dev/ttyUSB* /dev/ttyACM* /dev/serial/by-id/*'
complete -c zyterm      -l match-vid-pid -d 'filter ports by USB VID:PID' \
    -x -a '0403:6001 0403:6014 067b:2303 10c4:ea60 1a86:7523 2341:0043'

# Line endings
complete -c zyterm      -l map-out -d 'rewrite outgoing line endings' \
    -x -a 'none cr lf crlf cr-crlf lf-crlf'
complete -c zyterm      -l map-in  -d 'rewrite incoming line endings' \
    -x -a 'none cr lf crlf cr-crlf lf-crlf'

# Logging
complete -c zyterm -s l -l log     -d 'append timestamped log' -r
complete -c zyterm      -l log-max-kb -d 'rotate log at N KB' -x
complete -c zyterm      -l replay  -d 'replay a captured log' -r
complete -c zyterm      -l jsonl   -d 'write structured JSONL log' -r

# Profile
complete -c zyterm      -l profile      -d 'load profile' -r
complete -c zyterm      -l profile-save -d 'save current options as profile' -x
complete -c zyterm      -l profile-list -d 'list saved profiles'

# Misc
complete -c zyterm      -l watch       -d 'regex; trigger on match' -x
complete -c zyterm      -l watch-color -d 'colour for watch matches' -x
complete -c zyterm      -l watch-beep  -d 'BEL on watch match'
complete -c zyterm      -l macro       -d 'bind macro F1..F12' -x
complete -c zyterm      -l hud         -d 'show statistics HUD'
complete -c zyterm      -l no-hud      -d 'disable HUD'
complete -c zyterm      -l http        -d 'start HTTP/SSE bridge on port' -x
complete -c zyterm      -l http-bind   -d 'bind HTTP bridge to address' -x
complete -c zyterm -s h -l help        -d 'show help'
complete -c zyterm -s V -l version     -d 'print version'

# Positional <device>: tty nodes and URL schemes
complete -c zyterm -n '__fish_is_token_n 2' -a 'tcp:// telnet:// rfc2217://' \
    -d 'network URL'
complete -c zyterm -n '__fish_is_token_n 2' \
    -a '(__fish_complete_path /dev/tty)' -d 'serial device'
