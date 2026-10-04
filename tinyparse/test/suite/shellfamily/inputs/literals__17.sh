echo 'word'#not-comment # a legit comment
echo $(uname -a)#not-comment # a legit comment
echo `uname -a`#not-comment # a legit comment
echo $hey#not-comment # a legit comment
var=#not-comment # a legit comment
echo "'$var'" # -> '#not-comment'
