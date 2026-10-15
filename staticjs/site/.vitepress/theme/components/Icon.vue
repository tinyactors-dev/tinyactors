<script setup lang="ts">
// One icon from Lucide (lucide.dev, ISC), drawn as the Tinyactors elements
// draw theirs: inline SVG, decorative, named by the control around it.
defineProps<{ name: 'play' | 'pause' | 'step-back' | 'step-forward' | 'pencil' | 'reset' | 'arrow-right' | 'check' | 'x' | 'chevron-right' | 'alert-triangle'; size?: number }>();
</script>

<template>
  <svg
    class="icon"
    :width="size ?? 16"
    :height="size ?? 16"
    viewBox="0 0 24 24"
    fill="none"
    stroke="currentColor"
    stroke-width="2"
    stroke-linecap="round"
    stroke-linejoin="round"
    aria-hidden="true"
    focusable="false"
  >
    <template v-if="name === 'play'"><polygon points="6 3 20 12 6 21 6 3" /></template>
    <template v-else-if="name === 'pause'"><rect x="14" y="3" width="5" height="18" rx="1" /><rect x="5" y="3" width="5" height="18" rx="1" /></template>
    <template v-else-if="name === 'step-back'"><line x1="18" x2="18" y1="20" y2="4" /><polygon points="14,20 4,12 14,4" /></template>
    <template v-else-if="name === 'step-forward'"><line x1="6" x2="6" y1="4" y2="20" /><polygon points="10,4 20,12 10,20" /></template>
    <template v-else-if="name === 'pencil'"><path d="M21.174 6.812a1 1 0 0 0-3.986-3.987L3.842 16.174a2 2 0 0 0-.5.83l-1.321 4.352a.5.5 0 0 0 .623.622l4.353-1.32a2 2 0 0 0 .83-.497z" /><path d="m15 5 4 4" /></template>
    <template v-else-if="name === 'reset'"><path d="M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8" /><path d="M3 3v5h5" /></template>
    <template v-else-if="name === 'arrow-right'"><path d="M5 12h14" /><path d="m12 5 7 7-7 7" /></template>
    <template v-else-if="name === 'check'"><path d="M20 6 9 17l-5-5" /></template>
    <template v-else-if="name === 'x'"><path d="M18 6 6 18" /><path d="m6 6 12 12" /></template>
    <template v-else-if="name === 'chevron-right'"><path d="m9 18 6-6-6-6" /></template>
    <template v-else-if="name === 'alert-triangle'"><path d="m21.73 18-8-14a2 2 0 0 0-3.48 0l-8 14A2 2 0 0 0 4 21h16a2 2 0 0 0 1.73-3" /><path d="M12 9v4" /><path d="M12 17h.01" /></template>
  </svg>
</template>

<style scoped>
.icon {
  display: block;
  flex: none;
}
</style>
