for a in 1 2 $(seq 5 10); do
  echo $a
done

for ARG; do
  echo $ARG
  ARG=''
done
