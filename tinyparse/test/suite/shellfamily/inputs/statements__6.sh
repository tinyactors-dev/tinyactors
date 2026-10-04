if cat some_file | grep -v ok; then
  echo one
elif cat other_file | grep -v ok; then
  echo two
else
  exit
fi
