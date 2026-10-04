a () {
    ls || { echo "b"; return 0; }
    echo c
}

{ echo "a"
  echo "b"
} >&2
