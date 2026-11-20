namespace eval ::tyaml {
    proc options {defaults args} {
        if {[llength $args] % 2} {error "expected option/value pairs"}
        foreach {name value} $args {
            if {![dict exists $defaults $name]} {error "unknown option $name"}
            dict set defaults $name $value
        }
        return $defaults
    }

    proc parseInput {text all args} {
        set opts [options {-mode yaml -mutable 0 -dedup 1 -trim 1 -collect_diag 0 -create_markers 0 -keep_comments 0 -keep_style 0 -keep_anchors 1 -strings 0} {*}$args]
        return [_parse $text [dict get $opts -mode] $all [dict get $opts -mutable] \
            [dict get $opts -dedup] [dict get $opts -trim] [dict get $opts -collect_diag] \
            [dict get $opts -create_markers] [dict get $opts -keep_comments] \
            [dict get $opts -keep_style] [dict get $opts -keep_anchors] [dict get $opts -strings]]
    }
    proc loads {text args} {parseInput $text 0 {*}$args}
    proc loads_all {text args} {parseInput $text 1 {*}$args}

    proc readInput {source} {
        if {$source in [chan names]} {return [read $source]}
        set ch [open $source r]
        try {
            fconfigure $ch -encoding utf-8
            return [read $ch]
        } finally {close $ch}
    }
    proc load {source args} {loads [readInput $source] {*}$args}
    proc load_all {source args} {loads_all [readInput $source] {*}$args}

    proc makeValue {value typed args} {
        set opts [options {-mutable 0 -dedup 1 -tag {} -style {}} {*}$args]
        return [_create $value $typed [dict get $opts -mutable] [dict get $opts -dedup] \
            [dict get $opts -tag] [dict get $opts -style]]
    }
    proc from_tcl {value args} {makeValue $value 0 {*}$args}
    proc to_tcl {handle} {to_simple $handle}

    proc varTrace {handle name index op} {
        upvar 1 $name variable
        if {$op eq "unset"} {
            if {[_valid $handle]} {destroy $handle}
        } elseif {[_valid $handle]} {
            if {[array exists variable]} {set variable($index) $handle} else {set variable $handle}
            error "tyaml handle variable is read-only"
        }
    }
    proc scoped {handle name} {
        if {$name ne ""} {
            upvar 1 $name variable
            try {
                set variable $handle
                trace add variable variable {write unset} [list ::tyaml::varTrace $handle]
            } on error {msg opts} {
                destroy $handle
                return -options $opts $msg
            }
        }
        return $handle
    }
    proc parse {text {varname {}} args} {
        tailcall ::tyaml::scoped [loads $text {*}$args] $varname
    }
    proc create {typed {varname {}} args} {
        tailcall ::tyaml::scoped [makeValue $typed 1 {*}$args] $varname
    }
    proc yaml_to_simple {text args} {
        set handle [loads $text -strings 1 {*}$args]
        try {to_simple $handle} finally {destroy $handle}
    }
    proc yaml_to_typed {text args} {
        set handle [loads $text {*}$args]
        try {to_typed $handle} finally {destroy $handle}
    }
    proc serialize {obj multi args} {
        set opts [options {-json 0 -mode yaml -compact 0 -style {} -indent 0 -strip_newline 0} {*}$args]
        set mode [dict get $opts -mode]
        if {$mode ni {yaml json}} {error "expected mode yaml or json"}
        return [_emit $obj $multi [expr {$mode eq "json" || [dict get $opts -json]}] \
            [dict get $opts -compact] [dict get $opts -style] [dict get $opts -indent] \
            [dict get $opts -strip_newline]]
    }
    proc dumps {obj args} {serialize $obj 0 {*}$args}
    proc dumps_all {docs args} {serialize $docs 1 {*}$args}
    proc writeOutput {target text} {
        if {$target in [chan names]} {puts -nonewline $target $text; return}
        set ch [open $target w]
        try {
            fconfigure $ch -encoding utf-8 -translation lf
            puts -nonewline $ch $text
        } finally {close $ch}
    }
    proc dump {target obj args} {writeOutput $target [dumps $obj {*}$args]}
    proc dump_all {target docs args} {writeOutput $target [dumps_all $docs {*}$args]}
    proc get {handle key} {get_at_path $handle [list $key]}
    proc set_item {handle key value} {set_at_path $handle [list $key] $value}
    proc path_list_to_unix_path {path} {return /[join $path /]}
    proc unix_path_to_path_list {path} {
        if {$path in {{} /}} {return {}}
        if {[string index $path 0] ne "/"} {error "Unix path must start with /"}
        return [split [string range $path 1 end] /]
    }
    proc get_unix_path {handle} {path_list_to_unix_path [get_path $handle]}
    proc get_at_unix_path {handle path} {get_at_path $handle [unix_path_to_path_list $path]}
    proc set_at_unix_path {handle path value} {set_at_path $handle [unix_path_to_path_list $path] $value}
}

# A node's dump method in the Python API accepts an optional destination.
proc ::tyaml::node_dump {handle args} {
    if {![_valid $handle]} {error "unknown or destroyed tyaml handle"}
    if {[llength $args] % 2} {
        return [dump [lindex $args 0] $handle {*}[lrange $args 1 end]]
    }
    return [dumps $handle {*}$args]
}
