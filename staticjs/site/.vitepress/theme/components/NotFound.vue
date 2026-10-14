<script setup lang="ts">
// The 404 page: where the reader is, and the ways on.
import { useRoute } from 'vitepress';
const route = useRoute();
</script>

<template>
  <div class="not-found">
    <h1 class="section-title">No page here</h1>
    <p class="lede">Nothing lives at <code>{{ route.path }}</code>. It may have moved when the guide was reorganised.</p>
    <p class="ways">
      <a class="sjs-button primary" href="/guide/">The guide</a>
      <a class="sjs-button" href="/guide/reference">Quick reference</a>
      <a class="sjs-button" href="/">Home</a>
    </p>
    <p class="notice">Agents: every page is listed in <a href="/llms.txt">/llms.txt</a>.</p>
  </div>
</template>

<style scoped>
.not-found {
  max-width: 640px;
  margin: 0 auto;
  padding: 96px 24px 128px;
}
.ways {
  display: flex;
  flex-wrap: wrap;
  gap: var(--space-2);
  margin: 0 0 var(--space-8);
}
.notice {
  font: var(--type-body-sm);
  color: var(--fg-2);
}
.notice a {
  color: var(--text-link);
  text-decoration: underline;
}
</style>
