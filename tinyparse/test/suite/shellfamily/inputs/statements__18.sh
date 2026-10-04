do_something() {
  echo ok
}

run_subshell_command() (
  true
)

do_nothing() { return 0; }

foo::bar() {
  echo what
}
