(: The needs report, run by the build on content/site.xml (mise/website.rb,
   needs!), written to build/needs.json: for each need of each audience,
   the pages and sections whose `answers` name it (a purpose's `answers`
   is the whole page's), and the needs no page answers yet, which are the
   work list. It reports; it refuses nothing. :)
declare namespace s = "https://www.tinyactors.dev/ns/site";

let $site := /s:site
let $pages := $site/s:pages/s:page ! doc(resolve-uri(@src, base-uri($site)))/s:page
let $answering := $pages//(s:purpose, s:section)[@answers]
let $report := array {
  for $audience in $site/s:audiences/s:audience
  let $needs := array {
    for $need in $audience/(s:want, s:task, s:objection)
    let $ref := $audience/@id || '.' || $need/@id
    let $by := $answering[tokenize(@answers) = $ref]
    return map {
      'id': $ref,
      'kind': local-name($need),
      'text': normalize-space($need),
      'answeredBy': array { $by ! map:merge((
        map { 'page': string(ancestor::s:page/@id), 'path': string(ancestor::s:page/@path) },
        if (self::s:section) then map { 'section': string(@id) } else ())) }
    }
  }
  return map {
    'id': string($audience/@id),
    'name': normalize-space($audience/s:name),
    'mainAudienceOf': array { $pages[tokenize(s:purpose/@for)[1] = $audience/@id] ! string(@id) },
    'answered': count($needs?*[array:size(?answeredBy) gt 0]),
    'needs': $needs
  }
}
return map {
  'audiences': $report,
  'unanswered': array { $report?*?needs?*[array:size(?answeredBy) eq 0] ! ?id }
}
