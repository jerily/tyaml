lappend auto_path [lindex $argv 0]
set argv {}
package require tcltest
package require tyaml 0.1.0
namespace import ::tcltest::test

test typed-1 {native scalar types, explicit tag, empty collections} -body {
    tyaml::yaml_to_typed {items: [true, "true", 2, 2.5, null, [], {}, !Ref Bucket]}
} -result {M {items {L {{BOOL 1} {S true} {N 2} {N 2.5} {NULL {}} {L {}} {M {}} {TAG {!Ref {S Bucket}}}}}}}

test simple-1 {mock environment scalars retain their spelling} -body {
    tyaml::yaml_to_simple {ON: on
NUMBER: 001
FLAG: false}
} -result {ON on NUMBER 001 FLAG false}

test create-1 {construct and serialize typed values, including tags} -body {
    set value {M {a {L {{BOOL 1} {S true} {TAG {!Sub {S text}}}}}}}
    tyaml::create $value doc
    tyaml::yaml_to_typed [tyaml::dumps $doc]
} -cleanup {unset doc} -result {M {a {L {{BOOL 1} {S true} {TAG {!Sub {S text}}}}}}}

test native-1 {nested Tcl dictionaries and lists retain internal types} -body {
    set doc [tyaml::from_tcl [dict create a [list [expr {2}] xyz]]]
    tyaml::to_typed $doc
} -cleanup {tyaml::destroy $doc} -result {M {a {L {{N 2} {S xyz}}}}}

test path-1 {child paths, mutations and clone lifetime} -body {
    set doc [tyaml::loads {a: [{b: first}]} -mutable 1]
    set child [tyaml::get_at_unix_path $doc /a/0/b]
    set clone [tyaml::clone $child]
    tyaml::set_at_path $doc {a 0 b} second
    tyaml::destroy $doc
    list [tyaml::get_path $child] [tyaml::get_unix_path $child] [tyaml::to_simple $child] [tyaml::to_simple $clone]
} -cleanup {tyaml::destroy $child; tyaml::destroy $clone} -result {{a 0 b} /a/0/b second first}

test scoped-1 {variable scope and read-only trace} -body {
    set h [apply {{} {
        tyaml::parse {a: b} var
        if {![catch {set var replacement}]} {error "write was accepted"}
        return $var
    }}]
    catch {tyaml::to_simple $h} msg
    set msg
} -result {unknown or destroyed tyaml handle}

test mode-1 {YAML 1.1, YAML 1.2 and JSON} -body {
    list [tyaml::yaml_to_typed yes -mode 1.1] [tyaml::yaml_to_typed yes] \
        [catch {tyaml::loads {a: yaml} -mode json}]
} -result {{BOOL 1} {S yes} 1}

test stream-1 {multiple documents round trip through string, channel and path} -body {
    set filename [::tcltest::makeFile {} tyaml-stream.yml]
    set docs [tyaml::loads_all "---\na: 1\n---\nb: 2"]
    tyaml::dump_all $filename $docs
    set ch [open $filename r]
    try {set readDocs [tyaml::load_all $ch]} finally {close $ch}
    list [lmap h $readDocs {tyaml::to_simple $h}] [catch {tyaml::load $filename}] \
        [tyaml::loads_all "  # no documents\n"] [tyaml::dumps_all {}]
} -cleanup {
    foreach h [concat $docs $readDocs] {tyaml::destroy $h}
    ::tcltest::removeFile tyaml-stream.yml
} -result {{{a 1} {b 2}} 1 {} {}}

test metadata-1 {document state, anchors, comments, markers and tags} -body {
    tyaml::parse "%YAML 1.2\n---\na: &x !Ref bucket # comment\nb: *x" doc -keep_comments 1 -create_markers 1
    set a [tyaml::get $doc a]
    list [tyaml::get_tag $a] [tyaml::get_anchor $a] [string trim [tyaml::get_comment $a]] \
        [llength [tyaml::get_marker $a]] [dict get [tyaml::document_state $doc] version_explicit] \
        [tyaml::document_state $a]
} -cleanup {tyaml::destroy $a; unset doc} -result {!Ref x comment 6 1 {}}

test diag-1 {structured parser errors are available on request} -body {
    set doc [tyaml::loads "bad: \[" -collect_diag 1]
    set diag [tyaml::get_diag $doc]
    list [tyaml::has_diag $doc] [dict exists [lindex [tyaml::to_simple $diag] 0] message]
} -cleanup {tyaml::destroy $doc; tyaml::destroy $diag} -result {1 1}

test lifecycle-1 {immutable writes, missing paths and invalid handles report Tcl errors} -body {
    set h [tyaml::loads {a: b}]
    set errors [list [catch {tyaml::set_item $h a c}] [catch {tyaml::get $h no}] ]
    tyaml::destroy $h
    lappend errors [catch {tyaml::destroy $h}]
} -result {1 1 1}

test container-1 {keys, values and membership} -body {
    tyaml::parse {a: [red, green]} h
    set seq [tyaml::get $h a]
    set children [tyaml::values $seq]
    list [tyaml::keys $h] [tyaml::contains $seq green] [tyaml::size $seq] [lmap c $children {tyaml::to_simple $c}]
} -cleanup {foreach c $children {tyaml::destroy $c}; tyaml::destroy $seq; unset h} -result {a 1 2 {red green}}

test utf8-1 {sized UTF-8 through Tcl 8 and Tcl 9} -body {
    set scalar "jalapeño 世界 [encoding convertfrom utf-8 [binary format H* f09f9882]]\x00end"
    set h [tyaml::create [list S $scalar]]
    set reparsed [tyaml::loads [tyaml::dumps $h]]
    expr {[tyaml::to_simple $reparsed] eq $scalar}
} -cleanup {tyaml::destroy $h; tyaml::destroy $reparsed} -result 1

test binary-1 {tagged binary survives YAML and Tcl bytearray conversions} -body {
    set bytes [binary format H* ff00c07f]
    set doc [tyaml::from_tcl $bytes]
    set loaded [tyaml::loads [tyaml::dumps $doc]]
    binary encode hex [tyaml::to_simple $loaded]
} -cleanup {tyaml::destroy $doc; tyaml::destroy $loaded} -result ff00c07f

test number-1 {unsigned integers retain precision through typed construction} -body {
    tyaml::create {N 18446744073709551615} doc
    tyaml::to_typed $doc
} -cleanup {unset doc} -result {N 18446744073709551615}

test interp-1 {handles are local to their creating interpreter} -body {
    interp create child
    child eval [list lappend auto_path [lindex $auto_path end]]
    child eval {package require tyaml}
    set h [child eval {tyaml::loads hello}]
    list [catch {tyaml::to_simple $h}] [child eval [list tyaml::to_simple $h]]
} -cleanup {interp delete child} -result {1 hello}

set failed $::tcltest::numTests(Failed)
::tcltest::cleanupTests
exit [expr {$failed != 0}]
