<?xml version="1.0" encoding="UTF-8"?>
<!--
  The site, from its content (content/site.xml and the pages it lists) to
  HTML in the design system (style/site.css over the design system's
  styles.css). Run on content/site.xml with a result directory:

    sxml transform -\-result-directory=build/site render/site.xsl content/site.xml

  It writes one HTML file per page (a path "/x/" as x/index.html, "/404" as
  404.html), sitemap.xml, and the site for agents (render/markdown.xsl):
  each page as Markdown beside its HTML, llms.txt and llms-full.txt; and
  each audience as /audiences/<id>.xml with their index. It stops with a
  message when a link names a page or a section that does not exist, two
  pages share an id or a path, or a purpose or a section names an audience
  or a need site.xml does not define: what the schema cannot see across
  documents.

  Everything about how a page looks is here and in style/; nothing about it
  is in the content.
-->
<xsl:stylesheet version="4.0"
                xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                xmlns:xs="http://www.w3.org/2001/XMLSchema"
                xmlns:s="https://www.tinyactors.dev/ns/site"
                xmlns:f="https://www.tinyactors.dev/ns/site/functions"
                xmlns:map="http://www.w3.org/2005/xpath-functions/map"
                exclude-result-prefixes="xs s f map"
                expand-text="yes">

  <xsl:include href="markdown.xsl"/>

  <xsl:output method="html" html-version="5" indent="no"/>

  <xsl:variable name="importmap" as="xs:string" select="'{&quot;imports&quot;: {&quot;@tinyactors/wasm&quot;: &quot;/vendor/tinyactors-wasm/index.mjs&quot;, &quot;@tinyactors/scxmljs&quot;: &quot;/vendor/scxmljs/dist/index.js&quot;, &quot;@tinyactors/scxmljs/view&quot;: &quot;/vendor/scxmljs/dist/view.js&quot;, &quot;@tinyactors/scxmljs/trusted&quot;: &quot;/vendor/scxmljs/dist/trusted.js&quot;, &quot;quickjs-emscripten-core&quot;: &quot;/vendor/quickjs-emscripten-core/dist/index.mjs&quot;, &quot;@jitl/quickjs-singlefile-browser-release-sync&quot;: &quot;/vendor/quickjs-singlefile-browser-release-sync/dist/index.mjs&quot;, &quot;@jitl/quickjs-ffi-types&quot;: &quot;/vendor/quickjs-ffi-types/dist/index.mjs&quot;}}'"/>

  <!-- A hash of the stylesheet's and the scripts' sources (mise/website.rb). -->
  <xsl:param name="version" as="xs:string" select="'dev'"/>

  <!-- The CLI section's capture (mise/cli.rb): the released tinyactors
       command's help and its examples' runs, build/cli/cli.xml. Only a
       site whose content lists the commands (<commands>) needs it. -->
  <xsl:param name="cli" as="xs:string" select="''"/>

  <xsl:variable name="site" select="/s:site"/>
  <xsl:variable name="origin" select="string($site/@origin)"/>
  <xsl:variable name="content-pages" as="element(s:page)*"
                select="$site/s:pages/s:page ! doc(resolve-uri(@src, base-uri($site)))/s:page"/>
  <xsl:variable name="commands" as="element(s:commands)?" select="($content-pages//s:commands)[1]"/>
  <xsl:variable name="cli-doc" as="element(cli)?" select="if (exists($commands) and $cli != '') then doc($cli)/cli else ()"/>
  <!-- Every page: the content's, then a page per command of the capture. -->
  <xsl:variable name="pages" as="element(s:page)*" select="$content-pages, f:command-pages()"/>

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
    <xsl:call-template name="markdown"/>
    <xsl:call-template name="audiences"/>
  </xsl:template>

  <!-- What the schema cannot check: names across documents. -->
  <xsl:template name="check">
    <xsl:if test="count($content-pages//s:commands) gt 1">
      <xsl:message terminate="yes">site: two pages list the command's commands (&lt;commands&gt;); one does, and its command pages are under its path</xsl:message>
    </xsl:if>
    <xsl:if test="exists($commands) and empty($cli-doc)">
      <xsl:message terminate="yes">site: {f:where($commands)}: lists the command's commands, and the build gave no capture of the command (the parameter cli: mise/cli.rb)</xsl:message>
    </xsl:if>
    <xsl:if test="$commands/s:job">
      <xsl:for-each select="$cli-doc/command/@name[not(. = $commands/s:job/tokenize(normalize-space(@commands), ' '))]">
        <xsl:message terminate="yes">site: {f:where($commands)}: the command "{.}" is in no &lt;job&gt;: name the job it does</xsl:message>
      </xsl:for-each>
      <xsl:for-each select="$commands/s:job/tokenize(normalize-space(@commands), ' ')[not(. = $cli-doc/command/@name)]">
        <xsl:message terminate="yes">site: {f:where($commands)}: a &lt;job&gt; names the command "{.}", which the tinyactors {$cli-doc/@version} the site documents does not have</xsl:message>
      </xsl:for-each>
    </xsl:if>
    <xsl:for-each select="$commands/(s:related, s:artifact)[not(@command = $cli-doc/command/@name)]">
      <xsl:message terminate="yes">site: {f:where(.)}: the command "{@command}", which the tinyactors {$cli-doc/@version} the site documents does not have</xsl:message>
    </xsl:for-each>
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
      <!-- a section, or a group the commands page makes (a job, the shared options) -->
      <xsl:if test="@section and empty($target/s:section[@id = current()/@section])
                    and not($target//s:commands and current()/@section = ($commands/s:job/@id, 'program-options', 'commands'))">
        <xsl:message terminate="yes">site: {f:where(.)}: a link to the section "{@section}", which the page "{@to}" does not have</xsl:message>
      </xsl:if>
    </xsl:for-each>
    <xsl:variable name="audiences" select="$site/s:audiences/s:audience"/>
    <xsl:for-each select="$pages/s:purpose">
      <xsl:variable name="purpose" select="."/>
      <xsl:for-each select="tokenize(@for)[not(. = $audiences/@id)]">
        <xsl:message terminate="yes">site: {f:where($purpose)}: the audience "{.}", which site.xml does not define</xsl:message>
      </xsl:for-each>
    </xsl:for-each>
    <xsl:for-each select="$pages//(s:purpose, s:section)[@answers]">
      <xsl:variable name="at" select="."/>
      <xsl:for-each select="tokenize(@answers)[not(. = f:needs($audiences))]">
        <xsl:message terminate="yes">site: {f:where($at)}: the need "{.}", which site.xml's audiences do not define</xsl:message>
      </xsl:for-each>
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
          <link rel="alternate" type="text/markdown" href="{f:md-path(@path)}"/>
        </xsl:if>
        <meta property="og:type" content="website"/>
        <meta property="og:site_name" content="{$site/s:name}"/>
        <meta property="og:title" content="{normalize-space(s:title)}"/>
        <meta property="og:description" content="{normalize-space(s:description)}"/>
        <meta property="og:url" content="{$origin}{@path}"/>
        <meta name="color-scheme" content="light dark"/>
        <!-- What the page is for, and whom (README.md, "Audiences and page kinds"). -->
        <xsl:for-each select="s:purpose">
          <meta name="page-kind" content="{@kind}"/>
          <xsl:for-each select="tokenize(@for)">
            <link rel="audience" href="/audiences/{.}.xml"/>
          </xsl:for-each>
        </xsl:for-each>
        <link rel="icon" href="/assets/brand/favicon.svg" type="image/svg+xml"/>
        <link rel="icon" href="/assets/brand/favicon.ico" sizes="any"/>
        <link rel="apple-touch-icon" href="/assets/brand/apple-touch-icon.png"/>
        <link rel="stylesheet" href="/assets/site.css?v={$version}"/>
        <!-- The vendored packages by their names, as a reader's code imports them (mise/website.rb, VENDOR). -->
        <script type="importmap">{$importmap}</script>
        <xsl:if test=".//s:install[count(s:command) gt 1] or .//s:changelog or .//s:program">
          <script type="module" src="/assets/components/tabs.mjs?v={$version}"></script>
        </xsl:if>
        <xsl:if test=".//s:code[@language = 'scxml' and @src]">
          <link rel="stylesheet" href="/vendor/scxmljs/dist/themes/tinyactors.css"/>
          <script type="module" src="/assets/components/chart.mjs?v={$version}"></script>
        </xsl:if>
        <xsl:if test=".//s:code[not(@language = 'text')] or .//s:commands">
          <script type="module" src="/assets/components/highlight.mjs?v={$version}"></script>
        </xsl:if>
        <xsl:if test=".//s:code[parent::s:section or parent::s:example or parent::s:program or parent::s:in] or .//s:live or .//s:install">
          <script type="module" src="/assets/components/copy.mjs?v={$version}"></script>
        </xsl:if>
        <xsl:if test=".//s:demo">
          <script type="module" src="/assets/components/demo.mjs?v={$version}"></script>
        </xsl:if>
        <xsl:if test=".//s:code[@role = 'output'][@language = $output-formats]">
          <script type="module" src="/assets/components/output.mjs?v={$version}"></script>
        </xsl:if>
        <xsl:if test=".//s:playground">
          <script type="module" src="/assets/components/playground.mjs?v={$version}"></script>
        </xsl:if>
        <xsl:if test=".//s:live">
          <!-- tinyactors's WebAssembly, fetched while the page loads: the live examples need it before their first run. -->
          <link rel="preload" href="/vendor/tinyactors-wasm/tinyactors.wasm" as="fetch" type="application/wasm" crossorigin="anonymous"/>
          <script type="module" src="/assets/components/live.mjs?v={$version}"></script>
        </xsl:if>
        <!-- The design system's dark theme is [data-theme="dark"]; follow the reader's setting. -->
        <script>if (matchMedia('(prefers-color-scheme: dark)').matches) document.documentElement.dataset.theme = 'dark';</script>
        <!-- The copywriter's widget, on a device that turned it on at /copywriter/ (mise/website.rb, copywriter!). -->
        <script type="module" xsl:expand-text="no">try{if(localStorage.getItem('tinyactors-copywriter')==='on')import('/copywriter/embed.js')}catch(e){}</script>
      </head>
      <!-- A page that is one application fills the window: its body has the class style/site.css lays out as an app page. -->
      <body>
        <xsl:if test="s:app"><xsl:attribute name="class">page--app</xsl:attribute></xsl:if>
        <a class="skip" href="#main">Skip to content</a>
        <xsl:call-template name="header">
          <xsl:with-param name="current" select="."/>
        </xsl:call-template>
        <main id="main">
          <xsl:choose>
            <xsl:when test="s:command-nav">
              <xsl:apply-templates select="s:hero"/>
              <div class="wrap command-layout">
                <xsl:apply-templates select="s:command-nav" mode="sidebar"/>
                <div class="command-layout__body">
                  <xsl:apply-templates select="s:section"/>
                  <xsl:apply-templates select="s:command-nav" mode="steps"/>
                </div>
              </div>
            </xsl:when>
            <xsl:otherwise>
              <xsl:apply-templates select="s:hero, s:section, s:reference, s:app"/>
            </xsl:otherwise>
          </xsl:choose>
        </main>
        <xsl:call-template name="footer"/>
        <xsl:apply-templates select="$site/s:presence">
          <xsl:with-param name="current" select="."/>
        </xsl:apply-templates>
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
          <xsl:call-template name="nav-links">
            <xsl:with-param name="current" select="$current"/>
            <xsl:with-param name="menu" select="false()"/>
          </xsl:call-template>
        </nav>
        <!-- On a phone the links do not fit beside the wordmark: they open
             from a menu, so the sticky header stays one row. The ant button
             (presence) opens the same links, but floats where a reader may
             not look for them. -->
        <button class="site-menu" type="button" popovertarget="site-menu">Menu</button>
        <nav class="site-menu__links" id="site-menu" popover="" aria-label="Site">
          <xsl:call-template name="nav-links">
            <xsl:with-param name="current" select="$current"/>
          </xsl:call-template>
        </nav>
      </div>
    </header>
  </xsl:template>

  <!-- The navigation. The header's row (menu false) shows a group as one
       link, its label to its first page; the menu shows it as a
       disclosure that opens to the group's links in place, open already
       on one of its pages. -->
  <xsl:template name="nav-links">
    <xsl:param name="current" as="element(s:page)"/>
    <xsl:param name="menu" as="xs:boolean" select="true()"/>
    <xsl:for-each select="$site/s:nav/*">
      <xsl:choose>
        <xsl:when test="self::s:link">
          <xsl:call-template name="nav-link">
            <xsl:with-param name="current" select="$current"/>
          </xsl:call-template>
        </xsl:when>
        <xsl:when test="not($menu)">
          <a href="{f:href(s:link[1])}">
            <xsl:if test="f:in-group(., $current)">
              <xsl:attribute name="aria-current">page</xsl:attribute>
            </xsl:if>
            {normalize-space(@label)}
          </a>
        </xsl:when>
        <xsl:otherwise>
          <details class="nav-group">
            <xsl:if test="f:in-group(., $current)">
              <xsl:attribute name="open" select="'open'"/>
            </xsl:if>
            <summary><a class="nav-group__label" href="{f:href(s:link[1])}">
              <xsl:if test="f:in-group(., $current)"><xsl:attribute name="aria-current" select="'page'"/></xsl:if>{normalize-space(@label)}</a></summary>
            <div class="nav-group__links">
              <xsl:for-each select="s:link">
                <xsl:call-template name="nav-link">
                  <xsl:with-param name="current" select="$current"/>
                </xsl:call-template>
              </xsl:for-each>
            </div>
          </details>
        </xsl:otherwise>
      </xsl:choose>
    </xsl:for-each>
  </xsl:template>

  <!-- A page is in a group of the navigation when the group links to it, or
       when its path is under the group's first page's (a command's page
       under /cli/, a guide under /guides/). -->
  <xsl:function name="f:in-group" as="xs:boolean">
    <xsl:param name="group" as="element(s:group)"/>
    <xsl:param name="current" as="element(s:page)"/>
    <xsl:variable name="index" select="string(f:href($group/s:link[1]))"/>
    <xsl:sequence select="$group/s:link/@to = $current/@id or ($index != '/' and starts-with(string($current/@path), $index))"/>
  </xsl:function>

  <xsl:template name="nav-link">
    <xsl:param name="current" as="element(s:page)"/>
    <a href="{f:href(.)}">
      <xsl:if test="@to = $current/@id">
        <xsl:attribute name="aria-current">page</xsl:attribute>
      </xsl:if>
      <xsl:apply-templates select="." mode="text"/>
    </a>
  </xsl:template>

  <!-- Presence (docs/presence.md): the ant button, which opens the site's
       navigation, the switch for this tab's ant and the greeting line;
       presence/client.mjs runs it. Its words are the content's, handed to
       the script as data attributes. -->
  <xsl:template match="s:presence">
    <xsl:param name="current" as="element(s:page)"/>
    <div class="presence" data-server="{@server}" data-greeted="{normalize-space(s:greeted)}"
         data-greeted-anonymous="{normalize-space(s:greeted-anonymous)}" data-count="{normalize-space(s:count)}">
      <p class="presence__toast" role="status" hidden=""></p>
      <button class="presence__button" type="button" popovertarget="presence-menu" aria-label="{normalize-space(s:label)}">
        <picture><source srcset="/assets/brand/logo-mark-dark.svg" media="(prefers-color-scheme: dark)"/><img src="/assets/brand/logo-mark.svg" alt="" width="30" height="30"/></picture>
      </button>
      <div class="presence__menu" id="presence-menu" popover="">
        <nav class="presence__links" aria-label="Site">
          <xsl:call-template name="nav-links">
            <xsl:with-param name="current" select="$current"/>
          </xsl:call-template>
        </nav>
        <div class="presence__controls">
          <!-- One row: the switch, the greeting, and the count as the ant
               mark and a number (its sentence is its accessible name). -->
          <div class="presence__row">
            <button class="presence__switch" type="button" role="switch" aria-checked="true">
              <span class="presence__pill" aria-hidden="true"></span>
              <span>{normalize-space(s:switch)}</span>
            </button>
            <button class="presence__greet" type="button">{normalize-space(s:greet)}</button>
            <span class="presence__count" role="img" hidden="">
              <picture><source srcset="/assets/brand/logo-mark-dark.svg" media="(prefers-color-scheme: dark)"/><img src="/assets/brand/logo-mark.svg" alt="" width="16" height="16"/></picture>
              <span class="presence__number"></span>
            </span>
          </div>
          <p class="presence__status"></p>
        </div>
      </div>
    </div>
    <script type="module" src="/assets/presence/client.mjs?v={$version}"></script>
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
              <xsl:apply-templates select="s:author, s:install, s:actions"/>
            </div>
          </div>
        </div>
      </div>
    </section>
  </xsl:template>

  <!-- An application page: the tool's name for screen readers, then the
       application, full-bleed below the site's header. -->
  <xsl:template match="s:app">
    <section class="app" aria-labelledby="title">
      <h1 id="title" class="app__name"><xsl:apply-templates select="s:name/node()"/></h1>
      <xsl:apply-templates select="s:playground"/>
    </section>
  </xsl:template>

  <!-- A reference page: its name, the facts the reader came for, then
       the blocks, with no hero to scroll past. -->
  <xsl:template match="s:reference">
    <section class="reference" aria-labelledby="title">
      <div class="wrap">
        <header class="reference__head">
          <h1 id="title" class="reference__headline"><xsl:apply-templates select="s:headline/node()"/></h1>
          <xsl:apply-templates select="s:versions"/>
        </header>
        <div class="section__body">
          <xsl:apply-templates select="* except (s:headline, s:versions)"/>
        </div>
      </div>
    </section>
  </xsl:template>

  <!-- Each package's newest version, linking to its release below. -->
  <xsl:template match="s:versions">
    <ul class="versions">
      <xsl:for-each select="$site/s:packages/s:package">
        <xsl:variable name="release" select="f:releases(.)[1]"/>
        <li><a href="#{@id}-{translate($release?version, '.', '-')}"><xsl:sequence select="f:platform(.)"/><code>{.} {$release?version}</code></a></li>
      </xsl:for-each>
    </ul>
  </xsl:template>

  <!-- Who wrote the page: the portrait beside the name, which links to
       where to find them. The portrait's alt is empty: the name is next
       to it. -->
  <xsl:template match="s:author">
    <p class="hero__author">
      <img src="{replace(@src, '^\.\./images/', '/assets/images/')}" alt="" width="48" height="48"/>
      <a href="{@href}">{normalize-space(.)}</a>
    </p>
  </xsl:template>

  <!-- ===================================================== audiences -->

  <!-- Each audience of site.xml as /audiences/<id>.xml, and their index,
       in the format of schema/audience.xsd (copied beside them by the
       build): what a page's <link rel="audience"> names. -->
  <xsl:template name="audiences">
    <xsl:for-each select="$site/s:audiences/s:audience">
      <xsl:result-document href="audiences/{@id}.xml" method="xml" indent="yes">
        <audience xmlns="https://www.tinyactors.dev/ns/audience" id="{@id}" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:schemaLocation="https://www.tinyactors.dev/ns/audience audience.xsd">
          <xsl:for-each select="s:name, s:description, s:knows, s:want, s:task, s:objection">
            <xsl:element name="{local-name()}" namespace="https://www.tinyactors.dev/ns/audience">
              <xsl:copy-of select="@id"/>
              <xsl:value-of select="normalize-space(.)"/>
            </xsl:element>
          </xsl:for-each>
        </audience>
      </xsl:result-document>
    </xsl:for-each>
    <xsl:result-document href="audiences/index.xml" method="xml" indent="yes">
      <audiences xmlns="https://www.tinyactors.dev/ns/audience" xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xsi:schemaLocation="https://www.tinyactors.dev/ns/audience audience.xsd">
        <xsl:for-each select="$site/s:audiences/s:audience">
          <item id="{@id}" href="{@id}.xml">{normalize-space(s:name)}</item>
        </xsl:for-each>
      </audiences>
    </xsl:result-document>
  </xsl:template>

  <!-- ===================================================== sections -->

  <xsl:template match="s:section">
    <section id="{@id}" class="section" aria-labelledby="{@id}-heading">
      <div class="wrap">
        <header class="section__head">
          <h2 id="{@id}-heading"><a class="section__anchor" href="#{@id}"><xsl:apply-templates select="s:heading/node()"/></a></h2>
        </header>
        <div class="section__body">
          <xsl:apply-templates select="* except s:heading"/>
        </div>
      </div>
    </section>
  </xsl:template>

  <xsl:template match="s:section/s:p | s:reference/s:p">
    <p class="prose"><xsl:apply-templates/></p>
  </xsl:template>

  <!-- A problem: what the section solves, set apart under its heading. -->
  <xsl:template match="s:section/s:problem">
    <p class="problem"><xsl:apply-templates/></p>
  </xsl:template>

  <!-- A passage: the deep-dive voice's paragraph, set for reading at length. -->
  <xsl:template match="s:section/s:passage">
    <p class="prose passage"><xsl:apply-templates/></p>
  </xsl:template>

  <xsl:template match="s:list">
    <xsl:element name="{if (@ordered = 'true') then 'ol' else 'ul'}">
      <!-- terms that are all code (options, commands) take the width they need -->
      <xsl:attribute name="class" select="'items', if (s:item/s:term) then 'items--terms' else (),
          if (s:item/s:term and (every $t in s:item/s:term satisfies normalize-space(string-join($t//s:code)) = normalize-space($t))) then 'items--code' else ()"/>
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
        <xsl:sequence select="f:copyable(f:pre(.))"/>
      </div>
      <div role="tabpanel" id="{$id}-panel-2" aria-labelledby="{$id}-tab-2" class="chart__diagram" data-src="{f:url(@src)}" hidden="hidden"></div>
    </ta-tabs>
  </xsl:template>

  <xsl:template match="s:code[parent::s:section or parent::s:example or parent::s:program]">
    <xsl:variable name="viewable" select="@role = 'output' and @language = $output-formats"/>
    <figure class="ta-code code{if (@role = 'output') then ' code--output' else ''}">
      <figcaption class="ta-code__head">
        <span>{if (@role = 'output') then 'output' else (@file, @language)[1]}</span>
        <xsl:if test="@file and not(@role = 'output')">
          <span class="code__language">{@language}</span>
        </xsl:if>
        <!-- an output in a format the page can show: its switch between the
             preview and the source (components/output.mjs shows it) -->
        <xsl:if test="$viewable">
          <span class="output__switch" role="group" aria-label="View" hidden="hidden">
            <button type="button" data-view="preview" aria-pressed="false">Preview</button>
            <button type="button" data-view="source" aria-pressed="true">Source</button>
          </span>
        </xsl:if>
      </figcaption>
      <xsl:choose>
        <xsl:when test="$viewable">
          <ta-output class="output" data-format="{@language}"><xsl:sequence select="f:copyable(f:pre(.))"/></ta-output>
        </xsl:when>
        <xsl:otherwise><xsl:sequence select="f:copyable(f:pre(.))"/></xsl:otherwise>
      </xsl:choose>
    </figure>
  </xsl:template>

  <!-- The formats an output can be previewed in (components/output.mjs). -->
  <xsl:variable name="output-formats" as="xs:string+" select="('markdown', 'json', 'jsonl', 'xml')"/>

  <!-- What a command's example prints: what its line asks for with
       format, the protocol's JSON lines of run, the SCXML fmt writes;
       else the command's default answer, Markdown. -->
  <xsl:function name="f:output-language" as="xs:string">
    <xsl:param name="line" as="xs:string"/>
    <xsl:param name="command" as="xs:string"/>
    <xsl:variable name="format" select="replace($line, '^.*(-f|--format)[ =]?([a-z]+).*$', '$2')[matches($line, '(-f|--format)[ =]?[a-z]+')]"/>
    <xsl:sequence select="if ($format = ('json', 'xml', 'markdown')) then $format
                          else if ($format = 'svg') then 'xml'
                          else if ($command = 'run') then 'jsonl'
                          else if ($command = 'fmt') then 'xml'
                          else 'markdown'"/>
  </xsl:function>

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
        <xsl:sequence select="f:copyable(f:pre(()))"/>
      </figure>
    </ta-live>
  </xsl:template>

  <!-- A table transcluded from a guide: its first row is the head. -->
  <!-- A table written in the content: the head, then each row, its
       first cell naming it. -->
  <xsl:template match="s:table[s:head]" priority="2">
    <figure class="table">
      <div class="table__scroll" tabindex="0">
        <table>
          <thead>
            <tr><xsl:for-each select="s:head/s:cell"><th scope="col"><xsl:apply-templates/></th></xsl:for-each></tr>
          </thead>
          <tbody>
            <xsl:for-each select="s:row">
              <tr>
                <xsl:for-each select="s:cell">
                  <xsl:choose>
                    <xsl:when test="position() = 1"><th scope="row"><xsl:apply-templates/></th></xsl:when>
                    <xsl:otherwise><td><xsl:apply-templates/></td></xsl:otherwise>
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

  <!-- A playground: the module mounts itself into the element. Without
       scripts, the description says what it shows. -->
  <xsl:template match="s:playground">
    <ta-playground class="playground" src="{f:url(@src)}">
      <p class="playground__description"><xsl:apply-templates select="s:description/node()"/></p>
    </ta-playground>
  </xsl:template>

  <!-- A figure: its SVG written into the page, so its classes take the
       design system's colors; the description is its caption and its
       accessible name. -->
  <xsl:template match="s:figure">
    <xsl:variable name="id" select="'figure-' || generate-id()"/>
    <figure class="figure">
      <div class="figure__scroll" tabindex="0">
        <xsl:for-each select="document(resolve-uri(@src, base-uri(.)))/*">
          <xsl:copy>
            <xsl:copy-of select="@* except (@width, @height)"/>
            <xsl:attribute name="role">img</xsl:attribute>
            <xsl:attribute name="aria-labelledby" select="$id"/>
            <xsl:copy-of select="node()"/>
          </xsl:copy>
        </xsl:for-each>
      </div>
      <figcaption class="figure__caption" id="{$id}"><xsl:apply-templates select="s:description/node()"/></figcaption>
    </figure>
  </xsl:template>

  <!-- A demo: the video, its caption, and its steps, each seeking the
       video (components/demo.mjs; without scripts, the times read as text). -->
  <xsl:template match="s:demo">
    <figure class="demo">
      <ta-demo class="demo__frame">
        <!-- H.264 for Safari and Chrome, VP9 for browsers without H.264 (Chromium builds on Linux) -->
        <video class="demo__video" controls="controls" preload="metadata" playsinline="playsinline"
               poster="{replace(@poster, '^\.\./videos/', '/assets/videos/')}">
          <source src="{replace(@src, '^\.\./videos/', '/assets/videos/')}" type="video/mp4"/>
          <source src="{replace(replace(@src, '^\.\./videos/', '/assets/videos/'), '\.mp4$', '.webm')}" type="video/webm"/>
        </video>
        <ol class="demo__steps">
          <xsl:for-each select="s:step">
            <xsl:variable name="seconds" select="xs:integer(substring-before(@at, ':')) * 60 + xs:integer(substring-after(@at, ':'))"/>
            <li><button type="button" class="demo__at" data-at="{$seconds}"><xsl:value-of select="@at"/></button>
              <span class="demo__text"><xsl:apply-templates/></span></li>
          </xsl:for-each>
        </ol>
      </ta-demo>
      <figcaption class="demo__caption"><xsl:apply-templates select="s:description/node()"/></figcaption>
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

  <!-- One program for several packages: a tab per package, whose choice
       is the install commands' (data-group: the reader's platform holds),
       and in each a tab per file, a chart's file followed by its diagram
       (components/chart.mjs); the file tabs choose for their block only
       (data-local). Then what the programs print. -->
  <xsl:template match="s:program">
    <xsl:variable name="id" select="generate-id()"/>
    <div class="example program">
      <ta-tabs class="program__packages">
        <xsl:if test="count(s:in) gt 1">
          <div role="tablist" class="ta-tabs ta-tabs--segmented" aria-label="Program for" data-group="Install for">
            <xsl:for-each select="s:in">
              <button type="button" role="tab" class="ta-tab" id="{$id}-tab-{position()}" aria-controls="{$id}-panel-{position()}"
                      aria-selected="{position() = 1}" tabindex="{if (position() = 1) then 0 else -1}"><xsl:sequence select="f:platform(f:package(.))"/></button>
            </xsl:for-each>
          </div>
        </xsl:if>
        <xsl:for-each select="s:in">
          <div class="program__panel" role="{if (count(../s:in) gt 1) then 'tabpanel' else 'group'}" id="{$id}-panel-{position()}"
               aria-labelledby="{$id}-tab-{position()}">
            <xsl:if test="count(../s:in) = 1"><xsl:attribute name="aria-label" select="'Program for ' || f:package(.)/@for"/></xsl:if>
            <xsl:if test="position() gt 1"><xsl:attribute name="hidden" select="'hidden'"/></xsl:if>
            <xsl:apply-templates select="." mode="files"/>
          </div>
        </xsl:for-each>
      </ta-tabs>
      <xsl:apply-templates select="s:code"/>
    </div>
  </xsl:template>

  <!-- A program's files for one package, a tab each; a chart's diagram is
       a tab after its file. -->
  <xsl:template match="s:in" mode="files">
    <xsl:variable name="tabs" as="map(*)*">
      <xsl:for-each select="s:code">
        <xsl:sequence select="map { 'id': generate-id(), 'label': string(@file), 'code': . }"/>
        <xsl:if test="@language = 'scxml'">
          <xsl:sequence select="map { 'id': generate-id() || '-diagram', 'label': 'Diagram', 'chart': . }"/>
        </xsl:if>
      </xsl:for-each>
    </xsl:variable>
    <ta-tabs class="ta-code code chart program__files" data-local="">
      <div class="ta-code__head">
        <div role="tablist" class="ta-tabs ta-tabs--segmented chart__views" aria-label="Files">
          <xsl:for-each select="$tabs">
            <button type="button" role="tab" class="ta-tab" id="{?id}-tab" aria-controls="{?id}-panel"
                    aria-selected="{position() = 1}" tabindex="{if (position() = 1) then 0 else -1}">{?label}</button>
          </xsl:for-each>
        </div>
      </div>
      <xsl:for-each select="$tabs">
        <xsl:choose>
          <xsl:when test="exists(?chart)">
            <xsl:variable name="diagram" select="?chart/../../s:diagram[@chart = current()?chart/@file]"/>
            <div role="tabpanel" id="{?id}-panel" aria-labelledby="{?id}-tab" class="chart__diagram" data-src="{f:url(?chart/@src)}" hidden="hidden">
              <xsl:if test="$diagram/s:event">
                <xsl:attribute name="data-event-data" select="serialize(map:merge($diagram/s:event ! map { string(@name): parse-json(@data) }, map { 'duplicates': 'use-first' }), map { 'method': 'json' })"/>
              </xsl:if>
              <xsl:if test="$diagram/s:event[normalize-space()]">
                <xsl:attribute name="data-events" select="serialize(array { $diagram/s:event[normalize-space()] ! map { 'name': string(@name), 'data': parse-json(@data), 'label': normalize-space() } }, map { 'method': 'json' })"/>
              </xsl:if>
            </div>
          </xsl:when>
          <xsl:otherwise>
            <div role="tabpanel" id="{?id}-panel" aria-labelledby="{?id}-tab">
              <xsl:if test="position() gt 1"><xsl:attribute name="hidden" select="'hidden'"/></xsl:if>
              <xsl:sequence select="f:copyable(f:pre(?code))"/>
            </div>
          </xsl:otherwise>
        </xsl:choose>
      </xsl:for-each>
    </ta-tabs>
  </xsl:template>

  <!-- The command's commands, from its help: its usage, then a list of
       its commands, each linked to its page with its summary. -->
  <xsl:template match="s:commands">
    <xsl:apply-templates select="f:commands-blocks()"/>
  </xsl:template>

  <!-- A picture a command drew: a help example's SVG, from the capture (href),
       or one under examples/ (src), such as the tutorial's graph;
       alt says what it shows. -->
  <xsl:template match="s:drawing">
    <figure class="figure drawing">
      <div class="figure__scroll" tabindex="0">
        <img src="{(@href, @src ! f:url(.))[1]}" alt="{@alt}"/>
      </div>
    </figure>
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
                    aria-selected="{position() = 1}" tabindex="{if (position() = 1) then 0 else -1}"><xsl:sequence select="f:platform(f:package(.))"/></button>
          </xsl:for-each>
        </div>
      </xsl:if>
      <xsl:for-each select="s:command">
        <div class="install__panel" role="{if (count(../s:command) gt 1) then 'tabpanel' else 'group'}" id="{$id}-panel-{position()}"
             aria-labelledby="{$id}-tab-{position()}">
          <xsl:if test="count(../s:command) = 1"><xsl:attribute name="aria-label" select="'Install for ' || f:package(.)/@for"/></xsl:if>
          <xsl:if test="position() gt 1"><xsl:attribute name="hidden" select="'hidden'"/></xsl:if>
          <xsl:variable name="package" select="f:package(.)"/>
          <xsl:variable name="release" select="f:releases($package)[1]"/>
          <xsl:variable name="command" as="element()">
            <code class="install__command"><span class="install__prompt" aria-hidden="true">$ </span>{replace(normalize-space(.), '\{version\}', $release?version)}</code>
          </xsl:variable>
          <xsl:sequence select="f:copyable($command)"/>
          <p class="install__version">
            <span>{$package} {$release?version}, {f:date($release?date)}</span>
            <a href="{f:changelog-href($package)}">{$site/s:packages/s:changes}</a>
            <xsl:if test="@guide">
              <a class="install__guide" href="{@guide}">{$package/@for} guide</a>
            </xsl:if>
          </p>
        </div>
      </xsl:for-each>
    </ta-tabs>
  </xsl:template>

  <!-- Every package's releases, behind the install commands' tabs (the
       same tablist name, so the reader's platform holds): the newest
       release open, the older ones folded, each the hand-written part of
       its changelog section, quoted. -->
  <xsl:template match="s:changelog">
    <xsl:variable name="changelog" select="."/>
    <xsl:variable name="id" select="generate-id()"/>
    <xsl:variable name="packages" select="$site/s:packages/s:package"/>
    <ta-tabs class="changelog">
      <div role="tablist" class="ta-tabs ta-tabs--segmented" aria-label="Install for">
        <xsl:for-each select="$packages">
          <button type="button" role="tab" class="ta-tab" id="{$id}-tab-{position()}" aria-controls="{$id}-panel-{position()}"
                  aria-selected="{position() = 1}" tabindex="{if (position() = 1) then 0 else -1}"><xsl:sequence select="f:platform(.)"/></button>
        </xsl:for-each>
      </div>
      <xsl:for-each select="$packages">
        <xsl:variable name="package" select="."/>
        <div class="changelog__panel" role="tabpanel" id="{$id}-panel-{position()}" aria-labelledby="{$id}-tab-{position()}">
          <xsl:if test="position() gt 1"><xsl:attribute name="hidden" select="'hidden'"/></xsl:if>
          <xsl:for-each select="f:releases($package)">
            <xsl:variable name="release" select="."/>
            <xsl:variable name="title" select="$package || ' ' || ?version || ', ' || f:date(?date)"/>
            <xsl:variable name="body">
              <blockquote class="changelog__quote">
                <xsl:sequence select="f:release-body(?lines)"/>
              </blockquote>
              <p class="changelog__more"><a href="{f:release-href($package, $release)}">{$changelog/s:more}</a></p>
            </xsl:variable>
            <xsl:choose>
              <xsl:when test="position() = 1">
                <article class="changelog__release" id="{$package/@id}-{translate(?version, '.', '-')}">
                  <h3 class="changelog__title">{$title}</h3>
                  <xsl:sequence select="$body"/>
                </article>
              </xsl:when>
              <xsl:otherwise>
                <details class="changelog__release" id="{$package/@id}-{translate(?version, '.', '-')}">
                  <summary class="changelog__title">{$title}</summary>
                  <xsl:sequence select="$body"/>
                </details>
              </xsl:otherwise>
            </xsl:choose>
          </xsl:for-each>
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

  <!-- ===================================================== changelogs
       A package's CHANGELOG.md (docs/adr/0002-changelogs.md): a release is
       a "## [version] - date" section, newest first; its hand-written part
       ("### Summary", "### Upgrading": paragraphs and "- " items, a line
       each) runs to the "<!-\- Generated" line, the generated lists after
       it. -->

  <xsl:function name="f:package" as="element(s:package)">
    <xsl:param name="command" as="element()"/>
    <xsl:variable name="package" select="$site/s:packages/s:package[@id = $command/@package]"/>
    <xsl:if test="empty($package)">
      <xsl:message terminate="yes">site: {f:where($command)}: no package "{$command/@package}" in site.xml's packages</xsl:message>
    </xsl:if>
    <xsl:sequence select="$package"/>
  </xsl:function>

  <!-- Each release of a package, newest first: {version, date, lines}. -->
  <xsl:function name="f:releases" as="map(*)*" cache="yes">
    <xsl:param name="package" as="element(s:package)"/>
    <xsl:variable name="lines" select="tokenize(unparsed-text(resolve-uri($package/@changelog, base-uri($package))), '\r?\n')"/>
    <xsl:variable name="starts" select="index-of($lines ! matches(., '^## \[[^\]]+\] - \d{4}-\d{2}-\d{2}$'), true())"/>
    <xsl:if test="empty($starts)">
      <xsl:message terminate="yes">site: {$package/@changelog} has no "## [version] - date" section</xsl:message>
    </xsl:if>
    <xsl:for-each select="$starts">
      <xsl:variable name="i" select="position()"/>
      <xsl:variable name="section" select="subsequence($lines, . + 1, ($starts[$i + 1], count($lines) + 1)[1] - . - 1)"/>
      <xsl:variable name="end" select="(index-of($section ! starts-with(., '&lt;!-- Generated'), true())[1], count($section) + 1)[1]"/>
      <xsl:sequence select="map {
        'heading': substring($lines[current()], 4),
        'version': replace($lines[current()], '^## \[([^\]]+)\].*$', '$1'),
        'date': replace($lines[current()], '^.* - ', ''),
        'lines': subsequence($section, 1, $end - 1)
      }"/>
    </xsl:for-each>
  </xsl:function>

  <xsl:function name="f:date" as="xs:string">
    <xsl:param name="date" as="xs:string"/>
    <xsl:sequence select="format-date(xs:date($date), '[D] [MNn] [Y]', 'en', (), ())"/>
  </xsl:function>

  <!-- A release's hand-written part: "### " headings, paragraphs, lists. -->
  <xsl:function name="f:release-body" as="node()*">
    <xsl:param name="lines" as="xs:string*"/>
    <xsl:for-each-group select="$lines[normalize-space()]" group-adjacent="if (starts-with(., '- ')) then 'item' else if (starts-with(., '### ')) then 'heading' else 'p' || position()">
      <xsl:choose>
        <xsl:when test="current-grouping-key() = 'heading'"><h4>{substring(., 5)}</h4></xsl:when>
        <xsl:when test="current-grouping-key() = 'item'">
          <ul><xsl:for-each select="current-group()"><li><xsl:sequence select="f:inline(substring(., 3))"/></li></xsl:for-each></ul>
        </xsl:when>
        <xsl:otherwise><p><xsl:sequence select="f:inline(.)"/></p></xsl:otherwise>
      </xsl:choose>
    </xsl:for-each-group>
  </xsl:function>

  <!-- A release's whole section in the changelog on main, which holds
       every release (an early tag can predate the file): GitHub's anchor
       of the heading. -->
  <xsl:function name="f:release-href" as="xs:string">
    <xsl:param name="package" as="element(s:package)"/>
    <xsl:param name="release" as="map(*)"/>
    <xsl:variable name="directory" select="tokenize($package/@changelog, '/')[last() - 1]"/>
    <xsl:variable name="anchor" select="replace(replace(lower-case($release?heading), '[^a-z0-9 -]', ''), ' ', '-')"/>
    <xsl:sequence select="$site/s:packages/@source || '/blob/main/' || $directory || '/CHANGELOG.md#' || $anchor"/>
  </xsl:function>

  <!-- The page that holds the changelogs, at the newest release of `package`. -->
  <xsl:function name="f:changelog-href" as="xs:string">
    <xsl:param name="package" as="element(s:package)"/>
    <xsl:variable name="page" select="$pages[.//s:changelog][1]"/>
    <xsl:if test="empty($page)">
      <xsl:message terminate="yes">site: install commands link to the changelogs, and no page holds a changelog</xsl:message>
    </xsl:if>
    <xsl:sequence select="$page/@path || '#' || $package/@id || '-' || translate(f:releases($package)[1]?version, '.', '-')"/>
  </xsl:function>

  <!-- A code block's text: highlighted later by components/highlight.mjs
       (its language), the live example's output written by its run. -->
  <xsl:function name="f:pre" as="element()">
    <xsl:param name="code" as="element(s:code)?"/>
    <pre tabindex="0"><code><xsl:if test="$code"><xsl:attribute name="data-language" select="$code/@language"/>{f:code($code)}</xsl:if></code></pre>
  </xsl:function>

  <!-- A block of code a reader can copy (components/copy.mjs): a button
       over its top right corner, shown on hover, focus or a tap, that copies
       its text but what is aria-hidden (a command's prompt). Hidden until
       the component makes it work. The icons are Lucide's (lucide-static,
       ISC), inlined so they take the text's colour. -->
  <xsl:function name="f:copyable" as="element()">
    <xsl:param name="block" as="element()"/>
    <ta-copy class="copy">
      <xsl:sequence select="$block"/>
      <button type="button" class="copy__button" aria-label="Copy code" title="Copy" data-done="Copied" hidden="hidden">
        <xsl:sequence select="f:lucide('copy', 'copy__icon copy__icon--copy')"/>
        <xsl:sequence select="f:lucide('check', 'copy__icon copy__icon--done')"/>
      </button>
      <span class="copy__status visually-hidden" role="status"></span>
    </ta-copy>
  </xsl:function>

  <!-- A Lucide icon, from website/'s lucide-static (package.json). -->
  <xsl:function name="f:lucide" as="element()" cache="yes">
    <xsl:param name="name" as="xs:string"/>
    <xsl:param name="class" as="xs:string"/>
    <xsl:variable name="svg" select="doc(resolve-uri('../node_modules/lucide-static/icons/' || $name || '.svg', static-base-uri()))/*"/>
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="{$svg/@viewBox}" class="{$class}" aria-hidden="true" focusable="false">
      <xsl:copy-of select="$svg/(@fill, @stroke, @stroke-width, @stroke-linecap, @stroke-linejoin)"/>
      <xsl:copy-of select="$svg/*"/>
    </svg>
  </xsl:function>

  <!-- ===================================================== platforms
       A package's platforms as their icons (its `runs-on`; icons/platforms/,
       served at /assets/platforms/): a logo as an image of its own (their
       gradients have ids, which an inlined copy would repeat), the globe of
       a browser inlined, so it takes the text's colour. The package's `for`
       is the accessible name, as text a screen reader reads and the tabs
       compare (components/tabs.mjs), and the tooltip. -->

  <xsl:function name="f:platform" as="element()">
    <xsl:param name="package" as="element(s:package)"/>
    <span class="platform" title="{$package/@for}">
      <xsl:for-each select="tokenize($package/@runs-on)"><xsl:sequence select="f:icon(.)"/></xsl:for-each>
      <span class="visually-hidden">{$package/@for}</span>
    </span>
  </xsl:function>

  <xsl:function name="f:icon" as="element()" cache="yes">
    <xsl:param name="platform" as="xs:string"/>
    <xsl:choose>
      <xsl:when test="$platform = 'browser'">
        <xsl:sequence select="f:inline-icon($platform)"/>
      </xsl:when>
      <xsl:otherwise>
        <img class="platform__icon platform__icon--{$platform}" src="/assets/platforms/{$platform}.svg" alt="" width="18" height="18"/>
      </xsl:otherwise>
    </xsl:choose>
  </xsl:function>

  <xsl:function name="f:inline-icon" as="element()">
    <xsl:param name="platform" as="xs:string"/>
    <xsl:variable name="svg" select="doc(resolve-uri('../icons/platforms/' || $platform || '.svg', static-base-uri()))/*"/>
    <svg xmlns="http://www.w3.org/2000/svg" viewBox="{$svg/@viewBox}" class="platform__icon platform__icon--{$platform}" aria-hidden="true" focusable="false">
      <xsl:copy-of select="$svg/(@fill, @stroke, @stroke-width, @stroke-linecap, @stroke-linejoin)"/>
      <xsl:if test="empty($svg/@fill)"><xsl:attribute name="fill" select="'currentColor'"/></xsl:if>
      <xsl:copy-of select="$svg/* except $svg/*:title"/>
    </svg>
  </xsl:function>

  <!-- Every need of these audiences, as "audience.need". -->
  <xsl:function name="f:needs" as="xs:string*">
    <xsl:param name="audiences" as="element(s:audience)*"/>
    <xsl:sequence select="$audiences ! (let $a := @id return (s:want, s:task, s:objection) ! ($a || '.' || @id))"/>
  </xsl:function>

  <xsl:function name="f:where" as="xs:string">
    <xsl:param name="node" as="element()"/>
    <xsl:sequence select="replace(base-uri($node), '^.*/content/', 'content/')"/>
  </xsl:function>
  <!-- ===================================================== the CLI
       The pages of the command's commands (mise/cli.rb, build/cli/cli.xml),
       made of the content's own blocks so the HTML, the Markdown and the
       checks treat them as any page: no word of them is written here. The
       help's text is the command's, the headings and labels the content's
       (<commands>), each command's page at the commands page's path
       followed by its name, its id the page's followed by "-" and its
       name. -->

  <xsl:function name="f:command-pages" as="element(s:page)*">
    <xsl:variable name="page" select="$commands/ancestor::s:page"/>
    <xsl:for-each select="$cli-doc/command">
      <xsl:variable name="name" select="string(@name)"/>
      <xsl:variable name="help" select="help"/>
      <s:page id="{$page/@id}-{$name}" path="{$page/@path}{$name}/">
        <s:title>{f:fill($commands/s:title, 'command', $name)}</s:title>
        <s:description>{f:sentence($help/@summary)}</s:description>
        <s:purpose kind="reference" for="{$commands/@for}"/>
        <s:hero>
          <s:headline><s:code>{$cli-doc/help/@name} {$name}</s:code></s:headline>
          <s:lede><xsl:sequence select="f:help-text(f:sentence($help/@summary))"/></s:lede>
        </s:hero>
        <s:section id="usage">
          <s:heading>{normalize-space($commands/s:usage)}</s:heading>
          <s:code language="shell">{$help/@usage}</s:code>
          <xsl:if test="normalize-space($help/@description)"><s:p><xsl:sequence select="f:help-text($help/@description)"/></s:p></xsl:if>
        </s:section>
        <s:section id="options">
          <s:heading>{normalize-space($commands/s:options)}</s:heading>
          <xsl:if test="$help/options/option"><xsl:sequence select="f:options-list($help/options/option)"/></xsl:if>
          <xsl:choose>
            <xsl:when test="$commands/s:shared"><s:p><xsl:sequence select="$commands/s:shared/node()"/></s:p></xsl:when>
            <xsl:otherwise><xsl:sequence select="f:options-list($help/programOptions/option)"/></xsl:otherwise>
          </xsl:choose>
        </s:section>
        <xsl:if test="runs/example">
          <s:section id="examples">
            <s:heading>{normalize-space($commands/s:examples)}</s:heading>
            <xsl:for-each select="runs/example">
              <xsl:variable name="caption" select="string-join((
                  if (normalize-space(@caption)) then f:sentence(replace(normalize-space(@caption), ':$', '')) else (),
                  if (@status != '0') then f:fill($commands/s:status, 'status', @status) else ()), ' ')"/>
              <s:example>
                <s:code language="shell">{@line}</s:code>
                <xsl:if test="normalize-space(output)"><s:code language="{f:output-language(@line, $name)}" role="output">{output}</s:code></xsl:if>
                <xsl:if test="$caption"><s:caption><xsl:sequence select="f:help-text($caption)"/></s:caption></xsl:if>
              </s:example>
              <xsl:for-each select="svg">
                <s:drawing href="{$page/@path}{@href}" alt="{../@line}" file="{@file}"/>
              </xsl:for-each>
            </xsl:for-each>
          </s:section>
        </xsl:if>
        <!-- the help's exit statuses, when it gives them: each status and
             when the command exits with it -->
        <xsl:variable name="statuses" select="$help/*[matches(local-name(), '^exit', 'i')]/*[(@status, @code)]"/>
        <xsl:if test="$statuses">
          <s:section id="exit-status">
            <s:heading>{normalize-space($commands/s:exit-status)}</s:heading>
            <s:list>
              <xsl:for-each select="$statuses">
                <s:item>
                  <s:term><s:code>{(@status, @code)[1]}</s:code></s:term>
                  <s:text><xsl:sequence select="f:help-text(f:sentence((@description, @meaning, @when, string(.))[normalize-space()][1]))"/></s:text>
                </s:item>
              </xsl:for-each>
            </s:list>
          </s:section>
        </xsl:if>
        <xsl:variable name="see" select="$commands/(s:related, s:artifact)[@command = $name]"/>
        <!-- the commands that do a job with this one, in the jobs' order -->
        <xsl:variable name="siblings" select="distinct-values($commands/s:job[tokenize(normalize-space(@commands), ' ') = $name]/tokenize(normalize-space(@commands), ' '))[. != $name]"/>
        <xsl:if test="$see or exists($siblings)">
          <s:section id="see">
            <s:heading>{normalize-space($commands/s:see)}</s:heading>
            <s:list>
              <xsl:for-each select="$see">
                <s:item><s:text>
                  <xsl:choose>
                    <xsl:when test="self::s:related"><xsl:sequence select="s:link"/></xsl:when>
                    <xsl:otherwise><s:link href="{$page/@path}{@file}">{normalize-space(.)}</s:link></xsl:otherwise>
                  </xsl:choose>
                </s:text></s:item>
              </xsl:for-each>
              <xsl:for-each select="$siblings">
                <xsl:variable name="command" select="$cli-doc/help/commands/command[@command = current()]"/>
                <s:item>
                  <s:term><s:link to="{$page/@id}-{.}"><s:code>{$cli-doc/help/@name} {.}</s:code></s:link></s:term>
                  <s:text><xsl:sequence select="f:help-text(f:sentence($command/@summary))"/></s:text>
                </s:item>
              </xsl:for-each>
            </s:list>
          </s:section>
        </xsl:if>
        <!-- every command beside the page (render: the command layout) -->
        <s:command-nav current="{$name}"/>
      </s:page>
    </xsl:for-each>
  </xsl:function>

  <!-- A command's page ends with the way to the others: the one before and
       the one after it in the help's order, then every command, this one
       marked. -->
  <!-- Every command, beside a command's page: under each job's name its
       commands, this one marked. On a narrow screen it follows the page. -->
  <xsl:template match="s:command-nav" mode="sidebar">
    <xsl:variable name="page" select="$commands/ancestor::s:page"/>
    <xsl:variable name="names" select="$cli-doc/help/commands/command/@command ! string()"/>
    <xsl:variable name="current" select="string(@current)"/>
    <xsl:variable name="groups" as="map(*)*" select="if ($commands/s:job)
        then $commands/s:job ! map { 'name': s:name, 'names': tokenize(normalize-space(@commands), ' ') }
        else map { 'name': (), 'names': $names }"/>
    <nav class="command-sidebar" aria-labelledby="command-sidebar-title">
      <p class="command-sidebar__title" id="command-sidebar-title"><a href="{$page/@path}">{normalize-space($commands/s:all)}</a></p>
      <xsl:for-each select="$groups">
        <p class="command-sidebar__label"><xsl:apply-templates select="?name/node()"/></p>
        <ul class="command-sidebar__list">
          <xsl:for-each select="?names">
            <li>
              <xsl:choose>
                <xsl:when test=". = $current"><a href="{$page/@path}{.}/" aria-current="page"><code>{.}</code></a></xsl:when>
                <xsl:otherwise><a href="{$page/@path}{.}/"><code>{.}</code></a></xsl:otherwise>
              </xsl:choose>
            </li>
          </xsl:for-each>
        </ul>
      </xsl:for-each>
    </nav>
  </xsl:template>

  <!-- The command before and the one after, in the help's order, at the
       page's end. -->
  <xsl:template match="s:command-nav" mode="steps">
    <xsl:variable name="page" select="$commands/ancestor::s:page"/>
    <xsl:variable name="names" select="$cli-doc/help/commands/command/@command ! string()"/>
    <xsl:variable name="at" select="index-of($names, string(@current))[1]"/>
    <xsl:variable name="program" select="string($cli-doc/help/@name)"/>
    <nav class="command-steps wrap" aria-label="{normalize-space($commands/s:all)}">
      <xsl:if test="$at gt 1">
        <a class="command-steps__step" rel="prev" href="{$page/@path}{$names[$at - 1]}/"><span aria-hidden="true">← </span><code>{$program} {$names[$at - 1]}</code></a>
      </xsl:if>
      <xsl:if test="$at lt count($names)">
        <a class="command-steps__step command-steps__step--next" rel="next" href="{$page/@path}{$names[$at + 1]}/"><code>{$program} {$names[$at + 1]}</code><span aria-hidden="true"> →</span></a>
      </xsl:if>
    </nav>
  </xsl:template>

  <!-- Options as a list of terms: each one's names, then what it does,
       then the values it takes (the help's own words; the content's word
       for "values"). A list, not a table: a description wraps, and on a
       phone each option stacks. -->
  <xsl:function name="f:options-list" as="element(s:list)">
    <xsl:param name="options" as="element(option)*"/>
    <s:list>
      <xsl:for-each select="$options">
        <xsl:variable name="values" select="tokenize(@values, ',\s*')[. != '']"/>
        <s:item>
          <s:term><s:code>{@option}</s:code></s:term>
          <s:text><xsl:sequence select="f:help-text(f:sentence(@description))"/><xsl:if test="exists($values) and not(contains(@description, normalize-space($commands/s:values) || ':'))">
              <xsl:text> </xsl:text>{normalize-space($commands/s:values)}: <xsl:for-each select="$values"><xsl:if test="position() gt 1">, </xsl:if><s:code>{.}</s:code></xsl:for-each>.</xsl:if></s:text>
        </s:item>
      </xsl:for-each>
    </s:list>
  </xsl:function>

  <!-- The commands page's blocks: the program's usage, then each command
       (in a section of their own, as the templates of a section's blocks match them). -->
  <xsl:function name="f:commands-blocks" as="element()*">
    <xsl:variable name="page" select="$commands/ancestor::s:page"/>
    <xsl:variable name="blocks" as="element(s:section)">
      <s:section id="commands">
        <s:p><s:code>{$cli-doc/help/@usage}</s:code></s:p>
        <xsl:choose>
          <!-- by job, as the content groups them -->
          <xsl:when test="$commands/s:job">
            <xsl:for-each select="$commands/s:job">
              <s:command-group id="{@id}">
                <s:name><xsl:sequence select="s:name/node()"/></s:name>
                <s:summary><xsl:sequence select="s:summary/node()"/></s:summary>
                <xsl:sequence select="f:command-items(tokenize(normalize-space(@commands), ' '))"/>
              </s:command-group>
            </xsl:for-each>
          </xsl:when>
          <xsl:otherwise><xsl:sequence select="f:command-items($cli-doc/help/commands/command/@command ! string())"/></xsl:otherwise>
        </xsl:choose>
        <!-- the options every command takes, once, where each command's page links -->
        <s:command-group id="program-options">
          <s:name>{normalize-space($commands/s:program-options)}</s:name>
          <xsl:sequence select="f:options-list(($cli-doc/command/help/programOptions)[1]/option)"/>
        </s:command-group>
      </s:section>
    </xsl:variable>
    <xsl:sequence select="$blocks/*"/>
  </xsl:function>

  <!-- The commands named, each linked to its page with its summary. -->
  <xsl:function name="f:command-items" as="element(s:list)">
    <xsl:param name="names" as="xs:string*"/>
    <xsl:variable name="page" select="$commands/ancestor::s:page"/>
    <s:list>
      <xsl:for-each select="$names">
        <xsl:variable name="command" select="$cli-doc/help/commands/command[@command = current()]"/>
        <s:item>
          <s:term><s:link to="{$page/@id}-{.}"><s:code>{$cli-doc/help/@name} {.}</s:code></s:link></s:term>
          <s:text><xsl:sequence select="f:help-text(f:sentence($command/@summary))"/></s:text>
        </s:item>
      </xsl:for-each>
    </s:list>
  </xsl:function>

  <!-- A group of the commands page: its name as a heading that links to
       itself, what the group is for, and its commands or options. -->
  <xsl:template match="s:command-group">
    <div class="command-group" id="{@id}">
      <h3 class="command-group__name"><a class="section__anchor" href="#{@id}"><xsl:apply-templates select="s:name/node()"/></a></h3>
      <xsl:if test="s:summary"><p class="prose command-group__summary"><xsl:apply-templates select="s:summary/node()"/></p></xsl:if>
      <xsl:apply-templates select="s:list"/>
    </div>
  </xsl:template>

  <!-- The placeholders the command's usage lines and options name (INPUT,
       DIR, FORMAT, ...), which its prose refers to; "ID" is a word too. -->
  <xsl:variable name="metavariables" as="xs:string*" select="distinct-values(
      ($cli-doc//@usage, $cli-doc//option/@option) ! tokenize(., '[^A-Z0-9_]+')[matches(., '^[A-Z][A-Z0-9_]+$')][. != 'ID'])"/>

  <!-- The help's prose with what a reader types set as code: an option
       (a dash or two and a name, with its =VALUE), a placeholder of the usage (INPUT,
       NAME=VALUE), a file or an extension (order.scxml, .tai) and an
       element (<system>). -->
  <xsl:function name="f:help-text" as="node()*">
    <xsl:param name="text" as="xs:string"/>
    <xsl:variable name="meta" select="if (exists($metavariables)) then string-join($metavariables, '|') else 'NO_PLACEHOLDER'"/>
    <xsl:variable name="re" select="'(^|[\s(\[,;&quot;''])(--?[A-Za-z][A-Za-z0-9-]*(=[^\s,;)]+)?|(' || $meta || ')(=[^\s,;)]+)?(\.\.\.)?|[A-Za-z0-9_*/-]*\.(scxml|tai|jsonl|json|xml|svg|snapshot|md)|&lt;[a-z][a-z-]*&gt;)'"/>
    <xsl:analyze-string select="$text" regex="{$re}">
      <xsl:matching-substring>
        <xsl:value-of select="regex-group(1)"/>
        <s:code>{regex-group(2)}</s:code>
      </xsl:matching-substring>
      <xsl:non-matching-substring><xsl:value-of select="."/></xsl:non-matching-substring>
    </xsl:analyze-string>
  </xsl:function>

  <!-- The content's words with "{name}" filled in. -->
  <xsl:function name="f:fill" as="xs:string">
    <xsl:param name="words" as="element()"/>
    <xsl:param name="name" as="xs:string"/>
    <xsl:param name="value" as="xs:string"/>
    <xsl:sequence select="replace(normalize-space($words), '{' || $name || '}', $value, 'q')"/>
  </xsl:function>

  <!-- The help's lowercase summary as a sentence. -->
  <xsl:function name="f:sentence" as="xs:string">
    <xsl:param name="text" as="xs:string"/>
    <xsl:variable name="text" select="normalize-space($text)"/>
    <xsl:sequence select="upper-case(substring($text, 1, 1)) || substring($text, 2) || (if (matches($text, '[.!?]$')) then '' else '.')"/>
  </xsl:function>
</xsl:stylesheet>
