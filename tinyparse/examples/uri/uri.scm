(tinyparse version: "0.1")

(module examples.uri)

(grammar uri
  start: Uri

  (token SCHEME (regex "[A-Za-z][A-Za-z0-9+.-]*"))
  (token USERINFO (regex "[A-Za-z0-9._~%!$&'()*+,;=:-]+"))
  (token HOST (regex "[A-Za-z0-9._~%!$&'()*+,;=-]+"))
  (token PORT (regex "[0-9]+"))
  (token SEGMENTS (regex "/[A-Za-z0-9._~%!$&'()*+,;=:@/-]*"))
  (token QUERY (regex "[A-Za-z0-9._~%!$&'()*+,;=:@/?-]*"))
  (token FRAGMENT (regex "[A-Za-z0-9._~%!$&'()*+,;=:@/?-]*"))

  (rule Uri
    (seq (field scheme: SCHEME) "://" Authority (optional (field path: SEGMENTS)) (optional (seq "?" (field query: QUERY))) (optional (seq "#" (field fragment: FRAGMENT)))))
  (rule Authority
    (seq (optional (seq (field userinfo: USERINFO) "@")) (field host: HOST) (optional (seq ":" (field port: PORT)))))
)
