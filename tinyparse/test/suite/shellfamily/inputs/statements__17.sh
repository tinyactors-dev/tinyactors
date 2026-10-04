(
  ./start-server --port=80
) &

( cd tests && sh run-tests.sh )
