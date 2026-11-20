<?xml version="1.0" encoding="UTF-8"?>
<!--
  The site, from its content (content/site.xml and the pages it lists) to
  HTML in the design system (style/site.css over the design system's
  styles.css). Run on content/site.xml with a result directory:

    sxml transform -\-result-directory=build/site render/site.xsl content/site.xml

  It writes one HTML file per page (a path "/x/" as x/index.html, "/404" as
  404.html), sitemap.xml and llms.txt. It stops with a message when a link
  names a page or a section that does not exist, or two pages share an id
  or a path: what the schema cannot see across documents.

  Everything about how a page looks is here and in style/; nothing about it
  is in the content.
-->
<xsl:stylesheet version="4.0"
                xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                xmlns:xs="http://www.w3.org/2001/XMLSchema"
                xmlns:s="https://www.tinyactors.dev/ns/site"
                xmlns:f="https://www.tinyactors.dev/ns/site/functions"
                exclude-result-prefixes="xs s f"
                expand-text="yes">

  <xsl:output method="html" html-version="5" indent="no"/>

  <xsl:variable name="importmap" as="xs:string" select="'{&quot;imports&quot;: {&quot;@tinyactors/wasm&quot;: &quot;/vendor/tinyactors-wasm/index.mjs&quot;, &quot;@tinyactors/scxmljs&quot;: &quot;/vendor/scxmljs/dist/index.js&quot;, &quot;@tinyactors/scxmljs/view&quot;: &quot;/vendor/scxmljs/dist/view.js&quot;, &quot;@tinyactors/scxmljs/trusted&quot;: &quot;/vendor/scxmljs/dist/trusted.js&quot;, &quot;quickjs-emscripten-core&quot;: &quot;/vendor/quickjs-emscripten-core/dist/index.mjs&quot;, &quot;@jitl/quickjs-singlefile-browser-release-sync&quot;: &quot;/vendor/quickjs-singlefile-browser-release-sync/dist/index.mjs&quot;, &quot;@jitl/quickjs-ffi-types&quot;: &quot;/vendor/quickjs-ffi-types/dist/index.mjs&quot;}}'"/>

  <!-- A hash of the stylesheet's and the scripts' sources (mise/website.rb). -->
  <xsl:param name="version" as="xs:string" select="'dev'"/>

  <xsl:variable name="site" select="/s:site"/>
  <xsl:variable name="origin" select="string($site/@origin)"/>
  <xsl:variable name="pages" as="element(s:page)*"
                select="$site/s:pages/s:page ! doc(resolve-uri(@src, base-uri($site)))/s:page"/>

  <!-- ===================================================== the site -->

  <xsl:template match="/">
    <xsl:call-template name="check"/>
    <xsl:for-each select="$pages">
      <xsl:result-document href="{f:file(@path)}" method="html" html-version="5">
        <xsl:apply-templates select="."/>
      </xsl:result-document>
    </xsl:for-each>
    <xsl:result-document href="sitemap.xml" method="xml" indent="yes">
      <urlset xmlns="http://www.sitemaps.org/schemas/sitemap/0.9">
        <xsl:for-each select="$pages[not(@path = '/404')]">
          <url><loc>{$origin}{@path}</loc></url>
        </xsl:for-each>
      </urlset>
    </xsl:result-document>
    <xsl:result-document href="llms.txt" method="text">
      <xsl:text># {$site/s:name}&#10;&#10;> {$site/s:tagline}&#10;&#10;## Pages&#10;&#10;</xsl:text>
      <xsl:for-each select="$pages[not(@path = '/404')]">
        <xsl:text>- [{s:title}]({$origin}{@path}): {normalize-space(s:description)}&#10;</xsl:text>
      </xsl:for-each>
      <xsl:text>&#10;## Elsewhere&#10;&#10;</xsl:text>
      <xsl:for-each select="$site/s:nav/s:link[@href]">
        <xsl:text>- [{normalize-space(.)}]({@href})&#10;</xsl:text>
      </xsl:for-each>
    </xsl:result-document>
  </xsl:template>

  <!-- What the schema cannot check: names across documents. -->
  <xsl:template name="check">
    <xsl:for-each-group select="$pages" group-by="@id">
      <xsl:if test="count(current-group()) gt 1">
        <xsl:message terminate="yes">site: two pages have the id "{current-grouping-key()}"</xsl:message>
      </xsl:if>
    </xsl:for-each-group>
    <xsl:for-each-group select="$pages" group-by="@path">
      <xsl:if test="count(current-group()) gt 1">
        <xsl:message terminate="yes">site: two pages have the path "{current-grouping-key()}"</xsl:message>
      </xsl:if>
    </xsl:for-each-group>
    <xsl:for-each select="($site, $pages)//(s:link, s:action, s:source)[@to]">
      <xsl:variable name="target" select="$pages[@id = current()/@to]"/>
      <xsl:if test="empty($target)">
        <xsl:message terminate="yes">site: {f:where(.)}: a link to the page "{@to}", which the site does not list</xsl:message>
      </xsl:if>
      <xsl:if test="@section and empty($target/s:section[@id = current()/@section])">
        <xsl:message terminate="yes">site: {f:where(.)}: a link to the section "{@section}", which the page "{@to}" does not have</xsl:message>
      </xsl:if>
    </xsl:for-each>
  </xsl:template>

  <!-- ===================================================== a page -->

  <xsl:template match="s:page">
    <html lang="en">
      <head>
        <meta charset="utf-8"/>
        <meta name="viewport" content="width=device-width, initial-scale=1"/>
        <title>{normalize-space(s:title)}</title>
        <meta name="description" content="{normalize-space(s:description)}"/>
        <xsl:if test="not(@path = '/404')">
          <link rel="canonical" href="{$origin}{@path}"/>
        </xsl:if>
        <meta property="og:type" content="website"/>
        <meta property="og:site_name" content="{$site/s:name}"/>
        <meta property="og:title" content="{normalize-space(s:title)}"/>
        <meta property="og:description" content="{normalize-space(s:description)}"/>
        <meta property="og:url" content="{$origin}{@path}"/>
        <meta name="color-scheme" content="light dark"/>
        <link rel="icon" href="/assets/brand/favicon.svg" type="image/svg+xml"/>
        <link rel="icon" href="/assets/brand/favicon.ico" sizes="any"/>
        <link rel="apple-touch-icon" href="/assets/brand/apple-touch-icon.png"/>
        <link rel="stylesheet" href="/assets/site.css?v={$version}"/>
        <!-- The vendored packages by their names, as a reader's code imports them (mise/website.rb, VENDOR). -->
        <script type="importmap">{$importmap}</script>
        <xsl:if test=".//s:install[count(s:command) gt 1]">
          <script type="module" src="/assets/components/tabs.mjs?v={$version}"></script>
        </xsl:if>
        <xsl:if test=".//s:code[@language = 'scxml' and @src]">
          <link rel="stylesheet" href="/vendor/scxmljs/dist/themes/tinyactors.css"/>
          <script type="module" src="/assets/components/chart.mjs?v={$version}"></script>
        </xsl:if>
        <xsl:if test=".//s:code[not(@role = 'output') and not(@language = 'text')]">
          <script type="module" src="/assets/components/highlight.mjs?v={$version}"></script>
        </xsl:if>
        <xsl:if test=".//s:live">
          <!-- tinyactors's WebAssembly, fetched while the page loads: the live examples need it before their first run. -->
          <link rel="preload" href="/vendor/tinyactors-wasm/tinyactors.wasm" as="fetch" type="application/wasm" crossorigin="anonymous"/>
          <script type="module" src="/assets/components/live.mjs?v={$version}"></script>
        </xsl:if>
        <!-- The design system's dark theme is [data-theme="dark"]; follow the reader's setting. -->
        <script>if (matchMedia('(prefers-color-scheme: dark)').matches) document.documentElement.dataset.theme = 'dark';</script>
      </head>
      <body>
        <a class="skip" href="#main">Skip to content</a>
        <xsl:call-template name="header">
          <xsl:with-param name="current" select="."/>
        </xsl:call-template>
        <main id="main">
          <xsl:apply-templates select="s:hero, s:section"/>
        </main>
        <xsl:call-template name="footer"/>
      </body>
    </html>
  </xsl:template>

  <xsl:template name="header">
    <xsl:param name="current" as="element(s:page)"/>
    <header class="site-header">
      <div class="wrap site-header__row">
        <a class="wordmark" href="/" aria-label="{$site/s:name}, home">
          <picture><source srcset="/assets/brand/logo-mark-dark.svg" media="(prefers-color-scheme: dark)"/><img src="/assets/brand/logo-mark.svg" alt="" width="28" height="28"/></picture>
          <span>{$site/s:name}</span>
        </a>
        <nav class="site-nav" aria-label="Site">
          <xsl:for-each select="$site/s:nav/s:link">
            <a href="{f:href(.)}">
              <xsl:if test="@to = $current/@id">
                <xsl:attribute name="aria-current">page</xsl:attribute>
              </xsl:if>
              <xsl:apply-templates select="." mode="text"/>
            </a>
          </xsl:for-each>
        </nav>
      </div>
    </header>
  </xsl:template>

  <xsl:template name="footer">
    <footer class="site-footer">
      <div class="wrap site-footer__row">
        <div class="site-footer__note">
          <picture><source srcset="/assets/brand/logo-mark-dark.svg" media="(prefers-color-scheme: dark)"/><img src="/assets/brand/logo-mark.svg" alt="" width="20" height="20"/></picture>
          <p><xsl:apply-templates select="$site/s:footer/s:note/node()"/></p>
        </div>
        <nav class="site-footer__links" aria-label="Elsewhere">
          <xsl:for-each select="$site/s:footer/s:links/s:link">
            <a href="{f:href(.)}"><xsl:apply-templates select="." mode="text"/></a>
          </xsl:for-each>
        </nav>
      </div>
    </footer>
  </xsl:template>

  <!-- ===================================================== the hero -->

  <!-- The hero is the page's one drafting sheet: a frame with registration
       marks on the construction grid. -->
  <xsl:template match="s:hero">
    <section class="hero" aria-labelledby="title">
      <div class="wrap">
        <div class="ta-sheet ta-sheet--grid hero__sheet">
          <div class="ta-sheet__frame">
            <xsl:for-each select="'tl', 'tr', 'bl', 'br'">
              <span class="ta-sheet__reg ta-sheet__reg--{.}" aria-hidden="true"><i/></span>
            </xsl:for-each>
            <div class="ta-sheet__body hero__body">
              <h1 id="title" class="hero__headline"><xsl:apply-templates select="s:headline/node()"/></h1>
              <p class="hero__lede"><xsl:apply-templates select="s:lede/node()"/></p>
              <xsl:apply-templates select="s:install, s:actions"/>
            </div>
          </div>
        </div>
      </div>
    </section>
  </xsl:template>

  <!-- ===================================================== sections -->

  <xsl:template match="s:section">
    <section id="{@id}" class="section" aria-labelledby="{@id}-heading">
      <div class="wrap">
        <header class="section__head">
          <h2 id="{@id}-heading"><xsl:apply-templates select="s:heading/node()"/></h2>
        </header>
        <div class="section__body">
          <xsl:apply-templates select="* except s:heading"/>
        </div>
      </div>
    </section>
  </xsl:template>

  <xsl:template match="s:section/s:p">
    <p class="prose"><xsl:apply-templates/></p>
  </xsl:template>

  <xsl:template match="s:list">
    <xsl:element name="{if (@ordered = 'true') then 'ol' else 'ul'}">
      <xsl:attribute name="class" select="'items', if (s:item/s:term) then 'items--terms' else ()"/>
      <xsl:for-each select="s:item">
        <li>
          <xsl:if test="s:term">
            <span class="items__term"><xsl:apply-templates select="s:term/node()"/></span>
          </xsl:if>
          <span class="items__text"><xsl:apply-templates select="s:text/node()"/></span>
        </li>
      </xsl:for-each>
    </xsl:element>
  </xsl:template>

  <!-- Code, in the design system's code block: a head naming the file (or
       "output"), then the text as written. -->
  <!-- A chart read from a file: its document, or the whole chart as an
       interactive diagram (scxmljs's <scxml-view>, made when the tab is
       first shown: components/chart.mjs). -->
  <xsl:template match="s:code[@language = 'scxml' and @src][parent::s:section or parent::s:example]" priority="2">
    <xsl:variable name="id" select="generate-id()"/>
    <ta-tabs class="ta-code code chart">
      <div class="ta-code__head">
        <span>{(@file, @language)[1]}</span>
        <div role="tablist" class="ta-tabs ta-tabs--segmented chart__views" aria-label="View">
          <button type="button" role="tab" class="ta-tab" id="{$id}-tab-1" aria-controls="{$id}-panel-1" aria-selected="true" tabindex="0">Document</button>
          <button type="button" role="tab" class="ta-tab" id="{$id}-tab-2" aria-controls="{$id}-panel-2" aria-selected="false" tabindex="-1">Diagram</button>
        </div>
      </div>
      <div role="tabpanel" id="{$id}-panel-1" aria-labelledby="{$id}-tab-1">
        <pre tabindex="0"><code data-language="{@language}">{f:code(.)}</code></pre>
      </div>
      <div role="tabpanel" id="{$id}-panel-2" aria-labelledby="{$id}-tab-2" class="chart__diagram" data-src="{f:url(@src)}" hidden="hidden"></div>
    </ta-tabs>
  </xsl:template>

  <xsl:template match="s:code[parent::s:section or parent::s:example]">
    <figure class="ta-code code{if (@role = 'output') then ' code--output' else ''}">
      <figcaption class="ta-code__head">
        <span>{if (@role = 'output') then 'output' else (@file, @language)[1]}</span>
        <xsl:if test="@file and not(@role = 'output')">
          <span class="code__language">{@language}</span>
        </xsl:if>
      </figcaption>
      <pre tabindex="0"><code data-language="{@language}">{f:code(.)}</code></pre>
    </figure>
  </xsl:template>

  <!-- A live example: the button runs the module's export; until the
       components load, it stays disabled. -->
  <xsl:template match="s:live">
    <ta-live class="live" src="{f:url(@src)}" call="{@call}">
      <p class="live__run">
        <button type="button" class="ta-btn ta-btn--primary" disabled="disabled">{normalize-space(s:label)}</button>
        <span class="live__status" aria-live="polite"></span>
      </p>
      <figure class="ta-code code code--output live__output" hidden="hidden">
        <figcaption class="ta-code__head"><span>output</span><span class="code__language">in your browser</span></figcaption>
        <pre tabindex="0"><code></code></pre>
      </figure>
    </ta-live>
  </xsl:template>

  <!-- A table transcluded from a guide: its first row is the head. -->
  <xsl:template match="s:table">
    <xsl:variable name="rows" select="f:table(.)"/>
    <figure class="table">
      <div class="table__scroll" tabindex="0">
        <table>
          <thead>
            <tr><xsl:for-each select="f:cells($rows[1])"><th scope="col"><xsl:sequence select="f:inline(.)"/></th></xsl:for-each></tr>
          </thead>
          <tbody>
            <xsl:for-each select="$rows[position() gt 2]">
              <tr>
                <xsl:for-each select="f:cells(.)">
                  <xsl:choose>
                    <xsl:when test="position() = 1"><th scope="row"><xsl:sequence select="f:inline(.)"/></th></xsl:when>
                    <xsl:otherwise><td><xsl:sequence select="f:inline(.)"/></td></xsl:otherwise>
                  </xsl:choose>
                </xsl:for-each>
              </tr>
            </xsl:for-each>
          </tbody>
        </table>
      </div>
      <xsl:if test="s:caption">
        <figcaption class="table__caption"><xsl:apply-templates select="s:caption/node()"/></figcaption>
      </xsl:if>
    </figure>
  </xsl:template>

  <xsl:template match="s:example">
    <div class="example">
      <xsl:apply-templates select="s:code"/>
      <xsl:if test="s:caption">
        <p class="example__caption"><xsl:apply-templates select="s:caption/node()"/></p>
      </xsl:if>
    </div>
  </xsl:template>

  <!-- Claims: each a card with its statement, why, and where to check it. -->
  <xsl:template match="s:claims">
    <div class="claims">
      <xsl:for-each select="s:claim">
        <article class="ta-card claim">
          <div class="ta-card__body">
            <h3 class="claim__statement"><xsl:apply-templates select="s:statement/node()"/></h3>
            <xsl:for-each select="s:p">
              <p class="claim__text"><xsl:apply-templates/></p>
            </xsl:for-each>
            <p class="claim__sources">
              <span class="claim__label">Source:</span>
              <xsl:for-each select="s:source">
                <xsl:if test="position() gt 1"><span class="claim__sep" aria-hidden="true"> · </span></xsl:if>
                <a href="{f:href(.)}"><xsl:apply-templates/></a>
              </xsl:for-each>
            </p>
          </div>
        </article>
      </xsl:for-each>
    </div>
  </xsl:template>

  <!-- Install commands do one thing in different words: one shows at a
       time, behind tabs (components/tabs.mjs); without its script the
       first shows. A single command needs no tabs. -->
  <xsl:template match="s:install">
    <xsl:variable name="id" select="generate-id()"/>
    <ta-tabs class="install">
      <xsl:if test="count(s:command) gt 1">
        <div role="tablist" class="ta-tabs ta-tabs--segmented" aria-label="Install for">
          <xsl:for-each select="s:command">
            <button type="button" role="tab" class="ta-tab" id="{$id}-tab-{position()}" aria-controls="{$id}-panel-{position()}"
                    aria-selected="{position() = 1}" tabindex="{if (position() = 1) then 0 else -1}">{@for}</button>
          </xsl:for-each>
        </div>
      </xsl:if>
      <xsl:for-each select="s:command">
        <div class="install__panel" role="{if (count(../s:command) gt 1) then 'tabpanel' else 'group'}" id="{$id}-panel-{position()}"
             aria-labelledby="{$id}-tab-{position()}">
          <xsl:if test="count(../s:command) = 1"><xsl:attribute name="aria-label" select="'Install for ' || @for"/></xsl:if>
          <xsl:if test="position() gt 1"><xsl:attribute name="hidden" select="'hidden'"/></xsl:if>
          <code class="install__command"><span class="install__prompt" aria-hidden="true">$ </span>{normalize-space(.)}</code>
          <xsl:if test="@guide">
            <a class="install__guide" href="{@guide}">{@for} guide</a>
          </xsl:if>
        </div>
      </xsl:for-each>
    </ta-tabs>
  </xsl:template>

  <!-- The first action is the one the page wants taken: the primary button. -->
  <xsl:template match="s:actions">
    <p class="actions">
      <xsl:for-each select="s:action">
        <a class="ta-btn ta-btn--lg {if (position() = 1) then 'ta-btn--primary' else 'ta-btn--secondary'}" href="{f:href(.)}">
          <xsl:apply-templates select="." mode="text"/>
        </a>
      </xsl:for-each>
    </p>
  </xsl:template>

  <!-- ===================================================== inline -->

  <xsl:template match="s:em"><em><xsl:apply-templates/></em></xsl:template>
  <xsl:template match="s:strong"><strong><xsl:apply-templates/></strong></xsl:template>
  <xsl:template match="s:code"><code><xsl:apply-templates/></code></xsl:template>
  <xsl:template match="s:link">
    <a href="{f:href(.)}"><xsl:apply-templates select="." mode="text"/></a>
  </xsl:template>

  <!-- A link's text: its own, or the title of the page it names. -->
  <xsl:template match="s:link | s:action" mode="text">
    <xsl:choose>
      <xsl:when test="normalize-space(.) != ''"><xsl:apply-templates/></xsl:when>
      <xsl:otherwise>{normalize-space($pages[@id = current()/@to]/s:title)}</xsl:otherwise>
    </xsl:choose>
  </xsl:template>

  <!-- ===================================================== functions -->

  <!-- Where a page is written: "/" and "/x/" as index.html in their
       directory, "/x" as x.html. -->
  <xsl:function name="f:file" as="xs:string">
    <xsl:param name="path" as="xs:string"/>
    <xsl:sequence select="if (ends-with($path, '/')) then substring($path, 2) || 'index.html' else substring($path, 2) || '.html'"/>
  </xsl:function>

  <!-- Where the site serves a file of examples/. -->
  <xsl:function name="f:url" as="xs:string">
    <xsl:param name="src" as="xs:string"/>
    <xsl:sequence select="replace($src, '^.*?examples/', '/examples/')"/>
  </xsl:function>

  <xsl:function name="f:href" as="xs:string">
    <xsl:param name="link" as="element()"/>
    <xsl:sequence select="if ($link/@href) then string($link/@href)
                          else $pages[@id = $link/@to]/@path || (if ($link/@section) then '#' || $link/@section else '')"/>
  </xsl:function>

  <!-- A code block's text: the file `src` names, or its own text with the
       blank first and last lines and the common indentation removed. -->
  <xsl:function name="f:code" as="xs:string">
    <xsl:param name="code" as="element(s:code)"/>
    <xsl:variable name="text" select="if ($code/@fence) then f:fence($code)
                                      else if ($code/@src) then unparsed-text(resolve-uri($code/@src, base-uri($code)))
                                      else string($code)"/>
    <xsl:variable name="all" select="tokenize(replace(replace($text, '^\s*\n', ''), '\n\s*$', ''), '\n')"/>
    <xsl:variable name="first" select="if ($code/@lines) then xs:integer(substring-before($code/@lines, '-')) else 1"/>
    <xsl:variable name="last" select="if ($code/@lines) then xs:integer(substring-after($code/@lines, '-')) else count($all)"/>
    <xsl:if test="$last gt count($all)">
      <xsl:message terminate="yes">site: {f:where($code)}: lines {$code/@lines} of {$code/@src}, which has {count($all)}</xsl:message>
    </xsl:if>
    <xsl:variable name="lines" select="$all[position() ge $first and position() le $last]"/>
    <xsl:variable name="indent" select="min($lines[normalize-space(.) != ''] ! string-length(replace(., '^( *).*$', '$1')))"/>
    <xsl:sequence select="string-join($lines ! substring(., $indent + 1), '&#10;')"/>
  </xsl:function>

  <!-- ===================================================== Markdown
       What the site transcludes from the repository's guides. A section
       runs from its "## heading" line to the next "## " line; a fence is
       the lines between two "```" lines; a table is a run of lines that
       start with "|". mise/tasks/test reads them the same way. -->

  <xsl:function name="f:section" as="xs:string*">
    <xsl:param name="node" as="element()"/>
    <xsl:variable name="lines" select="tokenize(unparsed-text(resolve-uri($node/@src, base-uri($node))), '\r?\n')"/>
    <xsl:variable name="start" select="index-of($lines, '## ' || $node/@section)[1]"/>
    <xsl:if test="empty($start)">
      <xsl:message terminate="yes">site: {f:where($node)}: {$node/@src} has no section "## {$node/@section}"</xsl:message>
    </xsl:if>
    <xsl:variable name="after" select="subsequence($lines, $start + 1)"/>
    <xsl:variable name="end" select="(index-of($after ! starts-with(., '## '), true())[1], count($after) + 1)[1]"/>
    <xsl:sequence select="subsequence($after, 1, $end - 1)"/>
  </xsl:function>

  <xsl:function name="f:fence" as="xs:string">
    <xsl:param name="code" as="element(s:code)"/>
    <xsl:variable name="lines" select="f:section($code)"/>
    <xsl:variable name="marks" select="index-of($lines ! starts-with(., '```'), true())"/>
    <xsl:variable name="n" select="xs:integer($code/@fence)"/>
    <xsl:if test="count($marks) lt 2 * $n">
      <xsl:message terminate="yes">site: {f:where($code)}: "## {$code/@section}" of {$code/@src} has no fence {$n}</xsl:message>
    </xsl:if>
    <xsl:sequence select="string-join(subsequence($lines, $marks[2 * $n - 1] + 1, $marks[2 * $n] - $marks[2 * $n - 1] - 1), '&#10;')"/>
  </xsl:function>

  <xsl:function name="f:table" as="xs:string*">
    <xsl:param name="table" as="element(s:table)"/>
    <xsl:variable name="lines" select="f:section($table)"/>
    <xsl:variable name="row" select="$lines ! starts-with(., '|')"/>
    <xsl:variable name="starts" select="for $i in 1 to count($lines) return (if ($row[$i] and ($i = 1 or not($row[$i - 1]))) then $i else ())"/>
    <xsl:variable name="n" select="xs:integer(($table/@index, 1)[1])"/>
    <xsl:if test="count($starts) lt $n">
      <xsl:message terminate="yes">site: {f:where($table)}: "## {$table/@section}" of {$table/@src} has no table {$n}</xsl:message>
    </xsl:if>
    <xsl:variable name="from" select="$starts[$n]"/>
    <xsl:variable name="rest" select="subsequence($row, $from)"/>
    <xsl:variable name="length" select="(index-of($rest, false())[1] - 1, count($rest))[1]"/>
    <xsl:sequence select="subsequence($lines, $from, $length)"/>
  </xsl:function>

  <xsl:function name="f:cells" as="xs:string*">
    <xsl:param name="row" as="xs:string"/>
    <xsl:sequence select="tokenize(replace($row, '^\s*\|\s*|\s*\|\s*$', ''), '\s*\|\s*')"/>
  </xsl:function>

  <!-- A cell's Markdown: `code` and **strong** kept, a link's text only. -->
  <xsl:function name="f:inline" as="node()*">
    <xsl:param name="text" as="xs:string"/>
    <xsl:analyze-string select="$text" regex="`([^`]+)`|\*\*([^*]+)\*\*|\[([^\]]+)\]\([^)]+\)">
      <xsl:matching-substring>
        <xsl:choose>
          <xsl:when test="regex-group(1)"><code>{regex-group(1)}</code></xsl:when>
          <xsl:when test="regex-group(2)"><strong>{regex-group(2)}</strong></xsl:when>
          <xsl:otherwise>{regex-group(3)}</xsl:otherwise>
        </xsl:choose>
      </xsl:matching-substring>
      <xsl:non-matching-substring>{.}</xsl:non-matching-substring>
    </xsl:analyze-string>
  </xsl:function>

  <xsl:function name="f:where" as="xs:string">
    <xsl:param name="node" as="element()"/>
    <xsl:sequence select="replace(base-uri($node), '^.*/content/', 'content/')"/>
  </xsl:function>
</xsl:stylesheet>
