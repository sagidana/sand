# bash completion for sand: flags, profile names, paths, and after the
# flags the wrapped command's own completion.
# Needs the bash-completion package (for _init_completion/_command_offset).

_sand()
{
    local cur prev words cword
    _init_completion || return

    local i
    for ((i = 1; i < cword; i++)); do
        case ${words[i]} in
            -p|--profile|--ro|--rw|--hide|--env|--net)
                ((i++))
                ;;
            --)
                _sand_command $((i + 1))
                return
                ;;
            -*)
                ;;
            *)
                _sand_command "$i"
                return
                ;;
        esac
    done

    case $prev in
        -p|--profile)
            local dir=${XDG_CONFIG_HOME:-$HOME/.config}/sand
            COMPREPLY=($(cd "$dir" 2>/dev/null && compgen -f -- "$cur"))
            return
            ;;
        --ro|--rw|--hide)
            _filedir
            return
            ;;
        --env)
            COMPREPLY=($(compgen -e -- "$cur"))
            return
            ;;
        --net)
            COMPREPLY=($(compgen -W "none host $(ls /var/lib/sand/net 2>/dev/null)" -- "$cur"))
            return
            ;;
    esac

    if [[ $cur == -* ]]; then
        COMPREPLY=($(compgen -W '-p --profile --ro --rw --hide --env --net --' -- "$cur"))
        return
    fi
    # An empty command word would list every program on PATH, which
    # takes long on WSL where PATH reaches into Windows directories.
    [[ -z $cur ]] && return
    _sand_command "$cword"
}

# Complete as if the command line started at word $1.
_sand_command()
{
    if declare -F _comp_command_offset >/dev/null; then
        _comp_command_offset "$1"
    else
        _command_offset "$1"
    fi
}

complete -F _sand sand
