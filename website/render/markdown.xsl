<?xml version="1.0" encoding="UTF-8"?>
<!--
  The site for agents (llmstxt.org): every page as Markdown beside its
  HTML ("/x/" as x/index.md), llms.txt, which lists them in the order and
  groups of site.xml's navigation, and llms-full.txt, every page but the
  optional ones in one file. site.xsl includes it and writes the files.

  It says what the HTML says, from the same content: no words of its own
  beyond what the HTML has ("Source", "output"). What only a browser can
  do (tabs, a chart's diagram, a live example, a playground) becomes what
  it shows: every alternative, the chart's document, a link to the page.
  A block it cannot write stops the build, so a new kind of block never
  goes missing here: a new element of the schema gets its template in
  mode "md" too.

  A page whose content is a reference (the changelog) is optional: llms.txt
  lists it under "Optional", and llms-full.txt leaves it out.
-->
<xsl:stylesheet version="4.0"
                xmlns:xsl="http://www.w3.org/1999/XSL/Transform"
                xmlns:xs="http://www.w3.org/2001/XMLSchema"
                xmlns:s="https://www.tinyactors.dev/ns/site"
                xmlns:f="https://www.tinyactors.dev/ns/site/functions"
                exclude-result-prefixes="xs s f"
                expand-text="yes">

  <xsl:variable name="nl" as="xs:string" select="'&#10;'"/>
  <xsl:variable name="blank" as="xs:string" select="'&#10;&#10;'"/>

  <!-- The pages an agent reads: every page but the 404. -->
  <xsl:variable name="md-pages" as="element(s:page)*" select="$pages[not(@path = '/404')]"/>

  <xsl:function name="f:optional" as="xs:boolean">
    <xsl:param name="page" as="element(s:page)"/>
    <xsl:sequence select="exists($page/s:reference)"/>
  </xsl:function>

  <!-- ===================================================== the files -->

  <xsl:template name="markdown">
    <xsl:for-each select="$md-pages">
      <xsl:result-document href="{f:md-file(@path)}" method="text">
        <xsl:sequence select="f:page-md(.)"/>
      </xsl:result-document>
    </xsl:for-each>
    <xsl:result-document href="llms.txt" method="text">
      <xsl:sequence select="f:llms()"/>
    </xsl:result-document>
    <xsl:result-document href="llms-full.txt" method="text">
      <xsl:sequence select="string-join(f:llms-order()[not(f:optional(.))] ! f:page-md(.), $nl || '---' || $blank)"/>
    </xsl:result-document>
  </xsl:template>

  <!-- The pages in llms.txt's order: the home page, then the navigation's
       pages in its order, then any page the navigation does not name. -->
  <xsl:function name="f:llms-order" as="element(s:page)*">
    <xsl:variable name="named" select="$site/s:nav//s:link/@to"/>
    <xsl:sequence select="$md-pages[@path = '/'],
                          distinct-values($named) ! (let $id := . return $md-pages[@id = $id][not(@path = '/')]),
                          $md-pages[not(@path = '/') and not(@id = $named)]"/>
  </xsl:function>

  <!-- llms.txt: the name, the tagline, the home page's lede, the packages,
       the audiences each page names in its first line, then the pages: those outside a navigation group under "Pages", each
       group under its label, the optional ones under "Optional", and the
       links to other sites. Each entry has its page's description. -->
  <xsl:function name="f:llms" as="xs:string">
    <xsl:variable name="home" select="$md-pages[@path = '/']"/>
    <xsl:variable name="order" select="f:llms-order()"/>
    <xsl:variable name="grouped" select="$site/s:nav/s:group/s:link/@to"/>
    <xsl:variable name="parts" as="xs:string*">
      <xsl:sequence select="'# ' || normalize-space($site/s:name)"/>
      <xsl:sequence select="'> ' || normalize-space($site/s:tagline)"/>
      <xsl:sequence select="$home/s:hero/s:lede ! f:md-inline(.)"/>
      <xsl:sequence select="'## ' || 'Packages' || $blank || string-join(
                              $site/s:packages/s:package ! f:llms-package(.), $nl)"/>
      <xsl:sequence select="'## ' || 'Audiences' || $blank || string-join(
                              $site/s:audiences/s:audience ! ('- ' || f:audience-link(@id) || ': ' || normalize-space(s:description)), $nl)"/>
      <xsl:sequence select="f:llms-section('Pages', $order[not(f:optional(.)) and not(@id = $grouped)], false())"/>
      <xsl:for-each select="$site/s:nav/s:group">
        <xsl:variable name="ids" select="s:link/@to"/>
        <xsl:sequence select="f:llms-section(normalize-space(@label), $order[not(f:optional(.)) and @id = $ids], true())"/>
      </xsl:for-each>
      <xsl:sequence select="f:llms-section('Optional', $order[f:optional(.)], false())"/>
      <xsl:variable name="elsewhere" select="($site/s:nav//s:link, $site/s:footer/s:links/s:link)[@href]"/>
      <xsl:sequence select="'## ' || 'Elsewhere' || $blank || string-join(
                              distinct-values($elsewhere/@href) ! (let $href := . return
                                '- [' || normalize-space($elsewhere[@href = $href][1]) || '](' || $href || ')'), $nl)"/>
    </xsl:variable>
    <xsl:sequence select="string-join($parts[. != ''], $blank) || $nl"/>
  </xsl:function>

  <xsl:function name="f:llms-section" as="xs:string">
    <xsl:param name="heading" as="xs:string"/>
    <xsl:param name="pages" as="element(s:page)*"/>
    <xsl:param name="by-nav" as="xs:boolean"/>
    <xsl:sequence select="if (empty($pages)) then '' else
                          '## ' || $heading || $blank || string-join($pages ! f:llms-entry(., $by-nav), $nl)"/>
  </xsl:function>

  <!-- An entry: a page of a group by the name its group gives it (the
       group's label says what the titles' suffixes would), any other by
       its title, which says more than the header's one word. -->
  <xsl:function name="f:llms-entry" as="xs:string">
    <xsl:param name="page" as="element(s:page)"/>
    <xsl:param name="by-nav" as="xs:boolean"/>
    <xsl:variable name="link" select="$site/s:nav/s:group/s:link[@to = $page/@id][normalize-space(.) != ''][1]"/>
    <xsl:variable name="name" select="if ($by-nav and $link) then f:md-inline($link) else normalize-space($page/s:title)"/>
    <xsl:sequence select="'- [' || $name || '](' || $origin || f:md-path($page/@path) || '): ' || normalize-space($page/s:description)"/>
  </xsl:function>

  <!-- A package: its newest version, what it is for, and the first
       command a page gives to install it. -->
  <xsl:function name="f:llms-package" as="xs:string">
    <xsl:param name="package" as="element(s:package)"/>
    <xsl:variable name="release" select="f:releases($package)[1]"/>
    <xsl:variable name="command" select="($md-pages//s:install/s:command[@package = $package/@id])[1]"/>
    <xsl:sequence select="'- ' || normalize-space($package) || ' ' || $release?version || ' (' || $package/@for || ')'
                          || (if ($command) then ': `' || f:command($command) || '`' else '')"/>
  </xsl:function>

  <!-- ===================================================== a page -->

  <!-- Where a page's Markdown is written and served: "/" as index.md,
       "/x/" as x/index.md. -->
  <xsl:function name="f:md-file" as="xs:string">
    <xsl:param name="path" as="xs:string"/>
    <xsl:sequence select="substring(f:md-path($path), 2)"/>
  </xsl:function>

  <xsl:function name="f:md-path" as="xs:string">
    <xsl:param name="path" as="xs:string"/>
    <xsl:sequence select="if (ends-with($path, '/')) then $path || 'index.md' else $path || '.md'"/>
  </xsl:function>

  <!-- A page: a comment with its HTML's address, its title, its
       description, then its hero or reference and its sections. -->
  <xsl:function name="f:page-md" as="xs:string" cache="yes">
    <xsl:param name="page" as="element(s:page)"/>
    <xsl:variable name="blocks" as="xs:string*">
      <xsl:sequence select="'&lt;!-- ' || $origin || $page/@path || ' --&gt;' || $nl || '# ' || normalize-space($page/s:title)"/>
      <xsl:sequence select="$page/s:purpose ! f:purpose-md(.)"/>
      <xsl:sequence select="'> ' || normalize-space($page/s:description)"/>
      <xsl:apply-templates select="$page/(s:hero, s:reference, s:section)" mode="md"/>
    </xsl:variable>
    <xsl:sequence select="string-join($blocks[. != ''], $blank) || $nl"/>
  </xsl:function>

  <!-- A page's purpose, in one line: its kind, and its audiences in
       order, each linked to its file (/audiences/<id>.xml), as the HTML's
       head names them. -->
  <xsl:function name="f:purpose-md" as="xs:string">
    <xsl:param name="purpose" as="element(s:purpose)"/>
    <xsl:sequence select="'Kind: ' || $purpose/@kind || '. For: ' || string-join(tokenize($purpose/@for) ! f:audience-link(.), ', ') || '.'"/>
  </xsl:function>

  <xsl:function name="f:audience-link" as="xs:string">
    <xsl:param name="id" as="xs:string"/>
    <xsl:sequence select="'[' || normalize-space($site/s:audiences/s:audience[@id = $id]/s:name) || '](' || $origin || '/audiences/' || $id || '.xml)'"/>
  </xsl:function>

  <!-- The hero's headline is the HTML's first heading; here the page's
       title is, and the lede says what the headline says. -->
  <xsl:template match="s:hero" mode="md" as="xs:string*">
    <xsl:apply-templates select="* except s:headline" mode="md"/>
  </xsl:template>

  <xsl:template match="s:reference" mode="md" as="xs:string*">
    <xsl:apply-templates select="* except s:headline" mode="md"/>
  </xsl:template>

  <xsl:template match="s:lede | s:section/s:p | s:reference/s:p" mode="md" as="xs:string">
    <xsl:sequence select="f:md-inline(.)"/>
  </xsl:template>

  <xsl:template match="s:author" mode="md" as="xs:string">
    <xsl:sequence select="'[' || normalize-space(.) || '](' || @href || ')'"/>
  </xsl:template>

  <!-- A section: an anchor with its id, so links into it hold, then its
       heading and blocks. -->
  <xsl:template match="s:section" mode="md" as="xs:string">
    <xsl:variable name="blocks" as="xs:string*">
      <xsl:sequence select="'&lt;a id=&quot;' || @id || '&quot;&gt;&lt;/a&gt;' || $nl || $nl || '## ' || f:md-inline(s:heading)"/>
      <xsl:apply-templates select="* except s:heading" mode="md"/>
    </xsl:variable>
    <xsl:sequence select="string-join($blocks[. != ''], $blank)"/>
  </xsl:template>

  <xsl:template match="s:list" mode="md" as="xs:string">
    <xsl:variable name="ordered" select="@ordered = 'true'"/>
    <xsl:sequence select="string-join(s:item ! (
                            (if ($ordered) then position() || '. ' else '- ')
                            || (if (s:term) then '**' || f:md-inline(s:term) || '**: ' else '')
                            || f:md-inline(s:text)), $nl)"/>
  </xsl:template>

  <!-- Code: a fence in its language, the file it names as its title; an
       output's title is "output", as the HTML's head says. A chart's
       diagram is drawn from its document, which is here. -->
  <xsl:template match="s:code" mode="md" as="xs:string">
    <xsl:variable name="text" select="f:code(.)"/>
    <xsl:variable name="fence" select="if (contains($text, '```')) then '````' else '```'"/>
    <xsl:variable name="title" select="if (@role = 'output') then 'output' else string(@file)"/>
    <xsl:sequence select="$fence || (if (@role = 'output') then 'text' else @language)
                          || (if ($title != '') then ' title=&quot;' || $title || '&quot;' else '')
                          || $nl || $text || $nl || $fence"/>
  </xsl:template>

  <xsl:template match="s:example" mode="md" as="xs:string">
    <xsl:variable name="blocks" as="xs:string*">
      <xsl:apply-templates select="s:code" mode="md"/>
      <xsl:sequence select="s:caption ! f:md-inline(.)"/>
    </xsl:variable>
    <xsl:sequence select="string-join($blocks, $blank)"/>
  </xsl:template>

  <!-- A live example runs in the page: its button's label links there,
       and the module it runs is named beside it. -->
  <xsl:template match="s:live" mode="md" as="xs:string">
    <xsl:sequence select="'[' || normalize-space(s:label) || '](' || f:html-at(.) || ') · ['
                          || tokenize(@src, '/')[last()] || '](' || $origin || f:url(@src) || ')'"/>
  </xsl:template>

  <!-- A playground: what it shows, and the page where it runs. -->
  <xsl:template match="s:playground" mode="md" as="xs:string">
    <xsl:sequence select="f:md-inline(s:description) || $blank || '&lt;' || f:html-at(.) || '&gt;'"/>
  </xsl:template>

  <xsl:template match="s:table[s:head]" mode="md" as="xs:string" priority="2">
    <xsl:variable name="row" select="function($cells) { '| ' || string-join($cells ! replace(f:md-inline(.), '\|', '\\|'), ' | ') || ' |' }"/>
    <xsl:variable name="lines" select="$row(s:head/s:cell),
                                       '|' || string-join(s:head/s:cell ! '---', '|') || '|',
                                       s:row ! $row(s:cell)"/>
    <xsl:sequence select="string-join((string-join($lines, $nl), s:caption ! f:md-inline(.)), $blank)"/>
  </xsl:template>

  <!-- A table transcluded from a guide's Markdown: its lines, with a link
       into the repository's files kept as its text (f:md-repo-links). -->
  <xsl:template match="s:table" mode="md" as="xs:string">
    <xsl:variable name="lines" select="f:table(.) ! f:md-repo-links(.)"/>
    <xsl:sequence select="string-join((string-join($lines, $nl), s:caption ! f:md-inline(.)), $blank)"/>
  </xsl:template>

  <!-- A line of the repository's Markdown: a link relative to its file
       (another guide's notes, an anchor there) as its text, as the HTML
       keeps it; a link to https as it is. -->
  <xsl:function name="f:md-repo-links" as="xs:string">
    <xsl:param name="line" as="xs:string"/>
    <xsl:variable name="parts" as="xs:string*">
      <xsl:analyze-string select="$line" regex="\[([^\]]+)\]\(([^)]+)\)">
        <xsl:matching-substring>
          <xsl:sequence select="if (starts-with(regex-group(2), 'https://')) then . else regex-group(1)"/>
        </xsl:matching-substring>
        <xsl:non-matching-substring><xsl:sequence select="."/></xsl:non-matching-substring>
      </xsl:analyze-string>
    </xsl:variable>
    <xsl:sequence select="string-join($parts)"/>
  </xsl:function>

  <xsl:template match="s:claims" mode="md" as="xs:string">
    <xsl:sequence select="string-join(s:claim ! (
                            '- **' || f:md-inline(s:statement) || '**: '
                            || string-join(s:p ! f:md-inline(.), ' ')
                            || ' Source: ' || string-join(s:source ! ('[' || f:md-inline(.) || '](' || f:md-href(.) || ')'), ' · ')), $nl)"/>
  </xsl:template>

  <!-- Install commands: every alternative the tabs hold, each with its
       package's newest version and where its changes are. -->
  <xsl:template match="s:install" mode="md" as="xs:string">
    <xsl:sequence select="string-join(s:command ! f:install-md(.), $nl)"/>
  </xsl:template>

  <xsl:function name="f:install-md" as="xs:string">
    <xsl:param name="command" as="element(s:command)"/>
    <xsl:variable name="package" select="f:package($command)"/>
    <xsl:variable name="release" select="f:releases($package)[1]"/>
    <xsl:sequence select="'- ' || $package/@for || ': `' || f:command($command) || '` (' || normalize-space($package) || ' ' || $release?version
                          || ', ' || f:date($release?date) || ' · [' || normalize-space($site/s:packages/s:changes) || '](' || f:md-changelog-href($package) || ')'
                          || (if ($command/@guide) then ' · [' || $package/@for || ' guide](' || f:md-url($command/@guide) || ')' else '') || ')'"/>
  </xsl:function>

  <xsl:function name="f:command" as="xs:string">
    <xsl:param name="command" as="element(s:command)"/>
    <xsl:sequence select="replace(normalize-space($command), '\{version\}', f:releases(f:package($command))[1]?version)"/>
  </xsl:function>

  <!-- Actions: one line of links, as the HTML's row of buttons. -->
  <xsl:template match="s:actions" mode="md" as="xs:string">
    <xsl:sequence select="string-join(s:action ! ('[' || f:link-text(.) || '](' || f:md-href(.) || ')'), ' · ')"/>
  </xsl:template>

  <xsl:template match="s:versions" mode="md" as="xs:string">
    <xsl:sequence select="string-join($site/s:packages/s:package ! (let $release := f:releases(.)[1] return
                            '- [' || normalize-space(.) || ' ' || $release?version || '](#' || f:release-id(., $release) || ')'), $nl)"/>
  </xsl:template>

  <!-- Every package's releases, each its hand-written part: one heading a
       package, then each release under an anchor of the HTML's id. -->
  <xsl:template match="s:changelog" mode="md" as="xs:string">
    <xsl:variable name="changelog" select="."/>
    <xsl:variable name="blocks" as="xs:string*">
      <xsl:for-each select="$site/s:packages/s:package">
        <xsl:variable name="package" select="."/>
        <xsl:sequence select="'## ' || normalize-space(.) || ' (' || @for || ')'"/>
        <xsl:for-each select="f:releases($package)">
          <xsl:variable name="body" select="?lines ! replace(., '^### ', '#### ') ! f:md-repo-links(.)"/>
          <xsl:sequence select="'&lt;a id=&quot;' || f:release-id($package, .) || '&quot;&gt;&lt;/a&gt;' || $blank
                                || '### ' || normalize-space($package) || ' ' || ?version || ', ' || f:date(?date) || $blank
                                || replace(replace(string-join($body, $nl), '^\s+|\s+$', ''), '\n{3,}', $blank) || $blank
                                || '[' || normalize-space($changelog/s:more) || '](' || f:release-href($package, .) || ')'"/>
        </xsl:for-each>
      </xsl:for-each>
    </xsl:variable>
    <xsl:sequence select="string-join($blocks, $blank)"/>
  </xsl:template>

  <xsl:template match="*" mode="md">
    <xsl:message terminate="yes">site: {f:where(.)}: no Markdown for a &lt;{local-name()}&gt; in a &lt;{local-name(..)}&gt; (render/markdown.xsl, mode "md")</xsl:message>
  </xsl:template>

  <!-- ===================================================== inline -->

  <!-- An element's inline content as one line of Markdown. -->
  <xsl:function name="f:md-inline" as="xs:string">
    <xsl:param name="node" as="element()"/>
    <xsl:variable name="parts" as="xs:string*">
      <xsl:apply-templates select="$node/node()" mode="md-inline"/>
    </xsl:variable>
    <xsl:sequence select="normalize-space(string-join($parts))"/>
  </xsl:function>

  <xsl:template match="text()" mode="md-inline" as="xs:string">
    <xsl:sequence select="replace(., '\s+', ' ')"/>
  </xsl:template>

  <xsl:template match="s:em" mode="md-inline" as="xs:string">
    <xsl:sequence select="'*' || f:md-inline(.) || '*'"/>
  </xsl:template>

  <xsl:template match="s:strong" mode="md-inline" as="xs:string">
    <xsl:sequence select="'**' || f:md-inline(.) || '**'"/>
  </xsl:template>

  <xsl:template match="s:code" mode="md-inline" as="xs:string">
    <xsl:variable name="text" select="normalize-space(.)"/>
    <xsl:sequence select="if (contains($text, '`')) then '`` ' || $text || ' ``' else '`' || $text || '`'"/>
  </xsl:template>

  <xsl:template match="s:link" mode="md-inline" as="xs:string">
    <xsl:sequence select="'[' || f:link-text(.) || '](' || f:md-href(.) || ')'"/>
  </xsl:template>

  <xsl:template match="*" mode="md-inline">
    <xsl:message terminate="yes">site: {f:where(.)}: no Markdown for an inline &lt;{local-name()}&gt; (render/markdown.xsl, mode "md-inline")</xsl:message>
  </xsl:template>

  <!-- ===================================================== links -->

  <!-- A link's text: its own, or the title of the page it names. -->
  <xsl:function name="f:link-text" as="xs:string">
    <xsl:param name="link" as="element()"/>
    <xsl:sequence select="if (normalize-space($link) != '') then f:md-inline($link)
                          else normalize-space($pages[@id = $link/@to]/s:title)"/>
  </xsl:function>

  <!-- Where a link goes for an agent: a page of the site as its Markdown,
       a section as its anchor there, a path of the site absolute, another
       site's address as it is. -->
  <xsl:function name="f:md-href" as="xs:string">
    <xsl:param name="link" as="element()"/>
    <xsl:sequence select="if ($link/@href) then f:md-url($link/@href)
                          else $origin || f:md-path($pages[@id = $link/@to]/@path) || (if ($link/@section) then '#' || $link/@section else '')"/>
  </xsl:function>

  <!-- An address: a path of the site absolute, a page of the site
       (https://www.tinyactors.dev/x/) as its Markdown. -->
  <xsl:function name="f:md-url" as="xs:string">
    <xsl:param name="href" as="xs:string"/>
    <xsl:variable name="url" select="if (starts-with($href, '/')) then $origin || $href else $href"/>
    <xsl:variable name="path" select="substring-after(substring-before($url || '#', '#'), $origin)"/>
    <xsl:variable name="anchor" select="substring-after($url, '#')"/>
    <xsl:sequence select="if (starts-with($url, $origin || '/') and exists($md-pages[@path = $path]))
                          then $origin || f:md-path($path) || (if ($anchor != '') then '#' || $anchor else '')
                          else $url"/>
  </xsl:function>

  <!-- The HTML of the page and section an element is in. -->
  <xsl:function name="f:html-at" as="xs:string">
    <xsl:param name="node" as="element()"/>
    <xsl:sequence select="$origin || $node/ancestor::s:page/@path || ($node/ancestor::s:section/@id ! ('#' || .))"/>
  </xsl:function>

  <xsl:function name="f:release-id" as="xs:string">
    <xsl:param name="package" as="element(s:package)"/>
    <xsl:param name="release" as="map(*)"/>
    <xsl:sequence select="$package/@id || '-' || translate($release?version, '.', '-')"/>
  </xsl:function>

  <xsl:function name="f:md-changelog-href" as="xs:string">
    <xsl:param name="package" as="element(s:package)"/>
    <xsl:sequence select="f:md-url(f:changelog-href($package))"/>
  </xsl:function>
</xsl:stylesheet>
