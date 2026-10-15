<script setup lang="ts">
// A drafting-sheet frame: double rule, registration marks, an optional
// construction grid. A port of the Tinyactors design system's Sheet
// (tinyactors-dev/website, components/display/Sheet.jsx); its styles are
// .ta-sheet in ds-components.css.
withDefaults(defineProps<{ grid?: boolean; marks?: boolean }>(), { grid: false, marks: true });
</script>

<template>
  <div class="ta-sheet" :class="{ 'ta-sheet--grid': grid, 'ta-sheet--flush': !marks }">
    <template v-if="marks">
      <span v-for="p in ['tl', 'tr', 'bl', 'br']" :key="p" class="ta-sheet__reg" :class="`ta-sheet__reg--${p}`" aria-hidden="true"><i /></span>
    </template>
    <div class="ta-sheet__frame"><div class="ta-sheet__body"><slot /></div></div>
  </div>
</template>
