_path=$(
  while statement; do
    cd ".."
  done;
  echo $PWD
)
