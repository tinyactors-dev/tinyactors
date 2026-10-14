import DefaultTheme from 'vitepress/theme';
import type { Theme } from 'vitepress';
import '@fontsource/ibm-plex-sans/400.css';
import '@fontsource/ibm-plex-sans/500.css';
import '@fontsource/ibm-plex-sans/600.css';
import '@fontsource/ibm-plex-sans/400-italic.css';
import '@fontsource/ibm-plex-mono/400.css';
import '@fontsource/ibm-plex-mono/500.css';
import '@fontsource/newsreader/400.css';
import '@fontsource/newsreader/400-italic.css';
import './style.css';
import LiveSample from './components/LiveSample.vue';
import PipelineExplorer from './components/PipelineExplorer.vue';
import HomePage from './components/HomePage.vue';

export default {
  extends: DefaultTheme,
  enhanceApp({ app }) {
    app.component('LiveSample', LiveSample);
    app.component('PipelineExplorer', PipelineExplorer);
    app.component('HomePage', HomePage);
  },
} satisfies Theme;
